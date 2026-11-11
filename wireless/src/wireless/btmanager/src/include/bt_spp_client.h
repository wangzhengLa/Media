/*
* Copyright (c) 2018-2022 Allwinner Technology Co. Ltd. All rights reserved.
* Author: BT Team
* Date: 2022.03.12
*/

#ifndef __BT_SPP_CLIENT_H__
#define __BT_SPP_CLIENT_H__

#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>

int spp_client_regesiter_dev(int dev, const char *dst);
int spp_client_send(char *data, uint32_t len);
int spp_client_connect(int dev, const char *dst);
int spp_client_disconnect(int dev, const char *dst);
int spp_client_bt_disconnect(const char *addr);
bool is_spp_client_run(void);
int spp_client_start_recv_thread(void);
void spp_client_stop_recv_thread(void);

#ifdef __cplusplus
}
#endif
#endif
