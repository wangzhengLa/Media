/*
* Copyright (c) 2018-2022 Allwinner Technology Co. Ltd. All rights reserved.
* Author: BT Team
* Date: 2023.07.04
*/

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <dbus/dbus.h>
#include <sys/prctl.h>

#include "bt_log.h"
#include "bt_pcm_supervise.h"
#include "defs.h"
#include "dbus-client.h"
#include "hfp_transport.h"
#include "bt_a2dp_sink.h"
#include "hfp_rfcomm.h"
#include "common.h"
#include "dbus-client.h"

static pthread_t bluealsa_pcm_supervise_thread;
static pthread_t rfcomm_thread;
bool rfcomm_workers_enable;

struct ba_dbus_ctx dbus_ctx;
static char dbus_ba_service[32] = BLUEALSA_SERVICE;

static struct ba_pcm *ba_pcms = NULL;
static size_t ba_pcms_count = 0;

static bool main_loop_on = true;
static bool ba_addr_any = true;

static pthread_rwlock_t workers_lock = PTHREAD_RWLOCK_INITIALIZER;
static struct pcm_worker *workers = NULL;
static size_t workers_count = 0;
static size_t workers_size = 0;


snd_pcm_format_t get_snd_pcm_format(const struct ba_pcm *pcm)
{
    switch (pcm->format) {
    case 0x0108:
        return SND_PCM_FORMAT_U8;
    case 0x8210:
        return SND_PCM_FORMAT_S16_LE;
    case 0x8318:
        return SND_PCM_FORMAT_S24_3LE;
    case 0x8418:
        return SND_PCM_FORMAT_S24_LE;
    case 0x8420:
        return SND_PCM_FORMAT_S32_LE;
    default:
        BTMG_ERROR("Unknown PCM format: %#x", pcm->format);
        return SND_PCM_FORMAT_UNKNOWN;
    }
}

struct pcm_worker *get_active_worker(void)
{
    size_t i;
    struct pcm_worker *w = NULL;

    pthread_rwlock_rdlock(&workers_lock);

    for (i = 0; i < workers_count; i++)
        if (workers[i].active) {
            w = &workers[i];
            break;
        }

    pthread_rwlock_unlock(&workers_lock);

    return w;
}

static struct ba_pcm *get_ba_pcm(const char *path)
{
    size_t i;

    for (i = 0; i < ba_pcms_count; i++)
        if (strcmp(ba_pcms[i].pcm_path, path) == 0)
            return &ba_pcms[i];

    return NULL;
}

static int supervise_pcm_worker_start(struct ba_pcm *ba_pcm)
{
    size_t i;

    BTMG_DEBUG("enter");
    for (i = 0; i < workers_count; i++) {
        BTMG_DEBUG("w-pcm_path:%s,b-pcm_path:%s", workers[i].ba_pcm.pcm_path, ba_pcm->pcm_path);
        if (strcmp(workers[i].ba_pcm.pcm_path, ba_pcm->pcm_path) == 0) {
            BTMG_WARNG("%s already started", ba_pcm->pcm_path);
            return BT_OK;
        }
    }
    pthread_rwlock_wrlock(&workers_lock);

    workers_count++;
    if (workers_size < workers_count) {
        struct pcm_worker *tmp = workers;
        workers_size += 4; /* coarse-grained realloc */
        if ((workers = realloc(workers, sizeof(*workers) * workers_size)) == NULL) {
            BTMG_ERROR("Couldn't (re)allocate memory for PCM workers: %s", strerror(ENOMEM));
            workers = tmp;
            pthread_rwlock_unlock(&workers_lock);
            return BT_ERROR_NO_MEMORY;
        }
    }

    struct pcm_worker *worker = &workers[workers_count - 1];
    memcpy(&worker->ba_pcm, ba_pcm, sizeof(worker->ba_pcm));
    ba2str(&worker->ba_pcm.addr, worker->addr);
    worker->active = false;
    worker->ba_pcm_fd = -1;
    worker->ba_pcm_ctrl_fd = -1;

    pthread_rwlock_unlock(&workers_lock);

    BTMG_DEBUG("Creating PCM worker %s", worker->addr);

    if (ba_pcm->transport & BA_PCM_TRANSPORT_MASK_A2DP) {
        BTMG_INFO("transport A2DP:%s", ba_pcm->codec);
        if ((errno = pthread_create(&worker->thread, NULL, PTHREAD_ROUTINE(a2dp_pcm_worker_routine),
                                    worker)) != 0) {
            BTMG_ERROR("Couldn't create PCM worker %s: %s", worker->addr, strerror(errno));
            workers_count--;
            return BT_ERROR;
        }
    } else if ((ba_pcm->transport & BA_PCM_TRANSPORT_MASK_SCO) && (ba_pcm->mode & BA_PCM_MODE_SOURCE)) {
        BTMG_INFO("transport SCO:%s", ba_pcm->codec);
        if (hfp_if_type == UART_TYPE) {
            update_pcm_device_info(worker->addr);
        }
        workers_count--;

        BTMG_DEBUG("create rfcomm thread");
        if ((errno = pthread_create(&rfcomm_thread, NULL, PTHREAD_ROUTINE(rfcomm_thread_main),
                                    worker)) != 0) {
            BTMG_ERROR("Couldn't create rfcomm thread %s: %s", worker->addr, strerror(errno));
            return BT_ERROR;
        }
        rfcomm_workers_enable = true;

        if (btmg_cb_p && btmg_cb_p->btmg_hfp_cb.hfp_hf_connection_state_cb)
            btmg_cb_p->btmg_hfp_cb.hfp_hf_connection_state_cb(BTMG_HFP_HF_CONNECTED);
        if (btmg_cb_p && btmg_cb_p->btmg_hfp_cb.hfp_ag_connection_state_cb)
            btmg_cb_p->btmg_hfp_cb.hfp_ag_connection_state_cb(BTMG_HFP_AG_CONNECTED);
    }

    return BT_OK;
}

static int supervise_pcm_worker_stop(struct ba_pcm *ba_pcm)
{
    size_t i;

    BTMG_DEBUG("enter");

    for (i = 0; i < workers_count; i++)
        if (strcmp(workers[i].ba_pcm.pcm_path, ba_pcm->pcm_path) == 0) {
            pthread_rwlock_wrlock(&workers_lock);
            pthread_cancel(workers[i].thread);
            pthread_join(workers[i].thread, NULL);
            workers[i].thread_enable = false;
            memcpy(&workers[i], &workers[--workers_count], sizeof(workers[i]));
            pthread_rwlock_unlock(&workers_lock);
        }

    if ((ba_pcm->transport & BA_PCM_TRANSPORT_MASK_SCO) && (ba_pcm->mode & BA_PCM_MODE_SOURCE) && rfcomm_workers_enable) {
        bt_hfp_hf_pcm_stop();
        pthread_rwlock_wrlock(&workers_lock);
        pthread_cancel(rfcomm_thread);
        pthread_join(rfcomm_thread, NULL);
        rfcomm_workers_enable = false;
        pthread_rwlock_unlock(&workers_lock);
        if (btmg_cb_p && btmg_cb_p->btmg_hfp_cb.hfp_hf_connection_state_cb)
            btmg_cb_p->btmg_hfp_cb.hfp_hf_connection_state_cb(BTMG_HFP_HF_DISCONNECTED);
        if (btmg_cb_p && btmg_cb_p->btmg_hfp_cb.hfp_ag_connection_state_cb)
            btmg_cb_p->btmg_hfp_cb.hfp_ag_connection_state_cb(BTMG_HFP_AG_DISCONNECTED);
    }

    BTMG_DEBUG("quit");
    return BT_OK;
}

static int supervise_pcm_worker(struct ba_pcm *ba_pcm)
{
    BTMG_DEBUG("enter");

    if (ba_pcm == NULL)
        return BT_ERROR_INVALID_ARGS;

    BTMG_DEBUG("ba_pcm mode: %d, ba_pcm transport: %d", ba_pcm->mode, ba_pcm->transport);
    if (!(ba_pcm->mode & BA_PCM_MODE_SOURCE))
        goto stop;

    /* check whether SCO has selected codec */
    if (ba_pcm->transport & BA_PCM_TRANSPORT_MASK_SCO && ba_pcm->sampling == 0) {
        BTMG_DEBUG("Skipping SCO with codec not selected");
        goto stop;
    }

    if (ba_addr_any)
        goto start;

stop:
    return supervise_pcm_worker_stop(ba_pcm);
start:
    return supervise_pcm_worker_start(ba_pcm);
}

static DBusHandlerResult dbus_signal_handler(DBusConnection *conn, DBusMessage *message, void *data)
{
    (void)conn;
    (void)data;

    if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_SIGNAL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    const char *path = dbus_message_get_path(message);
    const char *interface = dbus_message_get_interface(message);
    const char *signal = dbus_message_get_member(message);

    DBusMessageIter iter;
    struct pcm_worker *worker;

    if (strcmp(interface, DBUS_INTERFACE_OBJECT_MANAGER) == 0) {
        if (strcmp(signal, "InterfacesAdded") == 0) {
            if (!dbus_message_iter_init(message, &iter))
                goto fail;
            struct ba_pcm pcm;
            DBusError err = DBUS_ERROR_INIT;
            if (!bluealsa_dbus_message_iter_get_pcm(&iter, &err, &pcm)) {
                BTMG_ERROR("Couldn't add new PCM: %s", err.message);
                dbus_error_free(&err);
                goto fail;
            }
            if (pcm.transport == BA_PCM_TRANSPORT_NONE)
                goto fail;
            struct ba_pcm *tmp = ba_pcms;
            if ((ba_pcms = realloc(ba_pcms, (ba_pcms_count + 1) * sizeof(*ba_pcms))) == NULL) {
                BTMG_ERROR("Couldn't add new PCM: %s", strerror(ENOMEM));
                ba_pcms = tmp;
                goto fail;
            }
            memcpy(&ba_pcms[ba_pcms_count++], &pcm, sizeof(*ba_pcms));
            BTMG_DEBUG("Interfaces Added");
            supervise_pcm_worker(&pcm);
            return DBUS_HANDLER_RESULT_HANDLED;
        }

        if (strcmp(signal, "InterfacesRemoved") == 0) {
            if (!dbus_message_iter_init(message, &iter) ||
                dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_OBJECT_PATH) {
                BTMG_ERROR("Couldn't remove PCM: %s", "Invalid signal signature");
                goto fail;
            }
            dbus_message_iter_get_basic(&iter, &path);
            struct ba_pcm *pcm;
            if ((pcm = get_ba_pcm(path)) == NULL)
                goto fail;
            BTMG_DEBUG("Interfaces Removed");
            supervise_pcm_worker_stop(pcm);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
    }

    if (strcmp(interface, DBUS_INTERFACE_PROPERTIES) == 0) {
        struct ba_pcm *pcm;
        if ((pcm = get_ba_pcm(path)) == NULL)
            goto fail;
        if (!dbus_message_iter_init(message, &iter) ||
            dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING) {
            BTMG_ERROR("Couldn't update PCM: %s", "Invalid signal signature");
            goto fail;
        }
        dbus_message_iter_get_basic(&iter, &interface);
        dbus_message_iter_next(&iter);
        if (!bluealsa_dbus_message_iter_get_pcm_props(&iter, NULL, pcm))
            goto fail;

        return DBUS_HANDLER_RESULT_HANDLED;
    }

fail:
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void *bluealsa_pcm_supervise_process(void *arg)
{
    BTMG_DEBUG("enter");

    if (prctl(PR_SET_NAME, (unsigned long)"bluealsa_pcm_supervise_thread") == -1) {
        BTMG_ERROR("unable to set bluealsa pcm supervise thread name: %s", strerror(errno));
    }
    dbus_threads_init_default();
    DBusError err = DBUS_ERROR_INIT;
    if (!bluealsa_dbus_connection_ctx_init(&dbus_ctx, dbus_ba_service, &err)) {
        BTMG_ERROR("Couldn't initialize D-Bus context: %s", err.message);
        return NULL;
    }
    bluealsa_dbus_connection_signal_match_add(&dbus_ctx, dbus_ba_service, NULL,
                                              DBUS_INTERFACE_OBJECT_MANAGER, "InterfacesAdded",
                                              "path_namespace='/org/bluealsa'");
    bluealsa_dbus_connection_signal_match_add(&dbus_ctx, dbus_ba_service, NULL,
                                              DBUS_INTERFACE_OBJECT_MANAGER, "InterfacesRemoved",
                                              "path_namespace='/org/bluealsa'");
    bluealsa_dbus_connection_signal_match_add(&dbus_ctx, dbus_ba_service, NULL,
                                              DBUS_INTERFACE_PROPERTIES, "PropertiesChanged",
                                              "arg0='org.bluealsa.PCM1'");

    if (!dbus_connection_add_filter(dbus_ctx.conn, dbus_signal_handler, NULL, NULL)) {
        BTMG_ERROR("Couldn't add D-Bus filter: %s", err.message);
        return NULL;
    }
    if (!bluealsa_dbus_get_pcms(&dbus_ctx, &ba_pcms, &ba_pcms_count, &err))
        BTMG_WARNG("Couldn't get BlueALSA PCM list: %s", err.message);

    size_t i;
    for (i = 0; i < ba_pcms_count; i++)
        supervise_pcm_worker(&ba_pcms[i]);

    BTMG_DEBUG("Starting main loop");
    main_loop_on = true;
    while (main_loop_on) {
        struct pollfd pfds[10];
        nfds_t pfds_len = ARRAYSIZE(pfds);

        if (!bluealsa_dbus_connection_poll_fds(&dbus_ctx, pfds, &pfds_len)) {
            BTMG_ERROR("Couldn't get D-Bus connection file descriptors");
            return NULL;
        }

        if (poll(pfds, pfds_len, -1) == -1 && errno == EINTR)
            continue;

        if (bluealsa_dbus_connection_poll_dispatch(&dbus_ctx, pfds, pfds_len))
            while (dbus_connection_dispatch(dbus_ctx.conn) == DBUS_DISPATCH_DATA_REMAINS)
                continue;
    }
    BTMG_DEBUG("main loop quit");

    return NULL;
}

int bluealsa_pcm_supervise_init(void)
{
    int ret = -1;
    if ((ret = pthread_create(&bluealsa_pcm_supervise_thread, NULL, bluealsa_pcm_supervise_process, NULL)) != 0) {
        BTMG_ERROR("Couldn't create a2dp sink process thread.");
        goto fail;
    }

    return BT_OK;
fail:
    return BT_ERROR;
}

int bluealsa_pcm_supervise_deinit(void)
{
    int i;
    main_loop_on = false;
    BTMG_DEBUG("close pcm worker thread.");
    for (i = 0; i < workers_count; i++) {
            pthread_rwlock_wrlock(&workers_lock);
            pthread_cancel(workers[i].thread);
            pthread_join(workers[i].thread, NULL);
            workers[i].thread_enable = false;
            memcpy(&workers[i], &workers[--workers_count], sizeof(workers[i]));
            pthread_rwlock_unlock(&workers_lock);
    }

    if (rfcomm_workers_enable) {
        BTMG_DEBUG("close rfcomm thread.");
        bt_hfp_hf_pcm_stop();
        pthread_rwlock_wrlock(&workers_lock);
        pthread_cancel(rfcomm_thread);
        pthread_join(rfcomm_thread, NULL);
        rfcomm_workers_enable = false;
        pthread_rwlock_unlock(&workers_lock);
        if (btmg_cb_p && btmg_cb_p->btmg_hfp_cb.hfp_hf_connection_state_cb)
            btmg_cb_p->btmg_hfp_cb.hfp_hf_connection_state_cb(BTMG_HFP_HF_DISCONNECTED);
        if (btmg_cb_p && btmg_cb_p->btmg_hfp_cb.hfp_ag_connection_state_cb)
            btmg_cb_p->btmg_hfp_cb.hfp_ag_connection_state_cb(BTMG_HFP_AG_DISCONNECTED);
    }

    BTMG_DEBUG("close bluealsa pcm supervise thread.");
    pthread_cancel(bluealsa_pcm_supervise_thread);
    pthread_join(bluealsa_pcm_supervise_thread, NULL);
    BTMG_DEBUG("complete.");
    return BT_OK;
}
