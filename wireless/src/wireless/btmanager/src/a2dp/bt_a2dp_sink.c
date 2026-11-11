/*
* Copyright (c) 2018-2022 Allwinner Technology Co. Ltd. All rights reserved.
* Author: BT Team
* Date: 2022.03.12
* Description: Create A2DP Sink function.
*/

#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <bluetooth/bluetooth.h>
#include <dbus/dbus.h>
#include <sys/prctl.h>

#include "bt_log.h"
#include "bt_alsa.h"
#include "bt_alarm.h"
#include "transmit.h"
#include "bt_a2dp_sink.h"
#include "bt_config_json.h"
#include "bt_bluez_signals.h"
#include "common.h"
#include "bt_pcm_supervise.h"

#include "dbus-client.h"
#include "defs.h"
#include "ffb.h"

#define CACHE_TIMEOUT 500
#define A2DP_SINK_COMSUMER_DATA_LEN 1920
static bool a2dp_stream_enable = false;
static pthread_mutex_t a2dp_sink_stream_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct pcm_config a2dp_sink_pcm = {
    .device = "default",
    .pcm = NULL,
    .stream = SND_PCM_STREAM_PLAYBACK,
    .channels = 2,
    .sampling = 44100,
    .buffer_time = 400000,
    .period_time = 100000,
    .format = SND_PCM_FORMAT_S16_LE,
};

static size_t pcm_max_read_len;
static bool main_loop_on = true;
static bool media_status = false;
static bool a2dp_sink_stream_cb = false;
static transmit_t *a2dp_sink_ts = NULL;
static struct pcm_config *a2dp_sink_pcm_pf = NULL;
static size_t pcm_open_retries;
static struct bt_a2dp_sink_cf sink_cf;

static int a2dp_sink_push_data(char *data, uint32_t len);
static int a2dp_sink_stream_stop(void);
static int a2dp_sink_stream_start(uint8_t channels, uint32_t sampling);
static int a2dp_sink_pcm_write(void *handle, char *buff, uint32_t *len);

static void media_change_callback(bt_media_state_t state)
{
    pthread_mutex_lock(&a2dp_sink_stream_mutex);
    if (state == BT_MEDIA_STOP || state == BT_MEDIA_PAUSE) {
        BTMG_DEBUG("a2dp sink stream stop.");
        media_status = false;
        a2dp_sink_stream_stop();
    } else if (state == BT_MEDIA_PLAYING) {
        BTMG_DEBUG("a2dp sink stream start.");
        media_status = true;
        a2dp_sink_stream_start(a2dp_sink_pcm_pf->channels, a2dp_sink_pcm_pf->sampling);
    }
    pthread_mutex_unlock(&a2dp_sink_stream_mutex);
}

static void a2dp_pcm_worker_routine_exit(struct pcm_worker *worker)
{
    if (worker->ba_pcm_fd != -1) {
        close(worker->ba_pcm_fd);
        worker->ba_pcm_fd = -1;
    }
    if (worker->ba_pcm_ctrl_fd != -1) {
        close(worker->ba_pcm_ctrl_fd);
        worker->ba_pcm_ctrl_fd = -1;
    }

    if (a2dp_sink_stream_cb == false) {
        a2dp_sink_stream_stop();
    }
    BTMG_INFO("Exiting PCM worker %s", worker->addr);
}

void *a2dp_pcm_worker_routine(struct pcm_worker *w)
{
    bool a2dp_stream_enable = false;
    bool pcm_dump = false;
    static int pcm_dump_fd = -1;
    ffb_t buffer = { 0 };
    snd_pcm_format_t pcm_format = get_snd_pcm_format(&w->ba_pcm);
    ssize_t pcm_format_size = snd_pcm_format_size(pcm_format, 1);
    size_t pcm_1s_samples = w->ba_pcm.sampling * w->ba_pcm.channels;

    a2dp_sink_pcm_pf->sampling = w->ba_pcm.sampling;
    a2dp_sink_pcm_pf->channels = w->ba_pcm.channels;
    a2dp_sink_pcm_pf->format = pcm_format;
    a2dp_sink_pcm_pf->format_size = pcm_format_size;

    BTMG_INFO("codec:%s, sampling:%d, channels:%d, format_size:%d", w->ba_pcm.codec,
                w->ba_pcm.sampling, w->ba_pcm.channels, pcm_format_size);

    if (prctl(PR_SET_NAME, (unsigned long)"sink_pcm") == -1) {
        BTMG_ERROR("unable to set sink pcm thread name: %s", strerror(errno));
    }

    /* Cancellation should be possible only in the carefully selected place
	 * in order to prevent memory leaks and resources not being released. */
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    pthread_cleanup_push(PTHREAD_CLEANUP(a2dp_pcm_worker_routine_exit), w);
    pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &buffer);

    /* create buffer big enough to hold 100 ms of PCM data */
    if (ffb_init(&buffer, pcm_1s_samples / 10, pcm_format_size) == -1) {
        BTMG_ERROR("Couldn't create PCM buffer: %s", strerror(errno));
        goto fail;
    }

    DBusError err = DBUS_ERROR_INIT;
    if (!bluealsa_dbus_pcm_open(&dbus_ctx, w->ba_pcm.pcm_path, &w->ba_pcm_fd, &w->ba_pcm_ctrl_fd,
                                &err)) {
        BTMG_ERROR("Couldn't open PCM: %s", err.message);
        dbus_error_free(&err);
        goto fail;
    }

    /* Initialize the max read length to 10 ms. Later, when the PCM device
	 * will be opened, this value will be adjusted to one period size. */
    size_t pcm_max_read_len_init = pcm_1s_samples / 100 * pcm_format_size;
    pcm_max_read_len = pcm_max_read_len_init;

    struct pollfd pfds[] = { { w->ba_pcm_fd, POLLIN, 0 } };
    int timeout = -1;

    BTMG_INFO("Starting PCM loop");
    w->thread_enable = true;
    main_loop_on = true;
    while (main_loop_on) {
        pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

        ssize_t ret;
        /* Reading from the FIFO won't block unless there is an open connection
		 * on the writing side. However, the server does not open PCM FIFO until
		 * a transport is created. With the A2DP, the transport is created when
		 * some clients (BT device) requests audio transfer. */
        switch (poll(pfds, ARRAYSIZE(pfds), timeout)) {
        case -1:
            if (errno == EINTR)
                continue;
            BTMG_ERROR("PCM FIFO poll error: %s", strerror(errno));
            goto fail;
        case 0:
            BTMG_DEBUG("Device marked as inactive: %s", w->addr);
            // if (a2dp_sink_stream_cb == false && media_status == false) {
            //     a2dp_sink_stream_stop();
            // }
            pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
            pcm_max_read_len = pcm_max_read_len_init;
            ffb_rewind(&buffer);
            w->active = false;
            timeout = -1;
            continue;
        }

        /* FIFO has been terminated on the writing side */
        if (pfds[0].revents & POLLHUP)
            break;

#define MIN_S(a, b) a < b ? a : b
        size_t _in = MIN_S(pcm_max_read_len, ffb_blen_in(&buffer));
        if ((ret = read(w->ba_pcm_fd, buffer.tail, _in)) == -1) {
            if (errno == EINTR)
                continue;
            BTMG_ERROR("PCM FIFO read error: %s", strerror(errno));
            goto fail;
        }

        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

        /* mark device as active and set timeout to 700ms */
        w->active = true;
        timeout = 700;

        ffb_seek(&buffer, ret / pcm_format_size);
        int write_len = ffb_len_out(&buffer) * pcm_format_size;

        if (btmg_ex_debug_mask & EX_DBG_A2DP_SINK_DUMP_RB) {
            pcm_dump = true;
            btmg_ex_debug_mask &= ~EX_DBG_A2DP_SINK_DUMP_RB;
        }
        if (pcm_dump == true) {
            if (bt_alsa_dump_pcm(&pcm_dump_fd, "/tmp/a2dp_bluealsa.raw", buffer.data, write_len,
                                 (1024 * 1024 * 8)) == -1) {
                pcm_dump = false;
            }
        }

        if (media_status == false) {
            //drop pcm data
            ms_sleep(10);
        } else if (a2dp_sink_stream_cb == true) {
            if (a2dp_stream_enable == true) {
                a2dp_sink_stream_stop();
                ms_sleep(10);
            }
            if (a2dp_sink_pcm_pf && a2dp_sink_pcm_pf->pcm != NULL) {
                aw_pcm_free(a2dp_sink_pcm_pf);
            }
            if (btmg_cb_p && btmg_cb_p->btmg_a2dp_sink_cb.a2dp_sink_stream_cb) {
                btmg_cb_p->btmg_a2dp_sink_cb.a2dp_sink_stream_cb(
                        NULL, w->ba_pcm.channels, w->ba_pcm.sampling, (int)pcm_format, buffer.data, write_len);
            }
        } else {
            // ret = a2dp_sink_pcm_write(a2dp_sink_pcm_pf, buffer.data, &write_len);
            ret = a2dp_sink_push_data(buffer.data, write_len);
            if (ret == 0) {
                write_len = 0;
            }
        }
        ffb_shift(&buffer, write_len / pcm_format_size);
    }

fail:
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    pthread_cleanup_pop(1);
    pthread_cleanup_pop(1);
    return NULL;
}

static int a2dp_sink_pcm_write(void *handle, char *buff, uint32_t *len)
{
    struct pcm_config *pf = (struct pcm_config *)handle;
    snd_pcm_uframes_t residue;
    snd_pcm_uframes_t total;
    int ret = BT_OK;
    int ex_frames = -1;
    int ac_frames = -1;
    static int fd = -1;
    static bool pcm_dump_hw = false;

    if (pf == NULL || buff == NULL || len == NULL) {
        BTMG_ERROR("a2dp sink send data error.");
        return BT_ERROR_NULL_VARIABLE;
    }
    if (pf->pcm == NULL) {
        snd_pcm_uframes_t buffer_size;
        snd_pcm_uframes_t period_size;

        /* After PCM open failure wait one second before retry. This can not be
        * done with a single sleep() call, because we have to drain PCM FIFO. */
        if (pcm_open_retries++ % 20 != 0) {
            usleep(50000);
            return BT_ERROR_A2DP_PCM_OPEN_FAIL;
        }

        ret = aw_pcm_open(pf);
        if (ret < 0) {
            BTMG_ERROR("a2dp sink open pcm error:%s", snd_strerror(ret));
            usleep(50000);
            return BT_ERROR_A2DP_PCM_OPEN_FAIL;
        }
        pcm_open_retries = 0;
        snd_pcm_get_params(pf->pcm, &buffer_size, &period_size);
        pcm_max_read_len = period_size * pf->channels * pf->format;
    }
    ex_frames = *len / (pf->format_size * pf->channels);
    residue = ex_frames;
    total = ex_frames;
    while (residue > 0) {
        void *buf = buff + snd_pcm_frames_to_bytes(pf->pcm, total - residue);
        ac_frames = aw_pcm_write(pf->pcm, buf, residue);
        if (ac_frames >= 0) {
            residue -= ac_frames;
            continue;
        }
    }

    if (btmg_ex_debug_mask & EX_DBG_A2DP_SINK_RATE) {
        static int data_count = 0;
        int speed = 0;
        int time_ms;

        data_count += *len;
        time_ms = btmg_interval_time((void *)a2dp_sink_pcm_write, 2000);
        if (time_ms) {
            speed = data_count * 1000 / time_ms;
            BTMG_INFO("time_ms[%d] tot[%d] len[%d] ex_frames[%d] ac_frames[%d] speed[%d]", time_ms,
                      data_count, *len, ex_frames, ac_frames, speed);
            data_count = 0;
        }
    }

    if (btmg_ex_debug_mask & EX_DBG_A2DP_SINK_DUMP_HW) {
        pcm_dump_hw = true;
        btmg_ex_debug_mask &= ~EX_DBG_A2DP_SINK_DUMP_HW;
    }
    if (pcm_dump_hw == true) {
        if (bt_alsa_dump_pcm(&fd, "/tmp/a2dp_sink_hw.raw", buff, *len, (1024 * 1024 * 8)) == -1) {
            pcm_dump_hw = false;
        }
    }

    return (ac_frames * pf->format_size * pf->channels);
}

static int a2dp_sink_pcm_deinit(void *usr_data)
{
    BTMG_DEBUG("enter.");

    if (a2dp_sink_pcm_pf && a2dp_sink_pcm_pf->pcm != NULL) {
        aw_pcm_free(a2dp_sink_pcm_pf);
    }

    return BT_OK;
}

static int a2dp_sink_stream_start(uint8_t channels, uint32_t sampling)
{
    int len = 0;

    if (a2dp_stream_enable == true) {
        BTMG_INFO("already true.");
        return BT_OK;
    }
    a2dp_stream_enable = true;
    BTMG_DEBUG("enter");
    if (a2dp_sink_ts == NULL || a2dp_sink_pcm_pf == NULL) {
        BTMG_ERROR("a2dp sink has not been initialized");
        return BT_ERROR_A2DP_SINK_NOT_INIT;
    }

    len = A2DP_SINK_COMSUMER_DATA_LEN;
    uint32_t byte_s = channels * sampling * a2dp_sink_pcm_pf->format_size;
    a2dp_sink_ts->comsumer.byte_s = byte_s;
    a2dp_sink_ts->comsumer.timeout = (len * 1000) / byte_s;
    a2dp_sink_ts->comsumer.cache_timeout = sink_cf.cache_time;
    a2dp_sink_ts->comsumer.cache = byte_s * a2dp_sink_ts->comsumer.cache_timeout / 1000;

    BTMG_INFO("cache_time:%d",  a2dp_sink_ts->comsumer.cache_timeout);

    if (transmit_comsumer_start(a2dp_sink_ts, a2dp_sink_pcm_pf, len) < 0) {
        BTMG_ERROR("comsumer start fail");
        return BT_ERROR;
    }

    return BT_OK;
}

static int a2dp_sink_stream_stop(void)
{
    if (a2dp_stream_enable == false) {
        BTMG_INFO("already false.");
        return BT_OK;
    }
    a2dp_stream_enable = false;

    int ret = -1;
    BTMG_INFO("a2dp sink stop transmit.");

    if (a2dp_sink_ts == NULL || a2dp_sink_pcm_pf == NULL) {
        BTMG_WARNG("a2dp sink stream has stopped");
        return BT_OK;
    }
    ret = transmit_comsumer_stop(a2dp_sink_ts, true);

    return ret;
}

static int a2dp_sink_push_data(char *data, uint32_t len)
{
    if (a2dp_sink_ts == NULL) {
        BTMG_ERROR("a2dp sink is not init");
        return BT_ERROR_A2DP_SINK_NOT_INIT;
    }

    return transmit_producer(a2dp_sink_ts, data, len, TS_SG_NONE);
}

int bt_a2dp_sink_init(void)
{
    int ret = -1;

    BTMG_DEBUG("a2dp sink init");
    bluez_register_media_change_cb(media_change_callback);
    if (bt_config_read_a2dp_sink(&sink_cf) == -1) {
        BTMG_ERROR("read a2dp sink config failed.");
        return BT_ERROR;
    }

    a2dp_sink_pcm_pf = &a2dp_sink_pcm;
    strcpy(a2dp_sink_pcm_pf->device, sink_cf.device);
    a2dp_sink_pcm_pf->buffer_time = sink_cf.buffer_time;
    a2dp_sink_pcm_pf->period_time = sink_cf.period_time;

    a2dp_sink_ts = (transmit_t *)malloc(sizeof(transmit_t));
    if (a2dp_sink_ts == NULL) {
        BTMG_ERROR("a2dp sink transmit_t malloc failed.");
        return BT_ERROR_NO_MEMORY;
    }

    int byte_s = a2dp_sink_pcm_pf->sampling * a2dp_sink_pcm_pf->channels * 16 / 8;
	// uint32_t byte_s = 96000 * 2 * 32 / 8;
    if (transmit_init(a2dp_sink_ts, byte_s) == 0) {
        strcpy(a2dp_sink_ts->comsumer.com.name, "a2sin");
        a2dp_sink_ts->comsumer.cache_timeout = sink_cf.cache_time;
        a2dp_sink_ts->comsumer.timeout = (A2DP_SINK_COMSUMER_DATA_LEN * 1000) / byte_s;
        a2dp_sink_ts->comsumer.cache = byte_s * a2dp_sink_ts->comsumer.cache_timeout / 1000;
        a2dp_sink_ts->comsumer.com.app_cb.app_cb_init = NULL;
        a2dp_sink_ts->comsumer.com.app_cb.app_cb_deinit = a2dp_sink_pcm_deinit;
        a2dp_sink_ts->comsumer.com.sem.app_cb_deinit = a2dp_sink_pcm_deinit;
        a2dp_sink_ts->comsumer.com.app_cb.app_tran_cb = a2dp_sink_pcm_write;
        a2dp_sink_ts->comsumer.com.data_len = A2DP_SINK_COMSUMER_DATA_LEN;
        a2dp_sink_ts->comsumer.com.max_data_len = byte_s;

    } else {
        BTMG_ERROR("transmit init fail");
        goto fail;
    }

    if (transmit_comsumer_init(a2dp_sink_ts) < 0) {
        BTMG_ERROR("comsumer init fail");
        goto fail;
    }

    return BT_OK;
fail:

    if (a2dp_sink_ts) {
        free(a2dp_sink_ts);
        a2dp_sink_ts = NULL;
    }

    return BT_ERROR;
}

int bt_a2dp_sink_deinit(void)
{
    main_loop_on = false;

    if (a2dp_sink_ts) {
        if (transmit_comsumer_deinit(a2dp_sink_ts) < 0) {
            BTMG_ERROR("comsumer deinit fail");
            return BT_ERROR;
        }
        if (transmit_deinit(a2dp_sink_ts) < 0) {
            BTMG_ERROR("transmit deinit fail");
            return BT_ERROR;
        }
    }
    if (a2dp_sink_ts) {
        free(a2dp_sink_ts);
        a2dp_sink_ts = NULL;
    }
    a2dp_sink_pcm_pf = NULL;

    return BT_OK;
}

void bt_a2dp_sink_stream_cb_enable(bool enable)
{
    a2dp_sink_stream_cb = enable;
}
