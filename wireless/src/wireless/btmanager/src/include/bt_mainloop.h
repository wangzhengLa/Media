/*
* Copyright (c) 2018-2022 Allwinner Technology Co. Ltd. All rights reserved.
* Author: BT Team
* Date: 2022.03.12
*/

#ifndef __BT_MAIN_LOOP_H__
#define __BT_MAIN_LOOP_H__

int bt_bluez_mainloop_init(void);
int bt_bluez_mainloop_deinit(void);
pthread_t bt_bluez_thread_tid_get(void);

#endif
