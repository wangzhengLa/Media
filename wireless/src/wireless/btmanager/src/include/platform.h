/*
* Copyright (c) 2018-2022 Allwinner Technology Co. Ltd. All rights reserved.
* Author: BT Team
* Date: 2022.03.12
*/

#ifndef __PLATFORM_H
#define __PLATFORM_H

#if __cplusplus
extern "C" {
#endif

int bt_platform_init(void);
void bt_platform_deinit(void);

#if __cplusplus
}; // extern "C"
#endif

#endif
