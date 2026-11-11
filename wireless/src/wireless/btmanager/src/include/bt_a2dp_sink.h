/*
* Copyright (c) 2018-2022 Allwinner Technology Co. Ltd. All rights reserved.
* Author: BT Team
* Date: 2022.03.12
*/

#ifndef __BLUEA_A2DP_SINK_H__
#define __BLUEA_A2DP_SINK_H__
#include "bt_pcm_supervise.h"

#ifdef __cplusplus
extern "C" {
#endif
int bt_a2dp_sink_init(void);
int bt_a2dp_sink_deinit(void);
void bt_a2dp_sink_stream_cb_enable(bool enable);
void *a2dp_pcm_worker_routine(struct pcm_worker *w);

#ifdef __cplusplus
}
#endif
#endif
