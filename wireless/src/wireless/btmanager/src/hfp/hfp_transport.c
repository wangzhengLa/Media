/*
* Copyright (c) 2018-2022 Allwinner Technology Co. Ltd. All rights reserved.
* Author: BT Team
* Date: 2022.03.12
* Description: alsa function about hfp.
*/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <alsa/asoundlib.h>

#include "bt_alsa.h"
#include "bt_log.h"
#include "hfp_transport.h"
#include "bt_pcm_supervise.h"
#include "bt_config_json.h"
#include "common.h"

#include "dbus-client.h"
#include "defs.h"
#include "ffb.h"

#define HFP_PCM_BUFF_TIME 200000
#define HFP_PCM_PERIOD_TIME 50000

static pthread_mutex_t pcm_open_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool hfp_hf_pcm_start = false;
enum interface_type_t hfp_if_type;

/*Mobile phone audio recording*/
static struct pcm_config phone_to_dev_cap = {
    .device = "hw:snddaudio1",
    .pcm = NULL,
    .stream = SND_PCM_STREAM_CAPTURE,
    .channels = 1,
    .sampling = 8000,
    .buffer_time = HFP_PCM_BUFF_TIME,
    .period_time = HFP_PCM_PERIOD_TIME,
    .format = SND_PCM_FORMAT_S16_LE,
    .drop = true,
};

/*Mobile phone audio play*/
static struct pcm_config phone_to_dev_play = {
    .device = "hw:audiocodec",
    .pcm = NULL,
    .stream = SND_PCM_STREAM_PLAYBACK,
    .channels = 1,
    .sampling = 8000,
    .buffer_time = HFP_PCM_BUFF_TIME,
    .period_time = HFP_PCM_PERIOD_TIME,
    .format = SND_PCM_FORMAT_S16_LE,
    .drop = true,
};

/*device recording*/
static struct pcm_config dev_to_phone_cap = {
    .device = "hw:audiocodec",
    .pcm = NULL,
    .stream = SND_PCM_STREAM_CAPTURE,
    .channels = 1,
    .sampling = 8000,
    .buffer_time = HFP_PCM_BUFF_TIME,
    .period_time = HFP_PCM_PERIOD_TIME,
    .format = SND_PCM_FORMAT_S16_LE,
    .drop = true,
};

/*device source write to mobile phone*/
static struct pcm_config dev_to_phone_play = {
    .device = "hw:snddaudio1",
    .pcm = NULL,
    .stream = SND_PCM_STREAM_PLAYBACK,
    .channels = 1,
    .sampling = 8000,
    .buffer_time = HFP_PCM_BUFF_TIME,
    .period_time = HFP_PCM_PERIOD_TIME,
    .format = SND_PCM_FORMAT_S16_LE,
    .drop = true,
};

static struct hfp_hf_worker PhoneStreamPlay = {
    .enable = false,
    .thread_flag = PTHONE_STREAM_PLAY,
    .thread_loop = false,
    .read_pcm = &phone_to_dev_cap,
    .write_pcm = &phone_to_dev_play,
    .read_write_frams = 128,
};

static struct hfp_hf_worker DevStreamRecord = {
    .enable = false,
    .thread_flag = DEV_STREAM_RECORD,
    .thread_loop = false,
    .read_pcm = &dev_to_phone_cap,
    .write_pcm = &dev_to_phone_play,
    .read_write_frams = 128,
};

static void aw_pcm_tansport_routine_data_free(void *arg)
{
    BTMG_DEBUG("enter");

    if (arg != NULL)
        free(arg);
}

static void aw_pcm_tansport_routine_device_free(void *arg)
{
    struct hfp_hf_worker *worker = (struct hfp_hf_worker *)arg;

    BTMG_DEBUG("enter,id:%d", worker->thread_flag);

    if (worker->read_pcm->pcm) {
        aw_pcm_free(worker->read_pcm);
        worker->read_pcm->pcm = NULL;
    }

    if (worker->write_pcm->pcm) {
        aw_pcm_free(worker->write_pcm);
        worker->write_pcm->pcm = NULL;
    }

    worker->enable = false;

    BTMG_DEBUG("quit,id:%d", worker->thread_flag);
}

static void *aw_pcm_transport_rontine(void *arg)
{
    char *read_buffer_ptr;
    int frams;
    int ac_frams = -1;
    static bool pcm_dump = false;
    struct hfp_hf_worker *worker = (struct hfp_hf_worker *)arg;
    static int fd = -1;

    BTMG_DEBUG("enter,id:%d", worker->thread_flag);

    worker->enable = true;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

    read_buffer_ptr = (char *)malloc(worker->read_write_frams * 2 * worker->read_pcm->channels);
    if (read_buffer_ptr == NULL) {
        BTMG_ERROR("malloc failed:%s", strerror(errno));
        return NULL;
    }

    pthread_cleanup_push(aw_pcm_tansport_routine_data_free, read_buffer_ptr);
    pthread_cleanup_push(aw_pcm_tansport_routine_device_free, worker);

    BTMG_INFO("opening read pcm device:%s", worker->read_pcm->device);
    if (aw_pcm_open(worker->read_pcm) < 0) {
        BTMG_ERROR("open read pcm failed");
        goto failed;
    }
    BTMG_INFO("opening write pcm device:%s", worker->write_pcm->device);
    if (aw_pcm_open(worker->write_pcm) < 0) {
        BTMG_ERROR("open write pcm failed");
        goto failed;
    }

    while (worker->thread_loop) {
        pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
        frams = aw_pcm_read(worker->read_pcm->pcm, read_buffer_ptr, worker->read_write_frams);
        if (frams < 0) {
            BTMG_ERROR("read pcm data null");
            ms_sleep(100);
            continue;
        }
        if (btmg_ex_debug_mask & EX_DBG_HFP_HF_DUMP_Capture) {
            pcm_dump = true;
            btmg_ex_debug_mask &= ~EX_DBG_HFP_HF_DUMP_Capture;
        }
        if (pcm_dump == true) {
            if (bt_alsa_dump_pcm(&fd, "/tmp/hfp_cap.raw", read_buffer_ptr,
                                 frams * worker->read_pcm->channels * 2, (1024 * 1024 * 8)) == -1) {
                pcm_dump = false;
            }
        }

        ac_frams = aw_pcm_write(worker->write_pcm->pcm, read_buffer_ptr, frams);
        if (ac_frams == -EPIPE)
            usleep(50000);
    }

failed:
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    pthread_cleanup_pop(1);
    pthread_cleanup_pop(1);
    return NULL;
}

int aw_pcm_tansport_start(struct hfp_hf_worker *worker)
{
    BTMG_DEBUG("enter,id:%d", worker->thread_flag);

    if (worker->enable)
        return BT_OK;

    BTMG_INFO("device:%s,%s;chanels:%d,sampling:%d", worker->read_pcm->device,
              worker->write_pcm->device, worker->read_pcm->channels, worker->read_pcm->sampling);

    worker->thread_loop = true;

    if (pthread_create(&worker->thread, NULL, aw_pcm_transport_rontine, worker) != 0) {
        BTMG_WARNG("Couldn't create PCM worker %s: ", strerror(errno));
    }

    return BT_OK;
}

int aw_pcm_transport_stop(struct hfp_hf_worker *worker)
{
    void *tret;
    int err;

    if (!worker->enable)
        return BT_OK;

    BTMG_DEBUG("enter,id:%d", worker->thread_flag);

    worker->thread_loop = false;

    if (pthread_cancel(worker->thread) != 0) {
        BTMG_ERROR("hfp worker rountine thread exit failed:%s", strerror(errno));
        return BT_ERROR;
    }

    err = pthread_join(worker->thread, &tret);

    if (err != 0) {
        BTMG_DEBUG("Can't join with thread ");
    }
    BTMG_DEBUG("quit,id:%d", worker->thread_flag);
    return BT_OK;
}

int bt_hfp_read_config(void)
{
    struct hfp_pcm pcm_config;

    BTMG_DEBUG("enter");
    if (bt_config_read_hfp(&pcm_config) != 0)
        return BT_ERROR;

    phone_to_dev_cap.sampling = pcm_config.rate;
    phone_to_dev_play.sampling = pcm_config.rate;
    dev_to_phone_cap.sampling = pcm_config.rate;
    dev_to_phone_play.sampling = pcm_config.rate;

    memset(phone_to_dev_cap.device, 0, sizeof(phone_to_dev_cap.device));
    strncpy(phone_to_dev_cap.device, pcm_config.phone_to_dev_cap,
            strlen(pcm_config.phone_to_dev_cap));

    memset(phone_to_dev_play.device, 0, sizeof(phone_to_dev_play.device));
    strncpy(phone_to_dev_play.device, pcm_config.phone_to_dev_play,
            strlen(pcm_config.phone_to_dev_play));

    memset(dev_to_phone_cap.device, 0, sizeof(dev_to_phone_cap.device));
    strncpy(dev_to_phone_cap.device, pcm_config.dev_to_phone_cap,
            strlen(pcm_config.dev_to_phone_cap));

    memset(dev_to_phone_play.device, 0, sizeof(dev_to_phone_play.device));
    strncpy(dev_to_phone_play.device, pcm_config.dev_to_phone_play,
            strlen(pcm_config.dev_to_phone_play));

    if (strcmp(pcm_config.interface_type, "uart") == 0) {
        hfp_if_type = UART_TYPE;
    } else if (strcmp(pcm_config.interface_type, "pcm") == 0) {
        hfp_if_type = PCM_TYPE;
    } else {
        BTMG_WARNG("interface_type unknow");
        hfp_if_type = PCM_TYPE;
    }

    return BT_OK;
}

void bt_hfp_hf_pcm_start(void)
{
    BTMG_DEBUG("enter");

    pthread_mutex_lock(&pcm_open_mutex);
    if (hfp_hf_pcm_start == true) {
        pthread_mutex_unlock(&pcm_open_mutex);
        BTMG_WARNG("hfp pcm already open");
        return;
    }
    hfp_hf_pcm_start = true;

    if (!PhoneStreamPlay.enable)
        aw_pcm_tansport_start(&PhoneStreamPlay);

    if (!DevStreamRecord.enable)
        aw_pcm_tansport_start(&DevStreamRecord);

    pthread_mutex_unlock(&pcm_open_mutex);

    if (btmg_cb_p && btmg_cb_p->btmg_hfp_cb.hfp_hf_connection_state_cb)
        btmg_cb_p->btmg_hfp_cb.hfp_hf_connection_state_cb(BTMG_HFP_HF_SCO_LINK_CONNECTED);
    if (btmg_cb_p && btmg_cb_p->btmg_hfp_cb.hfp_ag_connection_state_cb)
        btmg_cb_p->btmg_hfp_cb.hfp_ag_connection_state_cb(BTMG_HFP_AG_SCO_LINK_CONNECTED);
}

void bt_hfp_hf_pcm_stop(void)
{
    BTMG_DEBUG("enter");

    pthread_mutex_lock(&pcm_open_mutex);
    if (hfp_hf_pcm_start == false) {
        pthread_mutex_unlock(&pcm_open_mutex);
        BTMG_WARNG("hfp pcm already stop");
        return;
    }
    hfp_hf_pcm_start = false;

    if (PhoneStreamPlay.enable)
        aw_pcm_transport_stop(&PhoneStreamPlay);

    if (DevStreamRecord.enable)
        aw_pcm_transport_stop(&DevStreamRecord);

    pthread_mutex_unlock(&pcm_open_mutex);

    if (btmg_cb_p && btmg_cb_p->btmg_hfp_cb.hfp_hf_connection_state_cb)
        btmg_cb_p->btmg_hfp_cb.hfp_hf_connection_state_cb(BTMG_HFP_HF_SCO_LINK_DISCONNECTED);
    if (btmg_cb_p && btmg_cb_p->btmg_hfp_cb.hfp_ag_connection_state_cb)
        btmg_cb_p->btmg_hfp_cb.hfp_ag_connection_state_cb(BTMG_HFP_AG_SCO_LINK_DISCONNECTED);
}

void update_pcm_device_info(char *addr)
{
    snprintf(DevStreamRecord.write_pcm->device, 43, "bluealsa:DEV=%s,PROFILE=sco", addr);
    snprintf(PhoneStreamPlay.read_pcm->device, 43, "bluealsa:DEV=%s,PROFILE=sco", addr);
}
