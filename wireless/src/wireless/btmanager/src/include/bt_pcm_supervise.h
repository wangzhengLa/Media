/*
* Copyright (c) 2018-2022 Allwinner Technology Co. Ltd. All rights reserved.
* Author: BT Team
* Date: 2022.03.12
*/

#ifndef __BT_PCM_SUPERVISE_H__
#define __BT_PCM_SUPERVISE_H__

#include <stdbool.h>
#include <pthread.h>
#include "dbus-client.h"
#include <alsa/asoundlib.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pcm_worker {
    pthread_t thread;
    /*whether the sink pcm thread is running */
    bool thread_enable;
    /* used BlueALSA PCM device */
    struct ba_pcm ba_pcm;
    /* file descriptor of PCM FIFO */
    int ba_pcm_fd;
    /* file descriptor of PCM control */
    int ba_pcm_ctrl_fd;
    /* if true, playback is active */
    bool active;
    /* human-readable BT address */
    char addr[18];
};

extern struct ba_dbus_ctx dbus_ctx;

snd_pcm_format_t get_snd_pcm_format(const struct ba_pcm *pcm);
struct pcm_worker *get_active_worker(void);

#ifdef __cplusplus
}
#endif
#endif
