/*
 * Copyright (c) 2019-2025 Allwinner Technology Co., Ltd. ALL rights reserved.
 */

#ifndef __UDHCPC_H__
#define __UDHCPC_H__

#if __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <wifimg.h>

#define IPV4_ADDR_BM   0b1
#define IPV6_ADDR_BM   0b10

wmg_status_t start_udhcpc(char *inf, uint8_t start_ip_type);
void stop_udhcpc(char *inf, uint8_t stop_ip_type);
uint8_t is_ip_exist(char *inf, uint8_t check_ip);

/* WR-015: gate DHCP starts for one STA session. begin() must be called
 * once the STA is enabled (before any compensation DHCP), end() before
 * the monitor/event producer is stopped on disable. */
void dhcp_owner_session_begin(const char *inf);
void dhcp_owner_session_end(const char *inf);

#if __cplusplus
}
#endif

#endif /* __UDHCPC_H__ */
