/*
* Copyright (c) 2018-2022 Allwinner Technology Co. Ltd. All rights reserved.
* Author: BT Team
* Date: 2022.03.12
* Description: gatt-server test demo.
*/

#include <stdio.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <getopt.h>
#include <bt_manager.h>

#include "bt_dev_list.h"
#include "bt_log.h"
#include "bt_test.h"

#define TEST_SERVICE_UUID ((char *)"1112")
#define TEST_CHAR_UUID1 ((char *)"2223")
#define TEST_CHAR_UUID2 ((char *)"3334")
#define DEV_NAME_CHAR_UUID2 ((char *)"5555")
#define CCCD_UUID ((char *)"2902")

#define TEST_DESC_UUID ((char *)"00006666-0000-1000-8000-00805F9B34FB")

#define NORDIC_UART_SERVICE_UUID ((char *)"6e400001-b5a3-f393-e0a9-e50e24dcca9e")
#define NORDIC_UART_CHAR_RX_UUID ((char *)"6e400002-b5a3-f393-e0a9-e50e24dcca9e")
#define NORDIC_UART_CHAR_TX_UUID ((char *)"6e400003-b5a3-f393-e0a9-e50e24dcca9e")

#define TEST_BLE_RATE  0

#define EXT_ADV_2M_INSTANCE     0
#define EXT_ADV_1M_INSTANCE     1
#define EXT_ADV_LEG_INSTANCE    2
#define EXT_ADV_CODE_INSTANCE   3

static uint16_t service_handle;
char device_name[28] = "aw-ble-test-007";
static uint16_t tmp_connection_handle;

static void bt_test_gatt_connection_cb(char *addr, gatts_connection_event_t event, int conn_handle)
{
    if (event == BT_GATT_CONNECTION) {
        printf("gatt server Connected: %s, connection handle: 0x%02X\n", addr, conn_handle);
        tmp_connection_handle = conn_handle;
    } else if (event == BT_GATT_DISCONNECT) {
        printf("gatt server Disconnected: %s (reason UNKONW)\n", addr);
    } else if (event == BT_GATT_CONNECT_FAIL) {
        printf("gatt server set connect failed: %s\n", addr);
    } else {
        printf("gatt server event unkown:%d\n", event);
    }
}

static void bt_test_gatt_add_service_cb(gatts_add_svc_msg_t *msg)
{
    if (msg != NULL) {
        service_handle = msg->svc_handle;
        printf("add service handle: %d, handle max number: %d\n",
                                service_handle, msg->handle_num);
    }
}

static int char_handle = 0;
static int dev_name_char_handle = 0;
static int notify_handle = 0;
static int indicate_handle = 0;

static void bt_test_gatts_add_char_cb(gatts_add_char_msg_t *msg)
{
    if (msg != NULL) {
        printf("add char,uuid: %s,chr handle is 0x%04x\n", msg->uuid, msg->char_handle);
        if (strcmp(msg->uuid, DEV_NAME_CHAR_UUID2) == 0) {
            dev_name_char_handle = msg->char_handle;
        } else if (strcmp(msg->uuid, TEST_CHAR_UUID2) == 0) {
            notify_handle = msg->char_handle;
        } else if (strcmp(msg->uuid, TEST_CHAR_UUID1) == 0) {
            indicate_handle = msg->char_handle;
        } else {
            char_handle = msg->char_handle;
        }
    }
}

static void bt_test_gatt_add_desc_cb(gatts_add_desc_msg_t *msg)
{
    if (msg != NULL) {
        printf("desc handle is 0x%04x\n", msg->desc_handle);
    }
}

static void bt_test_gatt_char_read_request_cb(gatts_char_read_req_t *chr_read)
{
    char value[1];
    static unsigned char count = 0;
    char dev_name[] = "aw_ble_test_1149";

    printf("trans_id:%d,attr_handle:0x%04x,offset:%d\n", chr_read->trans_id, chr_read->attr_handle,
           chr_read->offset);

    if (chr_read) {
        if (chr_read->attr_handle == dev_name_char_handle) {
            gatts_send_read_rsp_t data;
            data.trans_id = chr_read->trans_id;
            data.attr_handle = chr_read->attr_handle;
            data.status = 0x0b;
            data.auth_req = 0x00;
            value[0] = count;
            data.value = dev_name;
            data.value_len = strlen(dev_name);
            bt_manager_gatt_server_send_read_response(&data);
            return;
        }
        gatts_send_read_rsp_t data;
        data.trans_id = chr_read->trans_id;
        data.attr_handle = chr_read->attr_handle;
        data.status = 0x0b;
        data.auth_req = 0x00;
        value[0] = count;
        data.value = value;
        data.value_len = 1;
        bt_manager_gatt_server_send_read_response(&data);
        count++;
    }
}

static void bt_test_gatts_send_indication_cb(gatts_send_indication_t *send_ind)
{
    printf("enter_func %d\n", __LINE__);
}

static void bt_test_gatt_char_write_request_cb(gatts_char_write_req_t *char_write)
{
    int ret = 0;

#if TEST_BLE_RATE
    uint8_t read_buf[512];
    const int LOOP = 1000;
    static unsigned long int start_ms, end_ms= 0;
    struct timeval tv;

    if ((char_write->trans_id % LOOP) == 1) {
        gettimeofday(&tv, NULL);
        start_ms = tv.tv_sec * 1000 + tv.tv_usec / 1000;
    } else if ((char_write->trans_id % LOOP) == 0) {
        gettimeofday(&tv, NULL);
        end_ms = tv.tv_sec * 1000 + tv.tv_usec / 1000;
        BTMG_INFO("speed: %.2f KB/S", char_write->value_len * (LOOP - 1) * 1.0f / 1024 / (end_ms - start_ms) * 1000);
        memset(read_buf, 0, sizeof(read_buf));
        memcpy(read_buf, char_write->value, char_write->value_len);
        BTMG_INFO("attr_handle: 0x%04x, tran_id: %d, len: %d\n%s", char_write->attr_handle,
                char_write->trans_id, char_write->value_len, read_buf);
    }
#else
    BTMG_INFO("write need rsp: %d", char_write->need_rsp);
    if (char_write) {
        BTMG_INFO("attr_handle: 0x%04x, tran_id: %d, len: %d", char_write->attr_handle, char_write->trans_id, char_write->value_len);
        bt_manager_hex_dump((char *)" ", 20, (unsigned char *)char_write->value, char_write->value_len);
    }
#endif

    if (char_write->need_rsp) {
        gatts_write_rsp_t data;
        data.trans_id = char_write->trans_id;
        data.attr_handle = char_write->attr_handle;
        data.state = BT_GATT_SUCCESS;
        ret = bt_manager_gatt_server_send_write_response(&data);
        if (ret != 0)
            BTMG_DEBUG("send write response failed!\n");
        else
            BTMG_DEBUG("send write response success!\n");
    }
}

static void bt_test_gatt_desc_read_requset_cb(gatts_desc_read_req_t *desc_read)
{
    printf("enter\n");
    char value[1];
    static unsigned char count = 0;
    printf("enter\n");

    printf("trans_id:%d,attr_handle:0x%04x,offset:%d\n", desc_read->trans_id, desc_read->attr_handle,
           desc_read->offset);

    if (desc_read) {
        gatts_send_read_rsp_t data;
        data.trans_id = desc_read->trans_id;
        data.attr_handle = desc_read->attr_handle;
        data.status = 0x0b;
        data.auth_req = 0x00;
        value[0] = count;
        data.value = value;
        data.value_len = 1;
        bt_manager_gatt_server_send_read_response(&data);
        count++;
    }
}

static void bt_test_gatt_desc_write_request_cb(gatts_desc_write_req_t *desc_write)
{
    int ret = 0;

    printf("enter\n");
    printf("desc_write->need_rsp: %d\n", desc_write->need_rsp);
    printf("desc_write->attr_handle: 0x%04x\n", desc_write->attr_handle);

    if (desc_write->need_rsp) {
        gatts_write_rsp_t data;
        data.trans_id = desc_write->trans_id;
        data.attr_handle = desc_write->attr_handle;
        data.state = BT_GATT_SUCCESS;
        ret = bt_manager_gatt_server_send_write_response(&data);
        if (ret != 0)
            printf("send write response failed!\n");
        else
            printf("send write response success!\n");
    }
}

static void set_adv_param(void)
{
    btmg_le_advertising_parameters_t adv_params;

    adv_params.min_interval = 0x0020;
    adv_params.max_interval = 0x01E0; /*range from 0x0020 to 0x4000*/
    adv_params.own_addr_type = BTMG_LE_RANDOM_ADDRESS;
    adv_params.adv_type = BTMG_LE_ADV_IND; /*ADV_IND*/
    adv_params.chan_map =
            BTMG_LE_ADV_CHANNEL_ALL; /*0x07, *bit0:channel 37, bit1: channel 38, bit2: channel39*/
    adv_params.filter = BTMG_LE_PROCESS_ALL_REQ;
    bt_manager_le_set_adv_param(&adv_params);
}

static void le_set_adv_data(const char *ble_name, uint16_t uuid)
{
    int index, ret;
    char advdata[31] = { 0 };
    index = 0;

    advdata[index] = 0x02; /* flag len */
    advdata[index + 1] = 0x01; /* type for flag */
    advdata[index + 2] = 0x1A; //0x05

    index += advdata[index] + 1;

    advdata[index] = strlen(ble_name) + 1; /* name len */
    advdata[index + 1] = 0x09; /* type for local name */
    int name_len;
    name_len = strlen(ble_name);
    strcpy(&(advdata[index + 2]), ble_name);
    index += advdata[index] + 1;

    advdata[index] = 0x03; /* uuid len */
    advdata[index + 1] = 0x03; /* type for complete list of 16-bit uuid */
    advdata[index + 2] = (char)(uuid & 0xFF);
    advdata[index + 3] = (char)((uuid >> 8) & 0xFF);
    index += advdata[index] + 1;

    BTMG_INFO("ble name = [%s]", ble_name);

    if (config_ext_adv == 1) {
        bt_manager_le5_ext_adv_data_raw(2, sizeof(advdata), advdata);
        bt_manager_le5_ext_scan_rsp_data_raw(2, sizeof(advdata), advdata);
    } else {
        btmg_scan_rsp_data_t adv;
        adv.data_len = index;
        memcpy(adv.data, advdata, 31);
        bt_manager_le_set_scan_rsp_data(&adv);
        bt_manager_le_set_adv_data((btmg_adv_data_t *)&adv);
    }
}

void bt_gatt_server_register_callback(btmg_callback_t *cb)
{
    cb->btmg_gatt_server_cb.gatts_add_svc_cb = bt_test_gatt_add_service_cb;
    cb->btmg_gatt_server_cb.gatts_add_char_cb = bt_test_gatts_add_char_cb;
    cb->btmg_gatt_server_cb.gatts_add_desc_cb = bt_test_gatt_add_desc_cb;
    cb->btmg_gatt_server_cb.gatts_connection_event_cb = bt_test_gatt_connection_cb;
    cb->btmg_gatt_server_cb.gatts_char_read_req_cb = bt_test_gatt_char_read_request_cb;
    cb->btmg_gatt_server_cb.gatts_char_write_req_cb = bt_test_gatt_char_write_request_cb;
    cb->btmg_gatt_server_cb.gatts_desc_read_req_cb = bt_test_gatt_desc_read_requset_cb;
    cb->btmg_gatt_server_cb.gatts_desc_write_req_cb = bt_test_gatt_desc_write_request_cb;
    cb->btmg_gatt_server_cb.gatts_send_indication_cb = bt_test_gatts_send_indication_cb;
}

static int bt_gatt_server_register_bt_test_svc()
{
    gatts_add_svc_t svc;
    gatts_add_char_t chr1;
    gatts_add_char_t chr2;
    gatts_add_char_t chr3;
    gatts_add_desc_t desc1;
    gatts_add_desc_t desc2;

    printf("add service,uuid:%s\n", TEST_SERVICE_UUID);
    svc.number = 10;
    svc.uuid = TEST_SERVICE_UUID;
    svc.primary = true;
    bt_manager_gatt_server_create_service(&svc);

    chr1.permissions = BT_GATT_PERM_READ | BT_GATT_PERM_WRITE;
    chr1.properties = BT_GATT_CHAR_PROPERTY_READ | BT_GATT_CHAR_PROPERTY_WRITE |
                      BT_GATT_CHAR_PROPERTY_INDICATE;
    chr1.svc_handle = service_handle;
    chr1.uuid = TEST_CHAR_UUID1;
    bt_manager_gatt_server_add_characteristic(&chr1);

    desc1.permissions = BT_GATT_PERM_READ | BT_GATT_PERM_WRITE;
    desc1.uuid = CCCD_UUID;
    desc1.svc_handle = service_handle;
    bt_manager_gatt_server_add_descriptor(&desc1);

    chr2.permissions = BT_GATT_PERM_READ | BT_GATT_PERM_WRITE;
    chr2.properties =
            BT_GATT_CHAR_PROPERTY_READ | BT_GATT_CHAR_PROPERTY_WRITE | BT_GATT_CHAR_PROPERTY_NOTIFY;
    chr2.svc_handle = service_handle;
    chr2.uuid = TEST_CHAR_UUID2;
    bt_manager_gatt_server_add_characteristic(&chr2);

    desc2.permissions = BT_GATT_PERM_READ | BT_GATT_PERM_WRITE;
    desc2.uuid = CCCD_UUID;
    desc2.svc_handle = service_handle;
    bt_manager_gatt_server_add_descriptor(&desc2);

    chr3.permissions = BT_GATT_PERM_READ;
    chr3.properties = BT_GATT_CHAR_PROPERTY_READ;
    chr3.svc_handle = service_handle;
    chr3.uuid = DEV_NAME_CHAR_UUID2;
    bt_manager_gatt_server_add_characteristic(&chr3);

    return 0;
}

static int bt_gatt_server_register_ble_uart_svc()
{
    gatts_add_svc_t svc;
    gatts_add_char_t chr_rx;
    gatts_add_char_t chr_tx;
    gatts_add_char_t chr3;
    gatts_add_desc_t desc;

    printf("add service,uuid:%s\n", NORDIC_UART_SERVICE_UUID);
    svc.number = 10;
    svc.uuid = NORDIC_UART_SERVICE_UUID;
    svc.primary = true;
    bt_manager_gatt_server_create_service(&svc);

    chr_rx.permissions = BT_GATT_PERM_WRITE;
    chr_rx.properties = BT_GATT_CHAR_PROPERTY_WRITE_NO_RESPONSE | BT_GATT_CHAR_PROPERTY_WRITE;
    chr_rx.svc_handle = service_handle;
    chr_rx.uuid = NORDIC_UART_CHAR_RX_UUID;
    bt_manager_gatt_server_add_characteristic(&chr_rx);

    chr_tx.permissions = BT_GATT_PERM_READ;
    chr_tx.properties = BT_GATT_CHAR_PROPERTY_NOTIFY;
    chr_tx.svc_handle = service_handle;
    chr_tx.uuid = NORDIC_UART_CHAR_TX_UUID;
    bt_manager_gatt_server_add_characteristic(&chr_tx);

    desc.permissions = BT_GATT_PERM_READ | BT_GATT_PERM_WRITE;
    desc.uuid = CCCD_UUID;
    desc.svc_handle = service_handle;
    bt_manager_gatt_server_add_descriptor(&desc);

    return 0;
}

static void ble_config_multi_ext_adv(void)
{
    btmg_le5_gap_ext_adv_params_t ext_adv_params_2M = {
        .type = BTMG_LE5_GAP_SET_EXT_ADV_PROP_CONNECTABLE,
        .interval_min = 0x30,
        .interval_max = 0x50,
        .channel_map = BTMG_LE_ADV_CHANNEL_ALL,
        .own_addr_type = BTMG_LE_RANDOM_ADDRESS,
        .filter_policy = BTMG_LE_PROCESS_ALL_REQ,
        .tx_power = EXT_ADV_TX_PWR_NO_PREFERENCE,
        .primary_phy = BTMG_LE5_GAP_PHY_1M,
        .max_skip = 0,
        .secondary_phy = BTMG_LE5_GAP_PHY_2M,
        .sid = 0,
        .scan_req_notif = false,
    };

    btmg_le5_gap_ext_adv_params_t ext_adv_params_1M = {
        .type = BTMG_LE5_GAP_SET_EXT_ADV_PROP_SCANNABLE,
        .interval_min = 0x40,
        .interval_max = 0x80,
        .channel_map = BTMG_LE_ADV_CHANNEL_ALL,
        .own_addr_type = BTMG_LE_RANDOM_ADDRESS,
        .filter_policy = BTMG_LE_PROCESS_ALL_REQ,
        .tx_power = EXT_ADV_TX_PWR_NO_PREFERENCE,
        .primary_phy = BTMG_LE5_GAP_PHY_1M,
        .max_skip = 0,
        .secondary_phy = BTMG_LE5_GAP_PHY_1M,
        .sid = 1,
        .scan_req_notif = false,
    };

    btmg_le5_gap_ext_adv_params_t legacy_adv_params = {
        .type = BTMG_LE5_GAP_SET_EXT_ADV_PROP_LEGACY_IND,
        .interval_min = 0x30,
        .interval_max = 0x60,
        .channel_map = BTMG_LE_ADV_CHANNEL_ALL,
        .own_addr_type = BTMG_LE_RANDOM_ADDRESS,
        .filter_policy = BTMG_LE_PROCESS_ALL_REQ,
        .tx_power = EXT_ADV_TX_PWR_NO_PREFERENCE,
        .primary_phy = BTMG_LE5_GAP_PHY_1M,
        .max_skip = 0,
        .secondary_phy = BTMG_LE5_GAP_PHY_1M,
        .sid = 2,
        .scan_req_notif = false,
    };

    btmg_le5_gap_ext_adv_params_t ext_adv_params_coded = {
        .type = BTMG_LE5_GAP_SET_EXT_ADV_PROP_SCANNABLE,
        .interval_min = 0x50,
        .interval_max = 0x80,
        .channel_map = BTMG_LE_ADV_CHANNEL_ALL,
        .own_addr_type = BTMG_LE_RANDOM_ADDRESS,
        .filter_policy = BTMG_LE_PROCESS_ALL_REQ,
        .tx_power = EXT_ADV_TX_PWR_NO_PREFERENCE,
        .primary_phy = BTMG_LE5_GAP_PHY_1M,
        .max_skip = 0,
        .secondary_phy = BTMG_LE5_GAP_PHY_CODED,
        .sid = 3,
        .scan_req_notif = false,
    };

    uint8_t raw_adv_data_2m[] = {
            0x02, 0x01, 0x06,
            0x02, 0x0a, 0xeb,
            0x11, 0x09, 'A', 'W', 'A', '_', 'M', 'U', 'L', 'T', 'I', '_', 'A',
            'D', 'V', '_', '2', 'M'
    };

    uint8_t raw_scan_rsp_data_1m[] = {
            0x02, 0x01, 0x06,
            0x02, 0x0a, 0xeb,
            0x11, 0x09, 'A', 'W', 'A', '_', 'M', 'U', 'L', 'T', 'I', '_', 'A',
            'D', 'V', '_', '1', 'M'
    };

    uint8_t legacy_adv_data[] = {
            0x02, 0x01, 0x06,
            0x02, 0x0a, 0xeb,
            0x15, 0x09, 'A', 'W', 'A', '_', 'M', 'U', 'L', 'T', 'I', '_', 'A',
            'D', 'V', '_', 'L', 'E', 'G', 'A', 'C', 'Y'
    };

    uint8_t legacy_scan_rsp_data[] = {
            0x02, 0x01, 0x06,
            0x02, 0x0a, 0xeb,
            0x15, 0x09, 'A', 'W', 'A', '_', 'M', 'U', 'L', 'T', 'I', '_', 'A',
            'D', 'V', '_', 'L', 'E', 'G', 'A', 'C', 'Y'
    };

    uint8_t raw_scan_rsp_data_coded[] = {
            0x02, 0x01, 0x06,
            0x02, 0x0a, 0xeb,
            0x14, 0x09, 'A', 'W', 'A', '_', 'M', 'U', 'L', 'T', 'I', '_', 'A',
            'D', 'V', '_', 'C', 'O', 'D', 'E', 'D'
    };

    btmg_le5_gap_ext_adv_t ext_adv[4] = {
        // handle, duration, max_events
        {EXT_ADV_2M_INSTANCE, 0, 0},
        // {EXT_ADV_1M_INSTANCE, 0, 0},
        {EXT_ADV_LEG_INSTANCE, 0, 0},
        // {EXT_ADV_CODE_INSTANCE, 0, 0},
    };

    // 2M phy extend adv, Connectable advertising
    bt_manager_le5_ext_adv_set_params(EXT_ADV_2M_INSTANCE, &ext_adv_params_2M);
    bt_manager_le5_ext_adv_set_rand_addr(EXT_ADV_2M_INSTANCE);
    bt_manager_le5_ext_adv_data_raw(EXT_ADV_2M_INSTANCE, sizeof(raw_adv_data_2m), raw_adv_data_2m);

    // 1M phy extend adv, Scannable advertising
    // bt_manager_le5_ext_adv_set_params(EXT_ADV_1M_INSTANCE, &ext_adv_params_1M);
    // bt_manager_le5_ext_adv_set_rand_addr(EXT_ADV_1M_INSTANCE);
    // bt_manager_le5_ext_scan_rsp_data_raw(EXT_ADV_1M_INSTANCE, sizeof(raw_scan_rsp_data_1m), raw_scan_rsp_data_1m);

    // 1M phy legacy adv, ADV_IND
    bt_manager_le5_ext_adv_set_params(EXT_ADV_LEG_INSTANCE, &legacy_adv_params);
    bt_manager_le5_ext_adv_set_rand_addr(EXT_ADV_LEG_INSTANCE);
    bt_manager_le5_ext_adv_data_raw(EXT_ADV_LEG_INSTANCE, sizeof(legacy_adv_data), legacy_adv_data);
    bt_manager_le5_ext_scan_rsp_data_raw(EXT_ADV_LEG_INSTANCE, sizeof(legacy_scan_rsp_data), legacy_scan_rsp_data);

    // coded phy extend adv, Scannable advertising
    // bt_manager_le5_ext_adv_set_params(EXT_ADV_CODE_INSTANCE, &ext_adv_params_coded);
    // bt_manager_le5_ext_adv_set_rand_addr(EXT_ADV_CODE_INSTANCE);
    // bt_manager_le5_ext_scan_rsp_data_raw(EXT_ADV_CODE_INSTANCE, sizeof(raw_scan_rsp_data_coded), raw_scan_rsp_data_coded);

    // start all adv
    bt_manager_le5_ext_adv_enable(1, 2, ext_adv);
}

int bt_gatt_server_init()
{
    bt_manager_gatt_server_init();

    bt_gatt_server_register_bt_test_svc();
    bt_gatt_server_register_ble_uart_svc();

    if(config_ext_adv == 1) {
        ble_config_multi_ext_adv();
        strcpy(device_name, "AWA_MULTI_ADV_LEGACY");
    } else {
        set_adv_param();
        bt_manager_le_set_random_address();
        le_set_adv_data(device_name, 0);
        bt_manager_le_enable_adv(true);
    }

    return 0;
}

int bt_gatt_server_deinit()
{
    bt_manager_gatt_server_deinit();
    return 0;
}

static int bt_test_send_indication(int attr_handle, char *data, int len)
{
    char default_data[] = "hello";
    gatts_indication_data_t indication_data;
    indication_data.attr_handle = attr_handle;
    indication_data.value = data;
    indication_data.value_len = len;

    if (data == NULL || len == 0) {
        indication_data.value = default_data;
        indication_data.value_len = strlen(default_data);
    }
    bt_manager_gatt_server_send_indication(&indication_data);
    return 0;
}

static int bt_test_send_notify(int attr_handle, char *data, int len)
{
    char default_data[] = "hello";

    gatts_notify_data_t notify_data;
    notify_data.attr_handle = attr_handle;
    notify_data.value = data;
    notify_data.value_len = len;

    if (data == NULL || len == 0) {
        notify_data.value = default_data;
        notify_data.value_len = strlen(default_data);
    }
    bt_manager_gatt_server_send_notify(&notify_data);
    return 0;
}

static void cmd_gatts_init(int argc, char *args[])
{
    bt_gatt_server_init();
    return;
}

static void cmd_gatts_deinit(int argc, char *args[])
{
    bt_gatt_server_deinit();
    return;
}

static void cmd_ble_name(int argc, char *args[])
{
    btmg_le5_gap_ext_adv_t ext_adv_stop[4] = {
        // handle, duration, max_events
        {EXT_ADV_2M_INSTANCE, 0, 0},
        // {EXT_ADV_1M_INSTANCE, 0, 0},
        {EXT_ADV_LEG_INSTANCE, 0, 0},
        // {EXT_ADV_CODE_INSTANCE, 0, 0},
    };
    btmg_le5_gap_ext_adv_t ext_adv_start[4] = {
        // handle, duration, max_events
        {EXT_ADV_LEG_INSTANCE, 0, 0},
    };

    if (argc > 1) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }

    if (strlen(args[0]) > 27) {
        BTMG_INFO("name = [%s] exceeds limit", args[0]);
        return;
    }

    if (argc == 1) {
        strcpy(device_name, args[0]);
        BTMG_INFO("set name = %s", device_name);
        if(config_ext_adv == 1) {
            bt_manager_le5_ext_adv_enable(0, 2, ext_adv_stop);
            le_set_adv_data(device_name, 0);
            bt_manager_le5_ext_adv_enable(1, 1, ext_adv_start);
        } else {
            bt_manager_le_enable_adv(false);
            le_set_adv_data(device_name, 0);
            bt_manager_le_enable_adv(true);
        }
    } else if (argc == 0) {
        BTMG_INFO("get name = %s", device_name);
    }
    return;
}

static void cmd_ble_adv(int argc, char *args[])
{
    int value = 0;
    btmg_le5_gap_ext_adv_t ext_adv_stop[4] = {
        // handle, duration, max_events
        {EXT_ADV_2M_INSTANCE, 0, 0},
        // {EXT_ADV_1M_INSTANCE, 0, 0},
        {EXT_ADV_LEG_INSTANCE, 0, 0},
        // {EXT_ADV_CODE_INSTANCE, 0, 0},
    };
    btmg_le5_gap_ext_adv_t ext_adv_start[4] = {
        // handle, duration, max_events
        {EXT_ADV_LEG_INSTANCE, 0, 0},
    };

    if (argc != 1) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }

    value = atoi(args[0]);
    BTMG_INFO("Set ble adv enable :%d", value);
    if (value || strcmp(args[0], "on") == 0) {
        if(config_ext_adv == 1) {
            bt_manager_le5_ext_adv_enable(1, 1, ext_adv_start);
        } else {
            bt_manager_le_enable_adv(true);
        }
    } else {
        if(config_ext_adv == 1) {
            bt_manager_le5_ext_adv_enable(0, 2, ext_adv_stop);
        } else {
            bt_manager_le_enable_adv(false);
        }
    }
}

static void cmd_set_adv_int(int argc, char *args[])
{
    uint32_t interval_min = 0x0020; //*0.625=20ms
    uint32_t interval_max = 0x01E0; //*0.625=300ms

    if (argc != 2) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }

    if(config_ext_adv == 1) {
        BTMG_ERROR("ext_adv not yet supported");
        return;
    }

    interval_min = strtoul(args[0], NULL, 16);
    interval_max = strtoul(args[1], NULL, 16);
    BTMG_INFO("interval_min: 0x%x, interval_max:0x%x", interval_min, interval_max);

    bt_manager_le_enable_adv(false);
    /*range from 0x0020 to 0x4000*/
    btmg_le_advertising_parameters_t adv_params;
    adv_params.min_interval = interval_min;
    adv_params.max_interval = interval_max;
    adv_params.own_addr_type = BTMG_LE_RANDOM_ADDRESS;
    adv_params.adv_type = BTMG_LE_ADV_IND;
    adv_params.chan_map = BTMG_LE_ADV_CHANNEL_ALL;
    adv_params.filter = BTMG_LE_PROCESS_ALL_REQ;
    bt_manager_le_set_adv_param(&adv_params);
    bt_manager_le_enable_adv(true);
    return;
}

static void cmd_gatt_indicate(int argc, char *args[])
{
    static char data[512] = { 0 };
    int attr_handle = indicate_handle;
    int len = argc - 1;

    if (argc < 1) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }
    if (argc >= 2) {
        attr_handle = strtol(args[0], NULL, 0);
    }
    if (attr_handle == 0) {
        attr_handle = indicate_handle;
    }

    for (int i = 0; i < len; i++) {
        data[i] = (char)strtoul(args[i + 1], NULL, 16);
    }
    if (len == 0) {
        len = 1;
    }
    bt_test_send_indication(attr_handle, data, len);
    data[0]++;
}

static void cmd_gatt_notify(int argc, char *args[])
{
    static char data[512] = { 0 };
    int attr_handle = notify_handle;
    int len = argc - 1;

    if (argc < 1) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }
    if (argc >= 2) {
        attr_handle = strtol(args[0], NULL, 0);
    }
    if (attr_handle == 0) {
        attr_handle = notify_handle;
    }

    for (int i = 0; i < len; i++) {
        data[i] = (char)strtoul(args[i + 1], NULL, 16);
    }
    if (len == 0) {
        len = 1;
    }
    bt_test_send_notify(attr_handle, data, len);
    data[0]++;
}

static void cmd_ble_disconnect(int argc, char *args[])
{
    BTMG_DEBUG("disconnect all");
    bt_manager_le_disconnect_all();
}

static void cmd_read_phy(int argc, char *args[])
{
    int ret;
    btmg_le5_read_phys_t phys;

    ret = bt_manager_le5_read_phy(tmp_connection_handle, &phys);
    if (ret < 0) {
        BTMG_ERROR("read phy error");
    }

    BTMG_INFO("return tx phy:%s, rx phy:%s",  (phys.tx_phy == 1) ? "1M" :
                                    (phys.tx_phy == 2) ? "2M" :
                                    (phys.tx_phy == 3) ? "CODED" : "Invalid value",
                                    (phys.rx_phy == 1) ? "1M" :
                                    (phys.rx_phy == 2) ? "2M" :
                                    (phys.rx_phy == 3) ? "CODED" : "Invalid value");
}

static void cmd_set_phy(int argc, char *args[])
{
    int ret;
    btmg_le5_set_phys_t value;

    if (argc != 1) {
        BTMG_ERROR("Unexpected argc: %d, see help", argc);
        return;
    }
    value = (btmg_le5_set_phys_t)atoi(args[0]);

    ret = bt_manager_le5_set_phy(tmp_connection_handle, value);
    if (ret < 0) {
        BTMG_ERROR("set phy error");
    }
}

cmd_tbl_t bt_gatts_cmd_table[] = {
    { "gatts_init", NULL, cmd_gatts_init, "gatts_init: gatt server init" },
    { "gatts_deinit", NULL, cmd_gatts_deinit, "gatts_deinit:gatt server deinit" },
    { "ble_name", NULL, cmd_ble_name, "ble name [name/none] to set name or get name" },
    { "ble_advertise", NULL, cmd_ble_adv, "ble_advertise [0/1]/[off/on]: Disable/Enable ble advertising" },
    { "set_adv_int", NULL, cmd_set_adv_int, "set_adv_int [min_val] [max_val]: set advertise interval" },
    { "ble_disconnect_all", NULL, cmd_ble_disconnect, "ble_disconnect: disconnect all le connections" },
    { "gatt_indicate", NULL, cmd_gatt_indicate, "gatt_indicate <char_handle/0> [value]" },
    { "gatt_notify", NULL, cmd_gatt_notify, "gatt_notify <char_handle/0> [value]" },
    { "read_phy", NULL, cmd_read_phy, "read_phy" },
    { "set_phy", NULL, cmd_set_phy, "set_phy [1~4]:1-set 1M phy,2-set 2M phy,3-set coded-s2 phy,4-set coded-s8 phy" },
    { NULL, NULL, NULL, NULL },
};
