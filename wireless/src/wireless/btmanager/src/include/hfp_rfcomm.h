/*
* Copyright (c) 2018-2022 Allwinner Technology Co. Ltd. All rights reserved.
* Author: BT Team
* Date: 2022.03.12
*/

#ifndef __HFP_RFCOMM_H_
#define __HFP_RFCOMM_H_

#include "at.h"
#include "bt_manager.h"
#include "bt_pcm_supervise.h"


struct ba_rfcomm {
    /* RFCOMM socket */
    int fd;
    /* received AG indicator values */
    unsigned char hfp_ind[__HFP_IND_MAX];
    /* 0-based indicators index */
    enum hfp_ind hfp_ind_map[20];
};

/**
 * Callback function used for RFCOMM AT message dispatching. */
typedef int ba_rfcomm_callback(const struct btmg_at_t *at);

/**
 * AT message dispatching handler. */
struct ba_rfcomm_handler {
    enum btmg_at_type type;
    const char *command;
    ba_rfcomm_callback *callback;
};

int bt_hfp_hf_send_at_ata(void);
int bt_hfp_hf_send_at_chup(void);
int bt_hfp_hf_send_at_atd(char *number);
int bt_hfp_hf_send_at_bldn(void);
int bt_hfp_hf_send_at_btrh(bool query, uint32_t val);
int bt_hfp_hf_send_at_vts(char code);
int bt_hfp_hf_send_at_bcc(void);
int bt_hfp_hf_send_at_cnum(void);
int bt_hfp_hf_send_at_vgs(uint32_t volume);
int bt_hfp_hf_send_at_vgm(uint32_t volume);
int bt_hfp_hf_send_at_cmd(char *cmd);
int bt_hfp_ag_sco_conn_cmd(char *addr);
int bt_hfp_ag_sco_disconn_cmd(void);

void *rfcomm_thread_main(struct pcm_worker *w);
int bt_hfp_write_voice_setting(uint16_t voice_setting);

#endif