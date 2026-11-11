/*
* Copyright (c) 2018-2022 Allwinner Technology Co. Ltd. All rights reserved.
* Author: BT team
* Date: 2022.03.12
* Description: btmanager api realize.
*/

#include <stdio.h>
#include <api_action.h>

#include "bt_log.h"
#include "bt_action.h"
#include "bt_manager.h"
#include "common.h"
#include "bt_le_hci.h"
#include "bt_gatt_client.h"
#include "bt_gatt_server.h"
#include "bt_agent.h"
#include "bt_le5_hci.h"
#include "bt_adapter.h"
#include "bt_config_json.h"

btmg_callback_t *btmg_cb_p = NULL;
btmg_profile_info_t *bt_pro_info = NULL;
static act_handle_t bt_act_handle;
static act_handle_t ble_act_handle;

static void register_callback(btmg_callback_t *btmg_cb)
{
    btmg_cb_p = btmg_cb;
}

static void unregister_callback()
{
    btmg_cb_p = NULL;
}

int bt_manager_preinit(btmg_callback_t **btmg_cb)
{
    *btmg_cb = (btmg_callback_t *)malloc(sizeof(btmg_callback_t));
    if (*btmg_cb == NULL) {
        BTMG_ERROR("malloc for btmg_cb failed");
        goto failed;
    }

    memset(*btmg_cb, 0, sizeof(btmg_callback_t));

    bt_pro_info = (btmg_profile_info_t *)malloc(sizeof(btmg_profile_info_t));
    if (NULL == bt_pro_info) {
        BTMG_ERROR("malloc for bt_pro_info failed\n");
        goto failed;
    }

    memset(bt_pro_info, 0, sizeof(btmg_profile_info_t));

#ifdef ENABLE_CLASSIC_BLUETOOTH
    act_init(&bt_act_handle, "bt_action", true);
    act_register_handler(&bt_act_handle, ACT_ID_BT, bt_table);
#endif

#ifdef ENABLE_LE_BLUETOOTH
    act_init(&ble_act_handle, "ble_action", true);
    act_register_handler(&ble_act_handle, ACT_ID_GATTC, gatt_client_action_table);
    act_register_handler(&ble_act_handle, ACT_ID_GATTS, gatts_table);
    act_register_handler(&ble_act_handle, ACT_ID_LE_GAP, le_gap_action_table);
    act_register_handler(&ble_act_handle, ACT_ID_LE5_GAP, le5_gap_action_table);
#endif

    return BT_OK;

failed:

    if (*btmg_cb) {
        free(*btmg_cb);
        *btmg_cb = NULL;
    }

    return BT_ERROR_NO_MEMORY;
}

int bt_manager_set_loglevel(btmg_log_level_t log_level)
{
    int ret = BT_OK;

    ret = btmg_set_debug_level(log_level);

    return ret;
}

btmg_log_level_t bt_manager_get_loglevel(void)
{
    int level = btmg_get_debug_level();

    if (level >= MSG_DEBUG)
        level = BTMG_LOG_LEVEL_DEBUG;

    return level;
}

int bt_manager_init(btmg_callback_t *btmg_cb)
{
    BTMG_DEBUG("enter");

    if (bt_pro_info == NULL) {
        BTMG_ERROR("btmanager not preinit");
        goto fail;
    }

    if (btmg_cb != NULL)
        register_callback(btmg_cb);
    else {
        BTMG_ERROR("main callback is NULL");
        goto fail;
    }

#ifdef ENABLE_CLASSIC_BLUETOOTH
    connected_devices = btmg_dev_list_new();
    if (connected_devices == NULL) {
        BTMG_ERROR("connected_devices init fail");
        goto fail;
    }
#endif

    return BT_OK;

fail:

    return BT_ERROR_NULL_VARIABLE;

}

int bt_manager_deinit(btmg_callback_t *btmg_cb)
{
    BTMG_DEBUG("enter");

    if (btmg_cb) {
        free(btmg_cb);
        unregister_callback();
    }

    if (bt_pro_info) {
        free(bt_pro_info);
        bt_pro_info = NULL;
    }

#ifdef ENABLE_CLASSIC_BLUETOOTH
    btmg_dev_list_free(connected_devices);
    act_deinit(&bt_act_handle);
#endif
#ifdef ENABLE_LE_BLUETOOTH
    act_deinit(&ble_act_handle);
#endif

    return BT_OK;
}

int bt_manager_enable_profile(int profile)
{
    if (bt_pro_info == NULL) {
        return BT_ERROR_NULL_VARIABLE;
    }

    BTMG_DEBUG("profile mask:%d", profile);

    if (profile & BTMG_A2DP_SINK_ENABLE) {
        BTMG_DEBUG("a2dp sink profile enable.");
        bt_pro_info->is_a2dp_sink_enabled = true;
    }
    if (profile & BTMG_A2DP_SOUCE_ENABLE) {
        BTMG_DEBUG("a2dp source profile enable.");
        bt_pro_info->is_a2dp_source_enabled = true;
    }
    if (profile & BTMG_AVRCP_ENABLE) {
        BTMG_DEBUG("avrcp profile enable.");
        bt_pro_info->is_avrcp_enabled = true;
    }
    if (profile & BTMG_HFP_HF_ENABLE) {
        BTMG_DEBUG("hfp hf profile enable.");
        bt_pro_info->is_hfp_hf_enabled = true;
    }
    if (profile & BTMG_HFP_AG_ENABLE) {
        BTMG_DEBUG("hfp ag profile enable.");
        bt_pro_info->is_hfp_ag_enabled = true;
    }
    if (profile & BTMG_GATT_SERVER_ENABLE) {
        BTMG_DEBUG("gatt server profile enable.");
        bt_pro_info->is_gatts_enabled = true;
    }
    if (profile & BTMG_GATT_CLIENT_ENABLE) {
        BTMG_DEBUG("gatt client profile enable.");
        bt_pro_info->is_gattc_enabled = true;
    }
    if (profile & BTMG_SPP_ENABLE) {
        BTMG_DEBUG("spp profile enable.");
        bt_pro_info->is_sppc_enabled = true;
        bt_pro_info->is_spps_enabled = true;
    }
    if (profile & BTMG_SMP_ENABLE) {
        BTMG_DEBUG("smp profile enable.");
        bt_pro_info->is_smp_enabled = true;
    }

    return BT_OK;
}

int bt_manager_set_default_profile(bool is_default)
{
    struct bt_profile_cf profile_cf;
    if (bt_config_read_profile(&profile_cf) == -1) {
        BTMG_ERROR("read bt config failed");
        return BT_ERROR;
    }
    /*enable a2dp_sink and disable HFP as default*/
    bt_pro_info->is_a2dp_sink_enabled = profile_cf.a2dp_sink;
    bt_pro_info->is_a2dp_source_enabled = profile_cf.a2dp_source;
    bt_pro_info->is_avrcp_enabled = profile_cf.avrcp;
    bt_pro_info->is_hfp_hf_enabled = profile_cf.hfp_hf;
    bt_pro_info->is_hfp_ag_enabled = profile_cf.hfp_ag;
    bt_pro_info->is_sppc_enabled = profile_cf.spp_client;
    bt_pro_info->is_spps_enabled = profile_cf.spp_server;
    bt_pro_info->is_gatts_enabled = profile_cf.gatt_server;
    bt_pro_info->is_gattc_enabled = profile_cf.gatt_client;
    bt_pro_info->is_smp_enabled = profile_cf.smp;

    BTMG_INFO("enable default profile from bt config");
    return BT_OK;
}

int bt_manager_get_adapter_address(char *addr)
{
    int ret = BT_OK;

    ret = bt_adapter_get_address(addr);

    return ret;
}

#ifdef ENABLE_CLASSIC_BLUETOOTH
bool check_bt_is_on(void)
{
    bool is_on = false;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_CHECK_STATE, 0, 1, &is_on);

    return is_on;
}

int bt_manager_set_ex_debug_mask(int mask)
{
    int ret = BT_OK;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_SET_EX_DEBUG, 1, 1, &mask, &ret);

    return ret;
}

int bt_manager_get_ex_debug_mask(void)
{
    int mask = -1;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_GET_EX_DEBUG, 0, 1, &mask);

    return mask;
}

const char *bt_manager_get_error_info(int error)
{
    return btmg_get_error_info(error);
}

int bt_manager_enable(bool enable)
{
    int ret = BT_OK;

    BTMG_DEBUG("enter");
    act_transfer(&bt_act_handle, ACT_ID_BT, BT_BTMG_ENABLE, 1, 1, &enable, &ret);

    return ret;
}

int bt_manager_set_scan_mode(btmg_scan_mode_t mode)
{
    int ret = BT_OK;

    BTMG_DEBUG("enter");
    act_transfer(&bt_act_handle, ACT_ID_BT, BT_SET_SCAN_MODE, 1, 1, &mode, &ret);

    return ret;
}

int bt_manager_scan_filter(btmg_scan_filter_t *filter)
{
    int ret = BT_OK;

    BTMG_DEBUG("enter");
    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_SET_SCAN_FILTER, 1, 1, filter, &ret);

    return ret;
}

int bt_manager_start_scan(void)
{
    int ret = BT_OK;

    BTMG_DEBUG("enter");
    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_START_SCAN, 0, 1, &ret);

    return ret;
}

int bt_manager_stop_scan(void)
{
    int ret = BT_OK;

    BTMG_DEBUG("enter");
    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_STOP_SCAN, 0, 1, &ret);

    return ret;
}

bool bt_manager_is_scanning(void)
{
    bool is_dis = false;

    if (check_bt_is_on == false)
        return false;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_IS_SCAN, 0, 1, &is_dis);

    return is_dis;
}

int bt_manager_pair(char *addr)
{
    int ret = BT_OK;

    BTMG_DEBUG("pair device:%s", addr);
    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_PAIR, 1, 1, addr, &ret);

    return ret;
}

int bt_manager_unpair(char *addr)
{
    int ret = BT_OK;

    BTMG_DEBUG("unpair device:%s", addr);
    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_UNPAIR, 1, 1, addr, &ret);

    return ret;
}

int bt_manager_get_paired_devices(btmg_bt_device_t **dev_list, int *count)
{
    int ret = BT_OK;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_GET_PAIRED_DEV, 1, 2, dev_list, &ret, count);

    return ret;
}

int bt_manager_free_paired_devices(btmg_bt_device_t *dev_list, int count)
{
    int ret = BT_OK;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_FREE_PAIRED_DEV, 2, 1, dev_list, &count, &ret);

    return ret;
}

btmg_adapter_state_t bt_manager_get_adapter_state(void)
{
    btmg_adapter_state_t adapter_state;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_GET_ADATER_STATE, 0, 1, &adapter_state);

    return adapter_state;
}

int bt_manager_get_adapter_name(char *name)
{
    int ret = BT_OK;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_GET_ADATER_NAME, 0, 2, &ret, name);

    return ret;
}

int bt_manager_set_adapter_name(const char *name)
{
    int ret = BT_OK;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_SET_ADATER_NAME, 1, 1, name, &ret);

    return ret;
}

int bt_manager_get_device_name(const char *addr, char *name)
{
    int ret = BT_OK;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_GET_DEVICE_NAME, 1, 2, addr, &ret, name);

    return ret;
}

int bt_manager_connect(const char *addr)
{
    int ret = BT_OK;

    BTMG_DEBUG("connect device:%s", addr);
    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;
    act_transfer(&bt_act_handle, ACT_ID_BT, BT_DEV_CONNECT, 1, 1, addr, &ret);

    return ret;
}

int bt_manager_disconnect(const char *addr)
{
    int ret = BT_OK;

    BTMG_DEBUG("disconnect device:%s", addr);
    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_DEV_DISCONNECT, 1, 1, addr, &ret);

    return ret;
}

bool bt_manager_device_is_connected(const char *addr)
{
    bool is_connected = false;

    if (check_bt_is_on == false)
        return false;
    act_transfer(&bt_act_handle, ACT_ID_BT, BT_IS_CONNECTED, 1, 1, addr, &is_connected);

    return is_connected;
}

int bt_manager_get_connection_state(void)
{
    //TODO:for all profile device:
    return 0;
}

int bt_manager_set_page_timeout(int slots)
{
    int ret = BT_OK;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;
    act_transfer(&bt_act_handle, ACT_ID_BT, BT_SET_PAGE_TO, 1, 1, &slots, &ret);

    return ret;
}

int bt_manager_remove_device(const char *addr)
{
    int ret = BT_OK;

    BTMG_DEBUG("remove device:%s", addr);
    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_REMOVE_DEV, 1, 1, addr, &ret);

    return ret;
}

void bt_manager_a2dp_sink_stream_cb_enable(bool enable)
{
    act_transfer(&bt_act_handle, ACT_ID_BT, BT_A2DP_SINK_STREAM_CB_ENABLE, 1, 0, &enable);
}

int bt_manager_a2dp_src_init(uint16_t channels, uint32_t sampling, int format)
{
    int ret = BT_OK;

    BTMG_DEBUG("enter");
    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_A2DP_SRC_INIT, 3, 1, &channels, &sampling, &format, &ret);

    return ret;
}

int bt_manager_a2dp_src_deinit(void)
{
    int ret = BT_OK;

    BTMG_DEBUG("enter");
    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_A2DP_SRC_DEINIT, 0, 1, &ret);

    return ret;
}

int bt_manager_a2dp_src_stream_start(uint32_t len)
{
    int ret = BT_OK;

    BTMG_DEBUG("enter");
    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_A2DP_SRC_STREAM_START, 1, 1, &len, &ret);

    return ret;
}

int bt_manager_a2dp_src_stream_stop(bool drop)
{
    int ret = BT_OK;

    BTMG_DEBUG("enter");
    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_A2DP_SRC_STREAM_STOP, 1, 1, &drop, &ret);

    return ret;
}

int bt_manager_a2dp_src_stream_send(char *data, int len)
{
    int ret = BT_OK;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    ret = bt_a2dp_src_stream_send(data, len);

    return ret;
}

bool bt_manager_a2dp_src_is_stream_start(void)
{
    bool is_start = false;

    if (check_bt_is_on == false)
        return false;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_A2DP_SRC_IS_STREAM_START, 0, 1, &is_start);

    return is_start;
}

int bt_manager_a2dp_set_vol(int vol_value)
{
    int ret = BT_OK;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_A2DP_SET_VOL, 1, 1, &vol_value, &ret);

    return ret;
}

int bt_manager_a2dp_get_vol(void)
{
    int vol_value = 0;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_A2DP_GET_VOL, 0, 1, &vol_value);

    return vol_value;
}

int bt_manager_avrcp_command(char *addr, btmg_avrcp_command_t command)
{
    int ret = BT_OK;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_AVRCP_COMMAND, 2, 1, addr, &command, &ret);

    return ret;
}

int bt_manager_avrcp_send_metadata(btmg_metadata_info_t command)
{
    int ret = BT_OK;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_AVRCP_METADATA, 1, 1, &command, &ret);

    return ret;
}

int bt_manager_avrcp_send_position(uint64_t position_ms)
{
    int ret = BT_OK;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_AVRCP_POSITION, 1, 1, &position_ms, &ret);

    return ret;
}

int bt_manager_avrcp_set_abs_volume(uint16_t volume)
{
    int ret = BT_OK;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_AVRCP_SET_ABS_VOLUME, 1, 1, &volume, &ret);

    return ret;
}

int bt_manager_avrcp_get_abs_volume(uint16_t *volume)
{
    int ret = BT_OK;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_AVRCP_GET_ABS_VOLUME, 0, 2, &ret, volume);

    return ret;
}

extern int avrcp_status;
btmg_avrcp_play_state_t bt_manager_get_avrcp_status(void)
{
    return avrcp_status;
}

void bt_manager_set_avrcp_status(btmg_avrcp_play_state_t status)
{
    avrcp_status = status;
    if (avrcp_status == BTMG_AVRCP_PLAYSTATE_STOPPED) {
        BTMG_INFO("BT set_avrcp_status stopped");
    } else if (avrcp_status == BTMG_AVRCP_PLAYSTATE_PLAYING) {
        BTMG_INFO("BT set_avrcp_status playing");
    } else if (avrcp_status == BTMG_AVRCP_PLAYSTATE_PAUSED) {
        BTMG_INFO("BT set_avrcp_status paused");
    } else if (avrcp_status == BTMG_AVRCP_PLAYSTATE_FWD_SEEK) {
        BTMG_INFO("BT set_avrcp_status FWD SEEK");
    } else if (avrcp_status == BTMG_AVRCP_PLAYSTATE_REV_SEEK) {
        BTMG_INFO("BT set_avrcp_status REV SEEK");
    } else if (avrcp_status == BTMG_AVRCP_PLAYSTATE_FORWARD) {
        BTMG_INFO("BT set_avrcp_status forward");
    } else if (avrcp_status == BTMG_AVRCP_PLAYSTATE_BACKWARD) {
        BTMG_INFO("BT set_avrcp_status backward");
    } else if (avrcp_status == BTMG_AVRCP_PLAYSTATE_ERROR) {
        BTMG_INFO("BT set_avrcp_status ERROR");
    }
}

/*
 1 slot = 0.625ms
 timeout = slots * 0.625ms;
*/
int bt_manager_set_link_supervision_timeout(const char *addr, int slots)
{
    int ret = BT_OK;

    BTMG_DEBUG("enter");
    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_SET_LINK_SUP_TO, 2, 1, addr, &slots, &ret);

    return ret;
}

int bt_manager_hfp_hf_send_at_ata(void)
{
    int ret = BT_OK;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_HFP_AT_ATA, 0, 1, &ret);

    return ret;
}

int bt_manager_hfp_hf_send_at_chup(void)
{
    int ret = BT_OK;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_HFP_AT_CHUP, 0, 1, &ret);

    return ret;
}

int bt_manager_hfp_hf_send_at_atd(char *number)
{
    int ret = BT_OK;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_HFP_AT_ATD, 1, 1, number, &ret);

    return ret;
}

int bt_manager_hfp_hf_send_at_bldn(void)
{
    int ret = BT_OK;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_HFP_AT_BLDN, 0, 1, &ret);

    return ret;
}

int bt_manager_hfp_hf_send_at_btrh(bool query, uint32_t val)
{
    int ret = BT_OK;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_HFP_AT_BTRH, 2, 1, &query, &val, &ret);

    return ret;
}

int bt_manager_hfp_hf_send_at_vts(char code)
{
    int ret = BT_OK;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_HFP_AT_VTS, 1, 1, &code, &ret);

    return ret;
}

int bt_manager_hfp_hf_send_at_bcc(void)
{
    int ret = BT_OK;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_HFP_AT_BCC, 0, 1, &ret);

    return ret;
}

int bt_manager_hfp_hf_send_at_cnum(void)
{
    int ret = BT_OK;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_HFP_AT_CNUM, 0, 1, &ret);

    return ret;
}

int bt_manager_hfp_hf_send_at_vgs(uint32_t volume)
{
    int ret = BT_OK;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_HFP_AT_VGS, 1, 1, &volume, &ret);

    return ret;
}

int bt_manager_hfp_hf_send_at_vgm(uint32_t volume)
{
    int ret = BT_OK;

    if (check_bt_is_on == false)
        return -1;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_HFP_AT_VGM, 1, 1, &volume, &ret);

    return ret;
}

int bt_manager_hfp_hf_send_at_cmd(char *cmd)
{
    int ret = BT_OK;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_HFP_AT_CMD, 1, 1, cmd, &ret);

    return ret;
}

int bt_manager_hfp_ag_sco_conn_cmd(char *addr)
{
    int ret = BT_OK;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_HFP_AG_SCO_CONN, 1, 1, addr, &ret);

    return ret;
}

int bt_manager_hfp_ag_sco_disconn_cmd(void)
{
    int ret = BT_OK;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_HFP_AG_SCO_DISCONN, 0, 1, &ret);

    return ret;
}

int bt_manager_agent_set_io_capability(btmg_io_capability_t io_cap)
{
    int ret = BT_OK;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    if (bt_agent_register(io_cap) == 0) {
        ret = bt_agent_set_default();
    }

    return ret;
}

int bt_manager_agent_send_pincode(void *handle, char *pincode)
{
    int ret = BT_OK;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    ret = bt_agent_send_pincode(handle, pincode);

    return ret;
}

int bt_manager_agent_send_passkey(void *handle, unsigned int passkey)
{
    int ret = BT_OK;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    ret = bt_agent_send_passkey(handle, passkey);

    return ret;
}

int bt_manager_agent_send_pair_error(void *handle, btmg_pair_request_error_t e, const char *err_msg)
{
    int ret = BT_OK;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    ret = bt_agent_send_error(handle, e, err_msg);

    return ret;
}

int bt_manager_agent_pair_send_empty_response(void *handle)
{
    int ret = BT_OK;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    bt_agent_send_empty_response(handle);

    return ret;
}

int bt_manager_spp_client_connect(const char *dst)
{
    int ret = BT_OK;

    BTMG_DEBUG("spp client connect device:%s", dst);
    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_SPP_CLI_CONNECT, 1, 1, dst, &ret);

    return ret;
}

int bt_manager_spp_client_send(char *data, uint32_t len)
{
    int ret = BT_OK;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_SPP_CLI_SEND, 2, 1, data, &len, &ret);

    return ret;
}

int bt_manager_spp_client_disconnect(const char *dst)
{
    int ret = BT_OK;

    BTMG_DEBUG("spp client disconnect device:%s", dst);
    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_SPP_CLI_DISCONNECT, 1, 1, dst, &ret);

    return ret;
}

int bt_manager_spp_service_accept(void)
{
    int ret = BT_OK;

    BTMG_DEBUG("spp service accept the client");
    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_SPP_SVC_ACCEPT, 0, 1, &ret);

    return ret;
}

int bt_manager_spp_service_send(char *data, uint32_t len)
{
    int ret = BT_OK;

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_SPP_SVC_SEND, 2, 1, data, &len, &ret);

    return ret;
}

int bt_manager_spp_service_disconnect(const char *dst)
{
    int ret = BT_OK;

    BTMG_DEBUG("spp service disconnect device:%s", dst);

    if (check_bt_is_on == false)
        return BT_ERROR_ADATER_NOT_ON;

    act_transfer(&bt_act_handle, ACT_ID_BT, BT_SPP_SVC_DISCONNECT, 1, 1, dst, &ret);

    return ret;
}
#endif

#ifdef ENABLE_LE_BLUETOOTH
int bt_manager_gatt_server_init(void)
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_GATTS, GATTS_INIT, 0, 1, &ret);

    return ret;
}

/*GATT Server API*/
int bt_manager_gatt_server_create_service(gatts_add_svc_t *svc)
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_GATTS, GATTS_ADD_SVC, 1, 1, svc, &ret);

    return ret;
}

int bt_manager_gatt_server_add_characteristic(gatts_add_char_t *chr)
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_GATTS, GATTS_ADD_CHAR, 1, 1, chr, &ret);

    return ret;
}

int bt_manager_gatt_server_add_descriptor(gatts_add_desc_t *desc)
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_GATTS, GATTS_ADD_DESC, 1, 1, desc, &ret);

    return ret;
}

int bt_manager_gatt_server_start_service(gatts_star_svc_t *start_svc)
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_GATTS, GATTS_START_SVC, 1, 1, start_svc, &ret);

    return ret;
}

int bt_manager_gatt_server_stop_service(gatts_stop_svc_t *stop_svc)
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_GATTS, GATTS_STOP_SVC, 1, 1, stop_svc, &ret);

    return ret;
}

int bt_manager_gatt_server_remove_service(gatts_remove_svc_t *remove_svc)
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_GATTS, GATTS_REMOVE_SVC, 1, 1, remove_svc, &ret);

    return ret;
}

int bt_manager_gatt_server_send_read_response(gatts_send_read_rsp_t *pData)
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_GATTS, GATTS_SEND_RSP, 1, 1, pData, &ret);

    return ret;
}

int bt_manager_gatt_server_send_write_response(gatts_write_rsp_t *pData)
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_GATTS, GATTS_SEND_WRITE_RSP, 1, 1, pData, &ret);

    return ret;
}

int bt_manager_gatt_server_send_notify(gatts_notify_data_t *pData)
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_GATTS, GATTS_SEND_NOTIFY, 1, 1, pData, &ret);

    return ret;
}

int bt_manager_gatt_server_send_indication(gatts_indication_data_t *pData)
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_GATTS, GATTS_SEND_IND, 1, 1, pData, &ret);

    return ret;
}

int bt_manager_gatt_server_deinit(void)
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_GATTS, GATTS_DEINIT, 0, 1, &ret);

    return ret;
}

int bt_manager_gatt_server_get_mtu()
{
    int mtu = -1;

    act_transfer(&ble_act_handle, ACT_ID_GATTS, GATTS_GET_MTU, 0, 1, &mtu);

    return mtu;
}

int bt_manager_le_set_adv_data(btmg_adv_data_t *adv_data)
{
    return bt_le_set_advertising_data(adv_data);
}

int bt_manager_le_set_scan_rsp_data(btmg_scan_rsp_data_t *rsp_data)
{
    return bt_le_set_scan_rsp_data(rsp_data);
}

int bt_manager_le_set_random_address(void)
{
    return bt_le_set_random_address();
}

int bt_manager_le_enable_adv(bool enable)
{
    return bt_le_advertising_enable(enable);
}

int bt_manager_le_set_adv_param(btmg_le_advertising_parameters_t *adv_param)
{
    return bt_le_set_advertising_params(adv_param);
}

int bt_manager_le5_ext_adv_set_rand_addr(uint8_t instance)
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_LE5_GAP, LE5_EXT_ADV_SET_RAND_ADDR, 1, 1, &instance, &ret);

    return ret;
}

int bt_manager_le5_ext_adv_set_params(uint8_t instance, const btmg_le5_gap_ext_adv_params_t *params)
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_LE5_GAP, LE5_EXT_ADV_SET_PARAMS, 2, 1, &instance, params, &ret);

    return ret;
}

int bt_manager_le5_ext_adv_data_raw(uint8_t instance, uint16_t length, const uint8_t *data)
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_LE5_GAP, LE5_EXT_ADV_DATA, 3, 1, &instance, &length, data, &ret);

    return ret;
}

int bt_manager_le5_ext_scan_rsp_data_raw(uint8_t instance, uint16_t length, const uint8_t *scan_rsp_data)
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_LE5_GAP, LE5_EXT_SCAN_RSP_DATA, 3, 1, &instance, &length, scan_rsp_data, &ret);

    return ret;
}

int bt_manager_le5_ext_adv_enable(bool enable, uint8_t num_adv, const btmg_le5_gap_ext_adv_t *ext_adv)
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_LE5_GAP, LE5_EXT_ADV_ENABLE, 3, 1, &enable, &num_adv, ext_adv, &ret);

    return ret;
}

int bt_manager_le5_ext_adv_set_remove(uint8_t instance)
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_LE5_GAP, LE5_EXT_ADV_SET_REMOVE, 1, 1, &instance, &ret);

    return ret;
}

int bt_manager_le5_ext_adv_set_clear_all(void)
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_LE5_GAP, LE5_EXT_ADV_SET_CLEAR_ALL, 0, 1, &ret);

    return ret;
}

int bt_manager_le5_periodic_adv_set_params(uint8_t instance, const btmg_le5_gap_periodic_adv_params_t *params)
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_LE5_GAP, LE5_PERIODIC_ADV_SET_PARAMS, 2, 1, &instance, params, &ret);

    return ret;
}

int bt_manager_le5_periodic_adv_data_raw(uint8_t instance, uint16_t length, const uint8_t *data)
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_LE5_GAP, LE5_PERIODIC_ADV_DATA, 3, 1, &instance, &length, data, &ret);

    return ret;
}

int bt_manager_le5_periodic_adv_enable(bool enable, uint8_t instance)
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_LE5_GAP, LE5_PERIODIC_ADV_ENABLE, 2, 1, &enable, &instance, &ret);

    return ret;
}

int bt_manager_gatt_client_init(void)
{
    int ret = BT_OK;

    BTMG_DEBUG("enter");
    act_transfer(&ble_act_handle, ACT_ID_GATTC, GATTC_INIT, 0, 1, &ret);
    bt_le_monitor_init(0);

    return ret;
}

int bt_manager_gatt_client_deinit(void)
{
    int ret = BT_OK;

    BTMG_DEBUG("enter");
    bt_le_monitor_deinit(0);
    act_transfer(&ble_act_handle, ACT_ID_GATTC, GATTC_DEINIT, 0, 1, &ret);

    return ret;
}
int bt_manager_gatt_client_connect(uint8_t *addr, btmg_le_addr_type_t dst_type,
                                   uint16_t mtu, btmg_security_level_t sec)
{
    int i = 0;
    int ret = BT_OK;

    gattc_cn_args_t cn_args;

    cn_args.addr = addr;
    cn_args.dst_type = dst_type;
    cn_args.mtu = mtu;
    cn_args.sec = sec;
    act_transfer(&ble_act_handle, ACT_ID_GATTC, GATTC_CONNECT, 1, 1, &cn_args, &ret);

    return ret;
}

int bt_manager_gatt_client_disconnect(uint8_t *addr)
{
    int ret = BT_OK;

    gattc_dcn_args_t args;
    args.addr = addr;
    act_transfer(&ble_act_handle, ACT_ID_GATTC, GATTC_DISCONNECT, 1, 1, &args, &ret);

    return ret;
}

int bt_manager_gatt_client_get_conn_list(void)
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_GATTC, GATTC_GET_CONNECTED_LIST, 0, 1, &ret);

    return ret;
}

int bt_manager_gatt_client_set_security(int conn_id, int sec_level)
{
    int ret = BT_OK;

    gattc_set_sec_args_t args;
    args.conn_id = conn_id;
    args.sec_level = sec_level;
    act_transfer(&ble_act_handle, ACT_ID_GATTC, GATTC_SET_SEC, 1, 1, &args, &ret);

    return ret;
}

int bt_manager_gatt_client_get_security(int conn_id)
{
    int level = -1;

    act_transfer(&ble_act_handle, ACT_ID_GATTC, GATTC_GET_SEC, 1, 1, &conn_id, &level);

    return level;
}

int bt_manager_gatt_client_unregister_notify_indicate(int conn_id, int id)
{
    int ret = BT_OK;

    gattc_unreg_notify_indicate_args_t args;
    args.conn_id = conn_id;
    args.id = id;

    act_transfer(&ble_act_handle, ACT_ID_GATTC, GATTC_UNREG_NOTIFY_INDICATE, 1, 1, &args, &ret);

    return ret;
}

int bt_manager_gatt_client_register_notify_indicate(int conn_id, int char_handle)
{
    int id = -1;
    gattc_reg_notify_indicate_args_t args;
    args.conn_id = conn_id;
    args.char_handle = char_handle;
    if (act_transfer(&ble_act_handle, ACT_ID_GATTC, GATTC_REG_NOTIFY_INDICATE, 1, 1, &args, &id) == 0) {
        BTMG_DEBUG("register notify/indicate id:%d", id);
        return id;
    } else {
        BTMG_DEBUG("register notify/indicate failed");
        return -1;
    }
}

int bt_manager_gatt_client_write_request(int conn_id, int char_handle, uint8_t *value,
                                         uint16_t len)
{
    int ret = BT_OK;

    gattc_write_req_args_t args;
    args.conn_id = conn_id;
    args.char_handle = char_handle;
    args.value = value;
    args.len = len;

    act_transfer(&ble_act_handle, ACT_ID_GATTC, GATTC_WRITE_REQ, 1, 1, &args, &ret);

    return ret;
}

int bt_manager_gatt_client_write_command(int conn_id, int char_handle, bool signed_write,
                                         uint8_t *value, uint16_t len)
{
    int ret = BT_OK;

    gattc_write_cmd_args_t args;
    args.conn_id = conn_id;
    args.char_handle = char_handle;
    args.signed_write = signed_write;
    args.value = value;
    args.len = len;

    act_transfer(&ble_act_handle, ACT_ID_GATTC, GATTC_WRITE_CMD, 1, 1, &args, &ret);

    return ret;
}

int bt_manager_gatt_client_write_long_request(int conn_id, bool reliable_writes, int char_handle, int offset, uint8_t *value, uint16_t len)
{
    int ret = BT_OK;

    gattc_write_long_req_args_t args;

    args.conn_id = conn_id;
    args.reliable_writes = reliable_writes;
    args.char_handle = char_handle;
    args.offset = offset;
    args.value = value;
    args.len = len;

    act_transfer(&ble_act_handle, ACT_ID_GATTC, GATTC_WRITE_LONG_REQ, 1, 1, &args, &ret);

    return ret;
}

int bt_manager_gatt_client_read_long_request(int conn_id, int char_handle, int offset)
{
    int ret = BT_OK;

    gattc_read_long_req_args_t args;
    args.conn_id = conn_id;
    args.char_handle = char_handle;
    args.offset = offset;

    act_transfer(&ble_act_handle, ACT_ID_GATTC, GATTC_READ_LONG_REQ, 1, 1, &args, &ret);

    return ret;
}

int bt_manager_gatt_client_read_request(int conn_id, int char_handle)
{
    int ret = BT_OK;

    gattc_read_req_args_t args;
    args.conn_id = conn_id;
    args.char_handle = char_handle;

    act_transfer(&ble_act_handle, ACT_ID_GATTC, GATTC_READ_REQ, 1, 1, &args, &ret);

    return ret;
}

const char *bt_manager_gatt_client_ecode_to_string(uint8_t ecode)
{
    return bt_gattc_ecode_to_string(ecode);
}

int bt_manager_gatt_client_get_mtu(int conn_id)
{
    int mtu = -1;

    act_transfer(&ble_act_handle, ACT_ID_GATTC, GATTC_GET_MTU, 1, 1, &conn_id, &mtu);

    return mtu;
}

void bt_manager_uuid_to_uuid128(const btmg_uuid_t *src, btmg_uuid_t *dst)
{
    bt_uuid_to_uuid128((const bt_uuid_t *)src, (bt_uuid_t *)dst);
}

int bt_manager_uuid_to_string(const btmg_uuid_t *uuid, char *str, size_t n)
{
    return bt_uuid_to_string((const bt_uuid_t *)uuid, str, n);
}

int bt_manager_gatt_client_discover_all_services(int conn_id, uint16_t start_handle,
                                                         uint16_t end_handle)
{
    int ret = BT_OK;

    gattc_dis_all_svs_args_t args;
    args.conn_id = conn_id;
    args.start_handle = start_handle;
    args.end_handle = end_handle;

    act_transfer(&ble_act_handle, ACT_ID_GATTC, GATTC_DIS_ALL_SVS, 1, 1, &args, &ret);

    return ret;
}
int bt_manager_gatt_client_discover_services_by_uuid(int conn_id, const char *uuid,
                                                             uint16_t start_handle,
                                                             uint16_t end_handle)
{
    int ret = BT_OK;

    gattc_dis_svs_by_uuid_args_t args;
    args.conn_id = conn_id;
    args.uuid = uuid;
    args.start_handle = start_handle;
    args.end_handle = end_handle;

    act_transfer(&ble_act_handle, ACT_ID_GATTC, GATTC_DIS_SVS_BY_UUID, 1, 1, &args, &ret);

    return ret;
}

int bt_manager_gatt_client_discover_service_all_char(int conn_id, void *svc_handle)
{
    int ret = BT_OK;

    gattc_dis_svc_all_char_args_t args;
    args.conn_id = conn_id;
    args.svc_handle = svc_handle;

    act_transfer(&ble_act_handle, ACT_ID_GATTC, GATTC_DIS_SVC_ALL_CHAR, 1, 1, &args, &ret);

    return ret;
}

int bt_manager_gatt_client_discover_char_all_descriptor(int conn_id, void *char_handle)
{
    int ret = BT_OK;

    gattc_dis_char_all_disc_args_t args;
    args.conn_id = conn_id;
    args.char_handle = char_handle;

    act_transfer(&ble_act_handle, ACT_ID_GATTC, GATTC_DIS_CHAR_ALL_DESC, 1, 1, &args, &ret);

    return ret;
}

int bt_manager_le_add_white_list(uint8_t *addr, btmg_le_addr_type_t addr_type)
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_LE_GAP, LE_ADD_WHITE_LIST, 2, 1, addr, &addr_type, &ret);

    return ret;
}

int bt_manager_le_rm_white_list(uint8_t *addr, btmg_le_addr_type_t addr_type)
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_LE_GAP, LE_RM_WHITE_LIST, 2, 1, addr, &addr_type, &ret);

    return ret;
}

int bt_manager_le_read_white_list_size(void)
{
    int size = -1;

    act_transfer(&ble_act_handle, ACT_ID_LE_GAP, LE_WHITE_LIST_SIZE, 0, 1, &size);

    return size;
}

int bt_manager_le_clear_white_list(void)
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_LE_GAP, LE_CLEAR_WHITE_LIST, 0, 1, &ret);

    return ret;
}

int bt_manager_le_scan_start(btmg_le_scan_param_t *scan_param)
{
    int ret = BT_OK;

    bt_le_scan_start_args_t args;
    args.scan_param = scan_param;

    act_transfer(&ble_act_handle, ACT_ID_LE_GAP, LE_SCAN_START, 1, 1, &args, &ret);

    return ret;
}

int bt_manager_le_scan_stop()
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_LE_GAP, LE_SCAN_STOP, 0, 1, &ret);

    return ret;
}

int bt_manager_le_disconnect_all(void)
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_GATTS, GATTS_DISCONNECT_ALL, 0, 1, &ret);

    return ret;
}

int bt_manager_le_disconnect(uint16_t conn_handle)
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_GATTS, GATTS_DISCONNECT, 1, 1, &conn_handle, &ret);

    return ret;
}

int bt_manager_le_set_scan_parameters(btmg_le_scan_param_t *scan_param)
{
    int ret = BT_OK;

    bt_le_set_scan_para_args_t args;
    args.scan_param = scan_param;

    act_transfer(&ble_act_handle, ACT_ID_LE_GAP, LE_SET_SCAN_PARA, 1, 1, &args, &ret);

    return ret;
}

int bt_manager_le_update_conn_params(btmg_le_conn_param_t *conn_params)
{
    int ret = BT_OK;

    bt_le_update_cn_para_args_t args;
    args.conn_params = conn_params;

    act_transfer(&ble_act_handle, ACT_ID_LE_GAP, LE_UPDATE_CN_PARA, 1, 1, &args, &ret);

    return ret;
}

int bt_manager_le5_set_ext_scan_params(const btmg_le5_ext_scan_params_t *params)
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_LE5_GAP, LE5_SET_EXT_SCAN_PARAMS, 1, 1, params, &ret);

    return ret;
}

int bt_manager_le5_start_ext_scan(btmg_le_scan_filter_duplicate_t scan_duplicate, uint32_t duration, uint16_t period)
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_LE5_GAP, LE5_START_EXT_SCAN, 3, 1, &scan_duplicate, &duration, &period, &ret);

    return ret;
}

int bt_manager_le5_stop_ext_scan(void)
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_LE5_GAP, LE5_STOP_EXT_SCAN, 0, 1, &ret);

    return ret;
}

int bt_manager_le5_read_phy(uint16_t handle, btmg_le5_read_phys_t *phys)
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_LE5_GAP, LE5_READ_PHY, 1, 2, &handle, &ret, phys);

    return ret;
}

int bt_manager_le5_set_phy(uint16_t handle, btmg_le5_set_phys_t param)
{
    int ret = BT_OK;

    act_transfer(&ble_act_handle, ACT_ID_LE5_GAP, LE5_SET_PHY, 2, 1, &handle, &param, &ret);

    return ret;
}

void bt_manager_hex_dump(char *pref, int width, unsigned char *buf, int len)
{
    int i, n;

    for (i = 0, n = 1; i < len; i++, n++) {
        if (n == 1)
            printf("%s ", pref);
        printf("%2.2X ", buf[i]);
        if (n == width) {
            printf("\n");
            n = 0;
        }
    }
    if (i && n != 1)
        printf("\n");
}
#endif