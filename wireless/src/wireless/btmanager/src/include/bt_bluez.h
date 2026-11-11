/*
* Copyright (c) 2018-2022 Allwinner Technology Co. Ltd. All rights reserved.
* Author: BT Team
* Date: 2022.03.12
*/

#include <glib.h>
#include <gio/gio.h>

#ifndef BT_BLUEZ_H_
#define BT_BLUEZ_H_

#include "bt_manager.h"

int bt_bluez_init(void);
int bt_bluez_deinit(void);

#endif
