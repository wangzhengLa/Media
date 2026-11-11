#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include "bt_le5_hci.h"
#include "bt_log.h"
#include "bt_manager.h"
#include "common.h"

#define BTMG_LE5_PRIM_ADV_INT_MIN            0x000020     /*!< Minimum advertising interval for undirected and low duty cycle directed advertising */
#define BTMG_LE5_PRIM_ADV_INT_MAX            0xFFFFFF     /*!< Maximum advertising interval for undirected and low duty cycle directed advertising */
#define BTMG_LE5_CONN_INT_MIN                0x0006
#define BTMG_LE5_CONN_INT_MAX                0x0C80
#define BTMG_LE5_CONN_LATENCY_MAX            499
#define BTMG_LE5_CONN_SUP_TOUT_MIN           0x000A
#define BTMG_LE5_CONN_SUP_TOUT_MAX           0x0C80
#define BTMG_LE5_IS_INVALID_PARAM(x, min, max)  ((x) < (min) || (x) > (max))

#define BTMG_LE5_ADV_DATA_OP_INTERMEDIATE_FRAG    0
#define BTMG_LE5_ADV_DATA_OP_FIRST_FRAG           1
#define BTMG_LE5_ADV_DATA_OP_LAST_FRAG            2
#define BTMG_LE5_ADV_DATA_OP_COMPLETE             3
#define BTMG_LE5_ADV_DATA_OP_UNCHANGED_DATA       4

#define MAX_BLE_ADV_INSTANCE 10 // the correct value is queried from the controller via HCI (max supported, 0x203b)

typedef struct {
    uint8_t    inst_id;
    bool       configured;
    bool       legacy_pdu;
    bool       directed;
    bool       scannable;
    bool       connetable;
} tBTMG_LE5_EXTENDED_INST;

typedef struct {
    tBTMG_LE5_EXTENDED_INST inst[MAX_BLE_ADV_INSTANCE]; /* dynamic array to store adv instance */
    uint8_t  scan_duplicate;
} tBTMG_LE5_EXTENDED_CB;

tBTMG_LE5_EXTENDED_CB extend_adv_cb;

int bt_le5_gap_ext_adv_set_params(void **call_args, void **cb_args)
{
    uint8_t instance = *(uint8_t *)call_args[0];
    btmg_le5_gap_ext_adv_params_t *params = (btmg_le5_gap_ext_adv_params_t *)call_args[1];

    hci_cp_le_set_ext_adv_params cp;
    struct hci_request rq;
    hci_rp_le_set_ext_adv_params rp;
    int hdev = 0;
    int s;
    bdaddr_t rand_addr;

    if(BTMG_LE5_IS_INVALID_PARAM(params->interval_min, BTMG_LE5_PRIM_ADV_INT_MIN, BTMG_LE5_PRIM_ADV_INT_MAX) ||
        BTMG_LE5_IS_INVALID_PARAM(params->interval_max, BTMG_LE5_PRIM_ADV_INT_MIN, BTMG_LE5_PRIM_ADV_INT_MAX) ||
        instance >= MAX_BLE_ADV_INSTANCE) {
        BTMG_ERROR("invalid params");
        RETURN_INT(BT_ERROR);
    }

    if (params->type & BTMG_LE5_GAP_SET_EXT_ADV_PROP_LEGACY) {
        /* Not allowed for legacy PDUs. */
        if (params->type & BTMG_LE5_GAP_SET_EXT_ADV_PROP_INCLUDE_TX_PWR) {
            BTMG_ERROR("The Legacy adv can't include tx power bit");
            RETURN_INT(BT_ERROR);
        }
    }

    if (!(params->type & BTMG_LE5_GAP_SET_EXT_ADV_PROP_LEGACY)) {
        /* Not allowed for extended advertising PDUs */
        if ((params->type & BTMG_LE5_GAP_SET_EXT_ADV_PROP_CONNECTABLE) &&
            (params->type & BTMG_LE5_GAP_SET_EXT_ADV_PROP_SCANNABLE)) {
            BTMG_ERROR("For the Extend adv, the properties can't be connectable and scannable at the same time");
            RETURN_INT(BT_ERROR);
        }

        /* HD directed advertising allowed only for legacy PDUs */
        if (params->type & BTMG_LE5_GAP_SET_EXT_ADV_PROP_HD_DIRECTED) {
            BTMG_ERROR("HD directed advertising allowed only for legacy PDUs");
            RETURN_INT(BT_ERROR);
        }
    }

    if (params->type & BTMG_LE5_GAP_SET_EXT_ADV_PROP_CONNECTABLE) {
        extend_adv_cb.inst[instance].connetable = true;
    } else {
        extend_adv_cb.inst[instance].connetable = false;
    }

    if (params->type & BTMG_LE5_GAP_SET_EXT_ADV_PROP_SCANNABLE) {
        extend_adv_cb.inst[instance].scannable = true;
    } else {
        extend_adv_cb.inst[instance].scannable = false;
    }

    if (params->type & BTMG_LE5_GAP_SET_EXT_ADV_PROP_LEGACY) {
        extend_adv_cb.inst[instance].legacy_pdu = true;
    } else {
        extend_adv_cb.inst[instance].legacy_pdu = false;
    }
    BTMG_INFO("instance:%d, %s %s %s", instance,
                extend_adv_cb.inst[instance].legacy_pdu ? "legacy" : "extend",
                extend_adv_cb.inst[instance].connetable ? "connetable" : "",
                extend_adv_cb.inst[instance].scannable ? "scannable" : "");

    memset(&cp, 0, sizeof(cp));
    cp.handle = instance;
    cp.evt_properties = params->type;
    cp.min_interval[0] = params->interval_min & 0x0000ff;
    cp.min_interval[1] = (params->interval_min & 0x00ff00) >> 8;
    cp.min_interval[2] = (params->interval_min & 0xff0000) >> 16;
    cp.max_interval[0] = params->interval_max & 0x0000ff;
    cp.max_interval[1] = (params->interval_max & 0x00ff00) >> 8;
    cp.max_interval[2] = (params->interval_max & 0xff0000) >> 16;
    cp.channel_map = params->channel_map;
    cp.own_addr_type = params->own_addr_type;
    cp.peer_addr_type = params->peer_addr_type;
    str2ba(params->peer_addr, &rand_addr);
    bacpy(&cp.peer_addr, &rand_addr);
    cp.filter_policy = params->filter_policy;
    cp.tx_power = params->tx_power;
    cp.primary_phy = params->primary_phy;
    cp.secondary_max_skip = params->max_skip;
    cp.secondary_phy = params->secondary_phy;
    cp.sid = params->sid;
    cp.notif_enable = params->scan_req_notif;

    memset(&rq, 0, sizeof(rq));
    rq.ogf = hci_opcode_ogf(HCI_OP_LE_SET_EXT_ADV_PARAMS);
    rq.ocf = hci_opcode_ocf(HCI_OP_LE_SET_EXT_ADV_PARAMS);
    rq.cparam = &cp;
    rq.clen   = sizeof(cp);
    rq.rparam = &rp;
    rq.rlen = sizeof(rp);
    if ((s = hci_open_dev(hdev)) < 0) {
        BTMG_ERROR("Can't open hci device");
        RETURN_INT(BT_ERROR);
    }
    if (hci_send_req(s, &rq, 2000) < 0) {
        BTMG_ERROR("hci_send_req fail");
        goto end;
    }
    if (rp.status) {
        BTMG_ERROR("status fail[%d]", rp.status);
        goto end;
    }
    extend_adv_cb.inst[instance].configured = true;
    hci_close_dev(s);
    RETURN_INT(BT_OK);
end:
    hci_close_dev(s);
    RETURN_INT(BT_ERROR);
}

int bt_le5_gap_ext_adv_set_rand_addr(void **call_args, void **cb_args)
{
    uint8_t instance = *(uint8_t *)call_args[0];

    hci_cp_le_set_adv_set_rand_addr cp;
    struct hci_request rq;
    uint8_t rp;
    int hdev = 0;
    int s;
    char addr[6];
    int urandom_fd;
    ssize_t len;

    if (instance >= MAX_BLE_ADV_INSTANCE) {
        BTMG_ERROR("ILLEGAL INSTANCE");
        RETURN_INT(BT_ERROR);
    }

    urandom_fd = open("/dev/urandom", O_RDONLY);
    if (urandom_fd < 0) {
        fprintf(stderr, "Failed to open /dev/urandom device\n");
        RETURN_INT(BT_ERROR);
    }

    len = read(urandom_fd, addr, sizeof(addr));
    if (len < 0 || len != sizeof(addr)) {
        fprintf(stderr, "Failed to read random data\n");
        close(urandom_fd);
        RETURN_INT(BT_ERROR);
    }
    close(urandom_fd);

    /* Clear top most significant bits */
    addr[5] &= 0x3f;

    memset(&cp, 0, sizeof(cp));
    cp.handle = instance;
    memcpy(cp.bdaddr.b, addr, 6);

    memset(&rq, 0, sizeof(rq));
    rq.ogf = hci_opcode_ogf(HCI_OP_LE_SET_ADV_SET_RAND_ADDR);
    rq.ocf = hci_opcode_ocf(HCI_OP_LE_SET_ADV_SET_RAND_ADDR);
    rq.cparam = &cp;
    rq.clen   = sizeof(cp);
    rq.rparam = &rp;
    rq.rlen = sizeof(rp);
    if ((s = hci_open_dev(hdev)) < 0) {
        BTMG_ERROR("Can't open hci device");
        RETURN_INT(BT_ERROR);
    }
    if (hci_send_req(s, &rq, 2000) < 0) {
        BTMG_ERROR("hci_send_req fail");
        goto end;
    }
    if (rp) {
        BTMG_ERROR("status fail[%d]", rp);
        goto end;
    }
    BTMG_INFO("instance: %d, RandomAddress: %02X:%02X:%02X:%02X:%02X:%02X ]", instance, addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    hci_close_dev(s);
    RETURN_INT(BT_OK);
end:
    hci_close_dev(s);
    RETURN_INT(BT_ERROR);
}

int bt_le5_ext_adv_set_data_validate(uint8_t instance, uint16_t length, const uint8_t *data)
{
    if (!data) {
        BTMG_ERROR("the extend adv data is NULL");
        return BT_ERROR;
    }

    if (instance >= MAX_BLE_ADV_INSTANCE) {
        BTMG_ERROR("instance Exceeded the maximum");
        return BT_ERROR;
    }

    if (!extend_adv_cb.inst[instance].configured) {
        BTMG_ERROR("The extend adv hasn't configured he ext adv parameters");
        return BT_ERROR;
    }
    /* Not allowed with the direted advertising for legacy */
    if (extend_adv_cb.inst[instance].legacy_pdu && extend_adv_cb.inst[instance].directed) {
        BTMG_ERROR("Not allowed with the direted advertising for legacy");
        return BT_ERROR;
    }
    /* Always allowed with legacy PDU but limited to legacy length */
    if (extend_adv_cb.inst[instance].legacy_pdu) {
        if (length > 31) {
            BTMG_ERROR("for the legacy adv, the adv data length can't exceed 31");
            return BT_ERROR;
        }
    } else {
        if (length > MAX_BLE_CTL_EXT_ADV_DATA_LEN) {
            BTMG_ERROR("The adv data len is longer then the controller adv max len %d", MAX_BLE_CTL_EXT_ADV_DATA_LEN);
            return BT_ERROR;
        }
    }
    return BT_OK;
}

int bt_le5_gap_config_ext_data_raw(bool is_scan_rsp, uint8_t instance, uint16_t length, const uint8_t *data)
{
    hci_cp_le_set_ext_adv_data cp;
    struct hci_request rq;
    uint8_t rp;
    int hdev = 0;
    int s;
    uint8_t send_data_len;
    uint16_t rem_len = length;
    uint8_t operation = 0;
    uint16_t data_offset = 0;

    if(bt_le5_ext_adv_set_data_validate(instance, length, data) != BT_OK) {
        BTMG_ERROR("invalid extend adv data.");
        return BT_ERROR;
    }

    if ((s = hci_open_dev(hdev)) < 0) {
        BTMG_ERROR("Can't open hci device");
        return BT_ERROR;
    }

    do {
        send_data_len = (rem_len > MAX_BLE_EXT_ADV_DATA_LEN) ? MAX_BLE_EXT_ADV_DATA_LEN : rem_len;
        if (length <= MAX_BLE_EXT_ADV_DATA_LEN) {
            operation = BTMG_LE5_ADV_DATA_OP_COMPLETE;
        } else {
            if (rem_len == length) {
                operation = BTMG_LE5_ADV_DATA_OP_FIRST_FRAG;
            } else if (rem_len <= MAX_BLE_EXT_ADV_DATA_LEN) {
                operation = BTMG_LE5_ADV_DATA_OP_LAST_FRAG;
            } else {
                operation = BTMG_LE5_ADV_DATA_OP_INTERMEDIATE_FRAG;
            }
        }
        memset(&cp, 0, sizeof(cp));
        cp.handle = instance;
        cp.operation = operation;
        cp.frag_pref = 0;
        cp.length = send_data_len;
        memset(cp.data, 0, sizeof(cp.data));
        memcpy(cp.data, &data[data_offset], send_data_len);

        memset(&rq, 0, sizeof(rq));
        if (!is_scan_rsp) {
            rq.ogf = hci_opcode_ogf(HCI_OP_LE_SET_EXT_ADV_DATA);
            rq.ocf = hci_opcode_ocf(HCI_OP_LE_SET_EXT_ADV_DATA);
        }
        else {
            rq.ogf = hci_opcode_ogf(HCI_OP_LE_SET_EXT_SCAN_RSP_DATA);
            rq.ocf = hci_opcode_ocf(HCI_OP_LE_SET_EXT_SCAN_RSP_DATA);
        }
        rq.cparam = &cp;
        rq.clen   = sizeof(cp);
        rq.rparam = &rp;
        rq.rlen = sizeof(rp);

        if (hci_send_req(s, &rq, 2000) < 0) {
            BTMG_ERROR("hci_send_req fail");
            goto end;
        }
        if (rp) {
            BTMG_ERROR("status fail[%d]", rp);
            goto end;
        }
        rem_len -= send_data_len;
        data_offset += send_data_len;
    } while (rem_len);

    hci_close_dev(s);
    return BT_OK;
end:
    hci_close_dev(s);
    return BT_ERROR;
}

int bt_le5_gap_ext_adv_data_raw(void **call_args, void **cb_args)
{
    uint8_t instance = *(uint8_t *)call_args[0];
    uint16_t length = *(uint16_t *)call_args[1];
    const uint8_t *data = (const uint8_t *)call_args[2];
    BTMG_DEBUG("instance = %d, len = %d", instance, length);

    int ret = bt_le5_gap_config_ext_data_raw(0, instance, length, data);
    RETURN_INT(ret);
}

int bt_le5_gap_ext_scan_rsp_data_raw(void **call_args, void **cb_args)
{
    uint8_t instance = *(uint8_t *)call_args[0];
    uint16_t length = *(uint16_t *)call_args[1];
    const uint8_t *scan_rsp_data = (const uint8_t *)call_args[2];
    BTMG_DEBUG("instance = %d, len = %d", instance, length);

    int ret = bt_le5_gap_config_ext_data_raw(1, instance, length, scan_rsp_data);
    RETURN_INT(ret);
}

int bt_le5_gap_ext_adv_enable(void **call_args, void **cb_args)
{
    bool enable = *(bool *)call_args[0];
    uint8_t num = *(uint8_t *)call_args[1];
    const btmg_le5_gap_ext_adv_t *ext_adv = (const btmg_le5_gap_ext_adv_t *)call_args[2];
    BTMG_INFO("enable: %d, num: %d", enable, num);

    hci_cp_le_set_ext_adv_enable *cp;
    hci_cp_ext_adv_set *adv_set;
    struct hci_request rq;
    uint8_t rp;
    int hdev = 0;
    int s;
	uint8_t data[sizeof(*cp) + sizeof(*adv_set) * num];

    // when enable = true, ext_adv = NULL or num = 0, goto end
    if ((!ext_adv || num == 0) && enable) {
        BTMG_ERROR("invalid parameters");
        RETURN_INT(BT_ERROR);
    }

    if ((s = hci_open_dev(hdev)) < 0) {
        BTMG_ERROR("Can't open hci device");
        RETURN_INT(BT_ERROR);
    }

	cp = (void *) data;
	adv_set = (void *) cp->data;

	memset(cp, 0, sizeof(*cp));
	cp->enable = enable;
	cp->num_of_sets = num;
	memset(adv_set, 0, sizeof(*adv_set)* num);
    for(int i = 0; i < num; i++) {
        adv_set[i].handle = ext_adv[i].instance;
        adv_set[i].duration = ext_adv[i].duration;
        adv_set[i].max_events = ext_adv[i].max_events;
    }

    memset(&rq, 0, sizeof(rq));
    rq.ogf = hci_opcode_ogf(HCI_OP_LE_SET_EXT_ADV_ENABLE);
    rq.ocf = hci_opcode_ocf(HCI_OP_LE_SET_EXT_ADV_ENABLE);
    rq.cparam = cp;
    rq.clen   = sizeof(*cp) + sizeof(*adv_set) * num;
    rq.rparam = &rp;
    rq.rlen = sizeof(rp);
    if (hci_send_req(s, &rq, 2000) < 0) {
        BTMG_ERROR("hci_send_req fail");
        goto end;
    }
    if (rp) {
        BTMG_ERROR("status fail[%d]", rp);
        goto end;
    }

    hci_close_dev(s);
    RETURN_INT(BT_OK);
end:
    hci_close_dev(s);
    RETURN_INT(BT_ERROR);
}

int bt_le5_gap_ext_adv_set_remove(void **call_args, void **cb_args)
{
    uint8_t instance = *(uint8_t *)call_args[0];

    hci_cp_le_remove_adv_set cp;
    struct hci_request rq;
    uint8_t rp;
    int hdev = 0;
    int s;
    if (instance >= MAX_BLE_ADV_INSTANCE) {
        BTMG_ERROR("instance Exceeded the maximum");
        RETURN_INT(BT_ERROR);
    }

    memset(&cp, 0, sizeof(cp));
    cp.handle = instance;

    memset(&rq, 0, sizeof(rq));
    rq.ogf = hci_opcode_ogf(HCI_OP_LE_REMOVE_ADV_SET);
    rq.ocf = hci_opcode_ocf(HCI_OP_LE_REMOVE_ADV_SET);
    rq.cparam = &cp;
    rq.clen   = sizeof(cp);
    rq.rparam = &rp;
    rq.rlen = sizeof(rp);
    if ((s = hci_open_dev(hdev)) < 0) {
        BTMG_ERROR("Can't open hci device");
        RETURN_INT(BT_ERROR);
    }
    if (hci_send_req(s, &rq, 2000) < 0) {
        BTMG_ERROR("hci_send_req fail");
        goto end;
    }
    if (rp) {
        BTMG_ERROR("status fail[%d]", rp);
        goto end;
    }

    extend_adv_cb.inst[instance].configured = false;
    extend_adv_cb.inst[instance].legacy_pdu = false;
    extend_adv_cb.inst[instance].directed = false;
    extend_adv_cb.inst[instance].scannable = false;
    extend_adv_cb.inst[instance].connetable = false;
    hci_close_dev(s);
    RETURN_INT(BT_OK);
end:
    hci_close_dev(s);
    RETURN_INT(BT_ERROR);
}

int bt_le5_gap_ext_adv_set_clear_all(void **call_args, void **cb_args)
{
    struct hci_request rq;
    uint8_t rp;
    int hdev = 0;
    int s;

    memset(&rq, 0, sizeof(rq));
    rq.ogf = hci_opcode_ogf(HCI_OP_LE_CLEAR_ADV_SET);
    rq.ocf = hci_opcode_ocf(HCI_OP_LE_CLEAR_ADV_SET);
    rq.clen   = 0;
    rq.rparam = &rp;
    rq.rlen = sizeof(rp);
    if ((s = hci_open_dev(hdev)) < 0) {
        BTMG_ERROR("Can't open hci device");
        RETURN_INT(BT_ERROR);
    }
    if (hci_send_req(s, &rq, 2000) < 0) {
        BTMG_ERROR("hci_send_req fail");
        goto end;
    }
    if (rp) {
        BTMG_ERROR("status fail[%d]", rp);
        goto end;
    }

    for (uint8_t i = 0; i < MAX_BLE_ADV_INSTANCE; i++) {
        extend_adv_cb.inst[i].configured = false;
        extend_adv_cb.inst[i].legacy_pdu = false;
        extend_adv_cb.inst[i].directed = false;
        extend_adv_cb.inst[i].scannable = false;
        extend_adv_cb.inst[i].connetable = false;
    }
    hci_close_dev(s);
    RETURN_INT(BT_OK);
end:
    hci_close_dev(s);
    RETURN_INT(BT_ERROR);
}

int bt_le5_gap_periodic_adv_set_params(void **call_args, void **cb_args)
{
    uint8_t instance = *(uint8_t *)call_args[0];
    const btmg_le5_gap_periodic_adv_params_t *params = (btmg_le5_gap_periodic_adv_params_t *)call_args[1];
    BTMG_DEBUG("instance = %d", instance);

    hci_cp_le_set_periodic_adv_params cp;
    struct hci_request rq;
    uint8_t rp;
    int hdev = 0;
    int s;

    if (instance >= MAX_BLE_ADV_INSTANCE) {
        BTMG_ERROR("instance Exceeded the maximum");
        RETURN_INT(BT_ERROR);
    }

    if (!extend_adv_cb.inst[instance].configured ||
        extend_adv_cb.inst[instance].scannable ||
        extend_adv_cb.inst[instance].connetable ||
        extend_adv_cb.inst[instance].legacy_pdu) {
        BTMG_ERROR("instance = %d, Before set the periodic adv parameters, please configure the the \
                extend adv to nonscannable and nonconnectable first, and it shouldn't include the legacy bit.", instance);
        RETURN_INT(BT_ERROR);
    }

    memset(&cp, 0, sizeof(cp));
    cp.handle = instance;
    cp.interval_min = params->interval_min;
    cp.interval_max = params->interval_max;
    cp.propertics = params->properties;

    memset(&rq, 0, sizeof(rq));
    rq.ogf = hci_opcode_ogf(HCI_OP_LE_SET_PERIODIC_ADV_PARAMS);
    rq.ocf = hci_opcode_ocf(HCI_OP_LE_SET_PERIODIC_ADV_PARAMS);
    rq.cparam = &cp;
    rq.clen   = sizeof(cp);
    rq.rparam = &rp;
    rq.rlen = sizeof(rp);
    if ((s = hci_open_dev(hdev)) < 0) {
        BTMG_ERROR("Can't open hci device");
        RETURN_INT(BT_ERROR);
    }
    if (hci_send_req(s, &rq, 2000) < 0) {
        BTMG_ERROR("hci_send_req fail");
        goto end;
    }
    if (rp) {
        BTMG_ERROR("status fail[%d]", rp);
        goto end;
    }
    hci_close_dev(s);
    RETURN_INT(BT_OK);
end:
    hci_close_dev(s);
    RETURN_INT(BT_ERROR);
}

int bt_le5_gap_periodic_adv_data_raw(void **call_args, void **cb_args)
{
    uint8_t instance = *(uint8_t *)call_args[0];
    uint8_t length = *(uint8_t *)call_args[1];
    const uint8_t *data = (uint8_t *)call_args[2];
    BTMG_DEBUG("instance = %d, length = %d", instance, length);

    hci_cp_le_set_periodic_adv_data cp;
    struct hci_request rq;
    uint8_t rp;
    int hdev = 0;
    int s;
    uint8_t send_data_len;
    uint16_t rem_len = length;
    uint8_t operation = 0;
    uint16_t data_offset = 0;

    if(bt_le5_ext_adv_set_data_validate(instance, length, data) != BT_OK) {
        BTMG_ERROR("invalid extend adv data.");
        RETURN_INT(BT_ERROR);
    }

    if ((s = hci_open_dev(hdev)) < 0) {
        BTMG_ERROR("Can't open hci device");
        RETURN_INT(BT_ERROR);
    }

    do {
        send_data_len = (rem_len > MAX_BLE_PERIODIC_ADV_DATA_LEN) ? MAX_BLE_PERIODIC_ADV_DATA_LEN : rem_len;
        if (length <= MAX_BLE_EXT_ADV_DATA_LEN) {
            operation = BTMG_LE5_ADV_DATA_OP_COMPLETE;
        } else {
            if (rem_len == length) {
                operation = BTMG_LE5_ADV_DATA_OP_FIRST_FRAG;
            } else if (rem_len <= MAX_BLE_EXT_ADV_DATA_LEN) {
                operation = BTMG_LE5_ADV_DATA_OP_LAST_FRAG;
            } else {
                operation = BTMG_LE5_ADV_DATA_OP_INTERMEDIATE_FRAG;
            }
        }
        memset(&cp, 0, sizeof(cp));
        cp.handle = instance;
        cp.operation = operation;
        cp.length = send_data_len;
        memset(cp.data, 0, sizeof(cp.data));
        memcpy(cp.data, &data[data_offset], send_data_len);

        memset(&rq, 0, sizeof(rq));
        rq.ogf = hci_opcode_ogf(HCI_OP_LE_SET_PERIODIC_ADV_DATA);
        rq.ocf = hci_opcode_ocf(HCI_OP_LE_SET_PERIODIC_ADV_DATA);
        rq.cparam = &cp;
        rq.clen   = sizeof(cp);
        rq.rparam = &rp;
        rq.rlen = sizeof(rp);

        if (hci_send_req(s, &rq, 2000) < 0) {
            BTMG_ERROR("hci_send_req fail");
            goto end;
        }
        if (rp) {
            BTMG_ERROR("status fail[%d]", rp);
            goto end;
        }
        rem_len -= send_data_len;
        data_offset += send_data_len;
    } while (rem_len);

    hci_close_dev(s);
    RETURN_INT(BT_OK);
end:
    hci_close_dev(s);
    RETURN_INT(BT_ERROR);
}

int bt_le5_gap_periodic_adv_enable(void **call_args, void **cb_args)
{
    bool enable = *(bool *)call_args[0];
    uint8_t instance = *(uint8_t *)call_args[1];
    BTMG_DEBUG("instance = %d, enable = %d", instance, enable);

    hci_cp_le_set_periodic_adv_enable cp;
    struct hci_request rq;
    uint8_t rp;
    int hdev = 0;
    int s;

    if (instance >= MAX_BLE_ADV_INSTANCE) {
        BTMG_ERROR("instance Exceeded the maximum");
        RETURN_INT(BT_ERROR);
    }

    memset(&cp, 0, sizeof(cp));
    cp.enable = enable;
    cp.handle = instance;

    memset(&rq, 0, sizeof(rq));
    rq.ogf = hci_opcode_ogf(HCI_OP_LE_SET_PERIODIC_ADV_ENABLE);
    rq.ocf = hci_opcode_ocf(HCI_OP_LE_SET_PERIODIC_ADV_ENABLE);
    rq.cparam = &cp;
    rq.clen   = sizeof(cp);
    rq.rparam = &rp;
    rq.rlen = sizeof(rp);
    if ((s = hci_open_dev(hdev)) < 0) {
        BTMG_ERROR("Can't open hci device");
        RETURN_INT(BT_ERROR);
    }
    if (hci_send_req(s, &rq, 2000) < 0) {
        BTMG_ERROR("hci_send_req fail");
        goto end;
    }
    if (rp) {
        BTMG_ERROR("status fail[%d]", rp);
        goto end;
    }

    hci_close_dev(s);
    RETURN_INT(BT_OK);
end:
    hci_close_dev(s);
    RETURN_INT(BT_ERROR);
}

int bt_le5_gap_set_ext_scan_params(void **call_args, void **cb_args)
{
    const btmg_le5_ext_scan_params_t *params = (const btmg_le5_ext_scan_params_t *)call_args[0];

    hci_cp_le_set_ext_scan_params *ext_param_cp;
    hci_cp_le_scan_phy_params *phy_params;
    uint32_t plen;
    uint8_t data[sizeof(*ext_param_cp) + sizeof(*phy_params) * 2];

    struct hci_request rq;
    uint8_t rp;
    int hdev = 0;
    int s;

    if (!params) {
        BTMG_ERROR("invalid parameters");
    }

    ext_param_cp = (void *)data;
    phy_params = (void *)ext_param_cp->data;

    memset(ext_param_cp, 0, sizeof(*ext_param_cp));
    ext_param_cp->own_addr_type = params->own_addr_type;
    ext_param_cp->filter_policy = params->filter_policy;
    plen = sizeof(*ext_param_cp);
    if (params->cfg_mask & BTMG_LE5_GAP_EXT_SCAN_CFG_UNCODE_MASK) {
        ext_param_cp->scanning_phys |= LE_SCAN_PHY_1M;
        memset(phy_params, 0, sizeof(*phy_params));
        phy_params->type = params->uncoded_cfg.scan_type;
        phy_params->interval = params->uncoded_cfg.scan_interval;
        phy_params->window = params->uncoded_cfg.scan_window;
        plen += sizeof(*phy_params);
        phy_params++;
        BTMG_INFO("LE SCAN 1M PHY, %s interval:0x%02x window:0x%02x", phy_params->type ? "active":"passtive", phy_params->interval, phy_params->window);
    }

    if (params->cfg_mask & BTMG_LE5_GAP_EXT_SCAN_CFG_CODE_MASK) {
        ext_param_cp->scanning_phys |= LE_SCAN_PHY_CODED;
        memset(phy_params, 0, sizeof(*phy_params));
        phy_params->type = params->coded_cfg.scan_type;
        phy_params->interval = params->coded_cfg.scan_interval;
        phy_params->window = params->coded_cfg.scan_window;
        plen += sizeof(*phy_params);
        phy_params++;
        BTMG_INFO("LE SCAN CODED PHY, interval:0x%02x window:0x%02x", phy_params->interval, phy_params->window);
    }

    // if (BTM_BleUpdateOwnType(&params->own_addr_type, NULL) != 0 ) {
    //     BTMG_ERROR("LE UpdateOwnType err");
    // }

    memset(&rq, 0, sizeof(rq));
    rq.ogf = hci_opcode_ogf(HCI_OP_LE_SET_EXT_SCAN_PARAMS);
    rq.ocf = hci_opcode_ocf(HCI_OP_LE_SET_EXT_SCAN_PARAMS);
    rq.cparam = ext_param_cp;
    rq.clen   = plen;
    rq.rparam = &rp;
    rq.rlen = sizeof(rp);
    if ((s = hci_open_dev(hdev)) < 0) {
        BTMG_ERROR("Can't open hci device");
        RETURN_INT(BT_ERROR);
    }
    if (hci_send_req(s, &rq, 2000) < 0) {
        BTMG_ERROR("hci_send_req fail");
        goto end;
    }
    if (rp) {
        BTMG_ERROR("status fail[%d]", rp);
        goto end;
    }

    hci_close_dev(s);
    RETURN_INT(BT_OK);
end:
    hci_close_dev(s);
    RETURN_INT(BT_ERROR);
}

int bt_le5_gap_ext_scan(bool enable, btmg_le_scan_filter_duplicate_t scan_duplicate, uint32_t duration, uint16_t period)
{
    hci_cp_le_set_ext_scan_enable cp;
    struct hci_request rq;
    uint8_t rp;
    int hdev = 0;
    int s;

    memset(&cp, 0, sizeof(cp));
    cp.enable = enable;
    cp.filter_dup = scan_duplicate;
    cp.duration = duration;
    cp.period = period;

    memset(&rq, 0, sizeof(rq));
    rq.ogf = hci_opcode_ogf(HCI_OP_LE_SET_EXT_SCAN_ENABLE);
    rq.ocf = hci_opcode_ocf(HCI_OP_LE_SET_EXT_SCAN_ENABLE);
    rq.cparam = &cp;
    rq.clen   = sizeof(cp);
    rq.rparam = &rp;
    rq.rlen = sizeof(rp);
    if ((s = hci_open_dev(hdev)) < 0) {
        BTMG_ERROR("Can't open hci device");
        return BT_ERROR;
    }
    if (hci_send_req(s, &rq, 2000) < 0) {
        BTMG_ERROR("hci_send_req fail");
        goto end;
    }
    if (rp) {
        BTMG_ERROR("status fail[%d]", rp);
        goto end;
    }

    hci_close_dev(s);
    return BT_OK;
end:
    hci_close_dev(s);
    return BT_ERROR;
}

int bt_le5_gap_start_ext_scan(void **call_args, void **cb_args)
{
    btmg_le_scan_filter_duplicate_t scan_duplicate = *(btmg_le_scan_filter_duplicate_t *)call_args[0];
    uint32_t duration = *(uint32_t *)call_args[1];
    uint16_t period = *(uint16_t *)call_args[2];
    BTMG_INFO("start extend scan");

    int ret = bt_le5_gap_ext_scan(1, scan_duplicate, duration, period);
    RETURN_INT(ret);
}

int bt_le5_gap_stop_ext_scan(void **call_args, void **cb_args)
{
    BTMG_INFO("stop extend scan");
    /* If it is set to 0x00, the remaining parameters shall be ignored.*/
    int ret = bt_le5_gap_ext_scan(0, 0, 0, 0);
    RETURN_INT(ret);
}

int bt_le5_read_phy(void **call_args, void **cb_args)
{
    uint16_t *conn_handle = (uint16_t *)call_args[0];
    btmg_le5_read_phys_t *phys = (btmg_le5_read_phys_t *)cb_args[1];

    hci_cp_le_read_phy cp;
    hci_rp_le_read_phy rp;
    struct hci_request rq;
    int hdev = 0;
    int s;

    BTMG_DEBUG("read handle:%d phy", *conn_handle);

    memset(&cp, 0, sizeof(cp));
    cp.handle = *conn_handle;

    memset(&rq, 0, sizeof(rq));
    rq.ogf = hci_opcode_ogf(HCI_OP_LE_READ_PHY);
    rq.ocf = hci_opcode_ocf(HCI_OP_LE_READ_PHY);
    rq.cparam = &cp;
    rq.clen   = sizeof(cp);
    rq.rparam = &rp;
    rq.rlen = sizeof(rp);
    if ((s = hci_open_dev(hdev)) < 0) {
        BTMG_ERROR("Can't open hci device");
        return BT_ERROR;
    }
    if (hci_send_req(s, &rq, 2000) < 0) {
        BTMG_ERROR("hci_send_req fail");
        goto end;
    }
    if (rp.status) {
        BTMG_ERROR("status fail[%d]", rp);
        goto end;
    }

    phys->tx_phy = rp.tx_phy;
    phys->rx_phy = rp.rx_phy;

    BTMG_DEBUG("return tx phy:%s",  (phys->tx_phy == 1) ? "1M" :
                                    (phys->tx_phy == 2) ? "2M" :
                                    (phys->tx_phy == 3) ? "CODED" : "Invalid value");
    BTMG_DEBUG("return rx phy:%s",  (phys->rx_phy == 1) ? "1M" :
                                    (phys->rx_phy == 2) ? "2M" :
                                    (phys->rx_phy == 3) ? "CODED" : "Invalid value");

    hci_close_dev(s);
    RETURN_INT(BT_OK);
end:
    hci_close_dev(s);
    RETURN_INT(BT_ERROR);
}

int bt_le5_set_phy(void **call_args, void **cb_args)
{
    uint16_t *conn_handle = (uint16_t *)call_args[0];
    btmg_le5_set_phys_t *param_phy = (btmg_le5_set_phys_t *)call_args[1];

    hci_cp_le_set_phy cp;
    struct hci_request rq;
    uint8_t rp;
    int hdev = 0;
    int s;

    memset(&cp, 0, sizeof(cp));
    cp.handle = *conn_handle;

    switch (*param_phy) {
    case BTMG_LE5_SET_1M_PHY:
        BTMG_INFO("change to 1M phy");
        cp.all_phys = 0;
        cp.tx_phys = 1;
        cp.rx_phys = 1;
        cp.phy_options = 0;
        break;
    case BTMG_LE5_SET_2M_PHY:
        BTMG_INFO("change to 2M phy");
        cp.all_phys = 0;
        cp.tx_phys = 2;
        cp.rx_phys = 2;
        cp.phy_options = 0;
        break;
    case BTMG_LE5_SET_S2_PHY:
        BTMG_INFO("change to coded-s2 phy");
        cp.all_phys = 0;
        cp.tx_phys = 4;
        cp.rx_phys = 4;
        cp.phy_options = 1;
        break;
    case BTMG_LE5_SET_S8_PHY:
        BTMG_INFO("change to coded-s8 phy");
        cp.all_phys = 0;
        cp.tx_phys = 4;
        cp.rx_phys = 4;
        cp.phy_options = 2;
        break;
    default:
        BTMG_ERROR("param error");
    }

    memset(&rq, 0, sizeof(rq));
    rq.ogf = hci_opcode_ogf(HCI_OP_LE_SET_PHY);
    rq.ocf = hci_opcode_ocf(HCI_OP_LE_SET_PHY);
    rq.cparam = &cp;
    rq.clen   = sizeof(cp);
    rq.rparam = &rp;
    rq.rlen = sizeof(rp);
    if ((s = hci_open_dev(hdev)) < 0) {
        BTMG_ERROR("Can't open hci device");
        return BT_ERROR;
    }

    if (hci_send_cmd(s, rq.ogf, rq.ocf, rq.clen, rq.cparam) < 0) {
        BTMG_ERROR("hci_send_cmd fail");
        goto end;
    }
    // if (hci_send_req(s, &rq, 2000) < 0) {
    //     BTMG_ERROR("hci_send_req fail");
    //     goto end;
    // }
    // if (rp) {
    //     BTMG_ERROR("status fail[%d]", rp);
    //     goto end;
    // }

    hci_close_dev(s);
    RETURN_INT(BT_OK);
end:
    hci_close_dev(s);
    RETURN_INT(BT_ERROR);
}

act_func_t le5_gap_action_table[] = {
    [LE5_EXT_ADV_SET_RAND_ADDR]     = { bt_le5_gap_ext_adv_set_rand_addr,   "ext_adv_set_rand_addr" },
    [LE5_EXT_ADV_SET_PARAMS]        = { bt_le5_gap_ext_adv_set_params,      "ext_adv_set_params" },
    [LE5_EXT_ADV_DATA]              = { bt_le5_gap_ext_adv_data_raw,        "ext_adv_data_raw" },
    [LE5_EXT_SCAN_RSP_DATA]         = { bt_le5_gap_ext_scan_rsp_data_raw,   "ext_scan_rsp_data_raw" },
    [LE5_EXT_ADV_ENABLE]            = { bt_le5_gap_ext_adv_enable,          "ext_adv_enable" },
    [LE5_EXT_ADV_SET_REMOVE]        = { bt_le5_gap_ext_adv_set_remove,      "ext_adv_set_remove" },
    [LE5_EXT_ADV_SET_CLEAR_ALL]     = { bt_le5_gap_ext_adv_set_clear_all,   "ext_adv_set_clear_all" },
    [LE5_PERIODIC_ADV_SET_PARAMS]   = { bt_le5_gap_periodic_adv_set_params, "periodic_adv_set_params" },
    [LE5_PERIODIC_ADV_DATA]         = { bt_le5_gap_periodic_adv_data_raw,   "periodic_adv_data_raw" },
    [LE5_PERIODIC_ADV_ENABLE]       = { bt_le5_gap_periodic_adv_enable,     "periodic_adv_enable" },
    [LE5_SET_EXT_SCAN_PARAMS]       = { bt_le5_gap_set_ext_scan_params,     "set_ext_scan_params" },
    [LE5_START_EXT_SCAN]            = { bt_le5_gap_start_ext_scan,          "start_ext_scan" },
    [LE5_STOP_EXT_SCAN]             = { bt_le5_gap_stop_ext_scan,           "stop_ext_scan" },
    // [LE5_DATA_LEN_UPDATE]           = { bt_le5_data_len_update,         "data_len_update" },
    [LE5_READ_PHY]                  = { bt_le5_read_phy,                    "read phy" },
    [LE5_SET_PHY]                   = { bt_le5_set_phy,                     "set phy" },
};
