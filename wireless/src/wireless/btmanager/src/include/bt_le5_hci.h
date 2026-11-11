/*
* Copyright (c) 2018-2022 Allwinner Technology Co. Ltd. All rights reserved.
* Author: BT Team
* Date: 2022.03.12
*/

#ifndef __HCI_BLE5_H
#define __HCI_BLE5_H

#include <stdint.h>
#include <stdbool.h>

#if __cplusplus
extern "C" {
#endif

#define hci_opcode_ogf(op)		(op >> 10)
#define hci_opcode_ocf(op)		(op & 0x03ff)

#define MAX_BLE_CTL_EXT_ADV_DATA_LEN 251 // the correct value is queried from the controller via HCI (max supported length? 0x203a)
#define MAX_BLE_EXT_ADV_DATA_LEN   251
#define MAX_BLE_PERIODIC_ADV_DATA_LEN 252


#define HCI_OP_LE_SET_DATA_LEN		0x2022

typedef struct {
	uint16_t	handle;
	uint16_t	tx_len;
	uint16_t	tx_time;
} __attribute__ ((packed)) hci_cp_le_set_data_len;

typedef struct {
	uint8_t	status;
	uint16_t	handle;
} __attribute__ ((packed)) hci_rp_le_set_data_len;

#define HCI_OP_LE_READ_PHY		0x2030

typedef struct {
	uint16_t  handle;
} __attribute__ ((packed)) hci_cp_le_read_phy;

typedef struct {
	uint8_t		status;
	uint16_t	handle;
	uint8_t		tx_phy;
	uint8_t		rx_phy;
} __attribute__ ((packed)) hci_rp_le_read_phy;

#define HCI_OP_LE_SET_PHY		0x2032

typedef struct {
	uint16_t	handle;
	uint8_t		all_phys;
	uint8_t		tx_phys;
	uint8_t		rx_phys;
	uint16_t	phy_options;
} __attribute__ ((packed)) hci_cp_le_set_phy;

#define HCI_OP_LE_SET_ADV_SET_RAND_ADDR		0x2035

typedef struct {
	uint8_t handle;
	bdaddr_t bdaddr;
} __attribute__ ((packed)) hci_cp_le_set_adv_set_rand_addr;

#define HCI_OP_LE_SET_EXT_ADV_PARAMS		0x2036

typedef struct {
	uint8_t		handle;
	uint16_t	evt_properties;
	uint8_t		min_interval[3];
	uint8_t		max_interval[3];
	uint8_t		channel_map;
	uint8_t		own_addr_type;
	uint8_t		peer_addr_type;
	bdaddr_t 	peer_addr;
	uint8_t		filter_policy;
	uint8_t		tx_power;
	uint8_t		primary_phy;
	uint8_t		secondary_max_skip;
	uint8_t		secondary_phy;
	uint8_t		sid;
	uint8_t		notif_enable;
} __attribute__ ((packed)) hci_cp_le_set_ext_adv_params;

typedef struct {
	uint8_t  status;
	uint8_t  tx_power;
} __attribute__ ((packed)) hci_rp_le_set_ext_adv_params;

#define HCI_OP_LE_SET_EXT_ADV_DATA		0x2037

typedef struct {
	uint8_t  handle;
	uint8_t  operation;
	uint8_t  frag_pref;
	uint8_t  length;
	uint8_t  data[MAX_BLE_EXT_ADV_DATA_LEN];
} __attribute__ ((packed)) hci_cp_le_set_ext_adv_data;

#define HCI_OP_LE_SET_EXT_SCAN_RSP_DATA		0x2038

typedef struct {
	uint8_t  handle;
	uint8_t  operation;
	uint8_t  frag_pref;
	uint8_t  length;
	uint8_t  data[MAX_BLE_EXT_ADV_DATA_LEN];
} __attribute__ ((packed)) hci_cp_le_set_ext_scan_rsp_data;

#define HCI_OP_LE_SET_EXT_ADV_ENABLE		0x2039

typedef struct {
	uint8_t  handle;
	uint16_t  duration;
	uint8_t  max_events;
} __attribute__ ((packed)) hci_cp_ext_adv_set;

typedef struct {
	uint8_t  enable;
	uint8_t  num_of_sets;
	uint8_t  data[0];
} __attribute__ ((packed)) hci_cp_le_set_ext_adv_enable;

#define HCI_OP_LE_REMOVE_ADV_SET		0x203C

typedef struct {
	uint8_t  handle;
} __attribute__ ((packed)) hci_cp_le_remove_adv_set;

#define HCI_OP_LE_CLEAR_ADV_SET			0x203D

#define HCI_OP_LE_SET_PERIODIC_ADV_PARAMS			0x203E

typedef struct {
	uint8_t  handle;
	uint16_t  interval_min;
	uint16_t  interval_max;
	uint16_t  propertics;
} __attribute__ ((packed)) hci_cp_le_set_periodic_adv_params;

#define HCI_OP_LE_SET_PERIODIC_ADV_DATA			0x203F

typedef struct {
	uint8_t  handle;
	uint8_t  operation;
	uint8_t  length;
	uint8_t  data[MAX_BLE_PERIODIC_ADV_DATA_LEN];
} __attribute__ ((packed)) hci_cp_le_set_periodic_adv_data;

#define HCI_OP_LE_SET_PERIODIC_ADV_ENABLE		0x2040

typedef struct {
	uint8_t  enable;
	uint8_t  handle;
} __attribute__ ((packed)) hci_cp_le_set_periodic_adv_enable;

#define HCI_OP_LE_SET_EXT_SCAN_PARAMS   0x2041

typedef struct {
	uint8_t  own_addr_type;
	uint8_t  filter_policy;
	uint8_t  scanning_phys;
	uint8_t  data[0];
} __attribute__ ((packed)) hci_cp_le_set_ext_scan_params;

#define LE_SCAN_PHY_1M		0x01
#define LE_SCAN_PHY_2M		0x02
#define LE_SCAN_PHY_CODED	0x04

typedef struct {
	uint8_t  type;
	uint16_t  interval;
	uint16_t  window;
} __attribute__ ((packed)) hci_cp_le_scan_phy_params;

#define HCI_OP_LE_SET_EXT_SCAN_ENABLE   0x2042

typedef struct {
	uint8_t  enable;
	uint8_t  filter_dup;
	uint16_t  duration;
	uint16_t  period;
} __attribute__ ((packed)) hci_cp_le_set_ext_scan_enable;


#define HCI_OP_LE_EXT_CREATE_CONN    0x2043

typedef struct {
	uint8_t  filter_policy;
	uint8_t  own_addr_type;
	uint8_t  peer_addr_type;
	bdaddr_t  peer_addr;
	uint8_t  phys;
	uint8_t  data[0];
} __attribute__ ((packed)) hci_cp_le_ext_create_conn;

typedef struct {
	uint16_t  scan_interval;
	uint16_t  scan_window;
	uint16_t  conn_interval_min;
	uint16_t  conn_interval_max;
	uint16_t  conn_latency;
	uint16_t  supervision_timeout;
	uint16_t  min_ce_len;
	uint16_t  max_ce_len;
} __attribute__ ((packed)) hci_cp_le_ext_conn_param;





#include <api_action.h>
typedef enum {
    LE5_EXT_ADV_SET_RAND_ADDR = 0,
    LE5_EXT_ADV_SET_PARAMS,
    LE5_EXT_ADV_DATA,
    LE5_EXT_SCAN_RSP_DATA,
    LE5_EXT_ADV_ENABLE,
    LE5_EXT_ADV_SET_REMOVE,
    LE5_EXT_ADV_SET_CLEAR_ALL,
    LE5_PERIODIC_ADV_SET_PARAMS,
    LE5_PERIODIC_ADV_DATA,
    LE5_PERIODIC_ADV_ENABLE,
    LE5_SET_EXT_SCAN_PARAMS,
    LE5_START_EXT_SCAN,
    LE5_STOP_EXT_SCAN,
    LE5_DATA_LEN_UPDATE,
    LE5_READ_PHY,
    LE5_SET_PHY,
} le5_call_t;

extern act_func_t le5_gap_action_table[];

// int bt_le5_gap_periodic_adv_create_sync(const btmg_le5_gap_periodic_adv_sync_params_t *params);
// int bt_le5_gap_periodic_adv_sync_cancel(void);
// int bt_le5_gap_periodic_adv_sync_terminate(uint16_t sync_handle);
// int bt_le5_gap_periodic_adv_add_dev_to_list(btmg_le5_addr_type_t addr_type, const char *addr_str, uint8_t sid);
// int bt_le5_gap_periodic_adv_remove_dev_from_list(btmg_le5_addr_type_t addr_type, const char *addr_str, uint8_t sid);
// int bt_le5_gap_periodic_adv_clear_dev(void);

#if __cplusplus
}; // extern "C"
#endif

#endif
