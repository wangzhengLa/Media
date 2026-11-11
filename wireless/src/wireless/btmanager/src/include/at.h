/*
* Copyright (c) 2018-2022 Allwinner Technology Co. Ltd. All rights reserved.
* Author: BT Team
* Date: 2022.03.12
*/

#ifndef __AT_H_
#define __AT_H_

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "bt_manager.h"

/**
 * HFP Indicators */
enum __attribute__((packed)) hfp_ind {
    HFP_IND_NULL = 0,
    HFP_IND_SERVICE,
    HFP_IND_CALL,
    HFP_IND_CALLSETUP,
    HFP_IND_CALLHELD,
    HFP_IND_SIGNAL,
    HFP_IND_ROAM,
    HFP_IND_BATTCHG,
    __HFP_IND_MAX
};


char *at_build(char *buffer, size_t size, enum btmg_at_type type, const char *command,
               const char *value);
char *at_parse(const char *str, struct btmg_at_t *at);
int at_parse_bia(const char *str, bool state[__HFP_IND_MAX]);
int at_parse_cind(const char *str, enum hfp_ind map[20]);
int at_parse_cmer(const char *str, unsigned int map[5]);
const char *at_type2str(enum btmg_at_type type);

#endif
