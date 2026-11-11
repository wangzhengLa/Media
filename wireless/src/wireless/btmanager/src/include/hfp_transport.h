/*
* Copyright (c) 2018-2022 Allwinner Technology Co. Ltd. All rights reserved.
* Author: BT Team
* Date: 2022.03.12
*/

#ifndef __HFP_TRANSPORT_H
#define __HFP_TRANSPORT_H
#if __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <bt_log.h>
#include "bt_pcm_supervise.h"

enum hfp_thread {
    PTHONE_STREAM_PLAY = 0,
    DEV_STREAM_RECORD,
};

struct hfp_hf_worker {
    bool enable;
    enum hfp_thread thread_flag;
    struct pcm_config *read_pcm;
    struct pcm_config *write_pcm;
    pthread_t thread;
    bool thread_loop;
    uint8_t read_write_frams;
};

enum interface_type_t {
    UART_TYPE = 0,
    PCM_TYPE,
};

int bt_hfp_read_config(void);
void bt_hfp_hf_pcm_start(void);
void bt_hfp_hf_pcm_stop(void);
void update_pcm_device_info(char *addr);

extern enum interface_type_t hfp_if_type;

#if __cplusplus
}; // extern "C"
#endif

#endif
