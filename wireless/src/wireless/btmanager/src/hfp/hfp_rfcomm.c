/*
* Copyright (c) 2018-2022 Allwinner Technology Co. Ltd. All rights reserved.
* Author: BT Team
* Date: 2023.07.06
* Description: hfp rfcomm function.
*/

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dbus/dbus.h>
#include <sys/prctl.h>
#include <sys/ioctl.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#include "bt_manager.h"
#include "bt_log.h"
#include "common.h"
#include "bt_pcm_supervise.h"
#include "defs.h"
#include "at.h"
#include "hfp_rfcomm.h"
#include "hfp_transport.h"

static bool main_loop_on = true;
static struct ba_rfcomm r;

static int rfcomm_write_at(int fd, enum btmg_at_type type, const char *command, const char *value);

/**
 * Structure used for buffered reading from blueasla io. */
struct at_reader {
    struct btmg_at_t at;
    char buffer[256];
    /* pointer to the next message within the buffer */
    char *next;
};

int bt_hfp_hf_send_at_ata(void)
{
    char *buf;
    buf = "ATA\r";

    return rfcomm_write_at(r.fd, BTMG_AT_TYPE_RAW, buf, NULL);
}

int bt_hfp_hf_send_at_chup(void)
{
    char *buf;
    buf = "+CHUP\r";

    return rfcomm_write_at(r.fd, BTMG_AT_TYPE_CMD, buf, NULL);
}

int bt_hfp_hf_send_at_atd(char *number)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "ATD%s;\r", number);

    return rfcomm_write_at(r.fd, BTMG_AT_TYPE_RAW, buf, NULL);
}

int bt_hfp_hf_send_at_bldn(void)
{
    char *buf;
    buf = "+BLDN\r";

    return rfcomm_write_at(r.fd, BTMG_AT_TYPE_CMD, buf, NULL);
}

int bt_hfp_hf_send_at_btrh(bool query, uint32_t val)
{
    char buf[128];

    if (query) {
        snprintf(buf, sizeof(buf), "AT+BTRH?\r");
    } else {
        snprintf(buf, sizeof(buf), "AT+BTRH=%u\r", val);
    }

    return rfcomm_write_at(r.fd, BTMG_AT_TYPE_RAW, buf, NULL);
}

int bt_hfp_hf_send_at_vts(char code)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "AT+VTS=%c\r", code);

    return rfcomm_write_at(r.fd, BTMG_AT_TYPE_RAW, buf, NULL);
}

int bt_hfp_hf_send_at_bcc(void)
{
    char *buf;
    buf = "+BCC\r";

    return rfcomm_write_at(r.fd, BTMG_AT_TYPE_CMD, buf, NULL);
}

int bt_hfp_hf_send_at_cnum(void)
{
    char *buf;
    buf = "+CNUM\r";

    return rfcomm_write_at(r.fd, BTMG_AT_TYPE_CMD, buf, NULL);
}


int bt_hfp_hf_send_at_vgs(uint32_t volume)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "AT+VGS=%u\r", volume);

    return rfcomm_write_at(r.fd, BTMG_AT_TYPE_RAW, buf, NULL);
}

int bt_hfp_hf_send_at_vgm(uint32_t volume)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "AT+VGM=%u\r", volume);

    return rfcomm_write_at(r.fd, BTMG_AT_TYPE_RAW, buf, NULL);
}

int bt_hfp_hf_send_at_cmd(char *cmd)
{
    static char command[512];
    bool at;
    int len = strlen(cmd);

    if (!len || len < 3)
        return BT_ERROR_INVALID_ARGS;

    command[0] = '\0';
    if (!(at = strncmp(cmd, "AT", 2) == 0))
        strcpy(command, "\r\n");

    strcat(command, cmd);
    strcat(command, "\r");
    if (!at)
        strcat(command, "\n");

    return rfcomm_write_at(r.fd, BTMG_AT_TYPE_RAW, command, NULL);
}
/***********************************************************/
static uint16_t sco_handle;

int bt_hfp_write_voice_setting(uint16_t voice_setting)
{
    write_voice_setting_cp cp;
    struct hci_request rq;

    int hdev = 0;
    int s;

    if ((s = hci_open_dev(hdev)) < 0) {
        BTMG_ERROR("Can't open hci device");
        return BT_ERROR;
    }

    memset(&rq, 0, sizeof(rq));
    cp.voice_setting = voice_setting;
    rq.ogf = OGF_HOST_CTL;
    rq.ocf = OCF_WRITE_VOICE_SETTING;
    rq.cparam = &cp;
    rq.clen = WRITE_VOICE_SETTING_CP_SIZE;

    if (hci_send_req(s, &rq, 2000) < 0) {
        BTMG_ERROR("write voice set fail");
        goto end;
    }

    hci_close_dev(s);
    return BT_OK;

end:
    hci_close_dev(s);
    return BT_ERROR;
}

int sco_conn_hci(uint16_t handle)
{
    setup_sync_conn_cp cp = { .handle = handle,
                              .tx_bandwith = 0x00001f40,
                              .rx_bandwith = 0x00001f40,
                              .max_latency = 0x000a,
                              .voice_setting = 0x0060,
                              .retrans_effort = 0x01,
                              .pkt_type = 0x03bf };
    struct hci_request rq;
    evt_sync_conn_complete rp;
    int hdev = 0;
    int s;

    if ((s = hci_open_dev(hdev)) < 0) {
        BTMG_ERROR("Can't open hci device");
        return BT_ERROR;
    }

    memset(&rq, 0, sizeof(rq));
    rq.ogf = OGF_LINK_CTL;
    rq.ocf = OCF_SETUP_SYNC_CONN;
    rq.event = EVT_SYNC_CONN_COMPLETE;
    rq.cparam = &cp;
    rq.clen = SETUP_SYNC_CONN_CP_SIZE;
    rq.rparam = &rp;
    rq.rlen = EVT_SYNC_CONN_COMPLETE_SIZE;

    if (hci_send_req(s, &rq, 20000) < 0) {
        BTMG_ERROR("bt_sco_create_conn hci_send_req fail");
        goto end;
    }

    if (rp.status) {
        BTMG_ERROR("connct sco fail");
        goto end;
    }

    BTMG_DEBUG("sco conn success, handle:0x%04X, link_type:0x%02X, air_mode:0x%02X", rp.handle, rp.link_type, rp.air_mode);

    sco_handle = rp.handle;

    hci_close_dev(s);
    return BT_OK;

end:
    hci_close_dev(s);
    return BT_ERROR;
}

int sco_conn(int s, int dev_id, long arg)
{
    struct hci_conn_list_req *cl = NULL;
    struct hci_conn_info *ci = NULL;
    int i;

    if (!(cl = malloc(10 * sizeof(*ci) + sizeof(*cl)))) {
        BTMG_ERROR("Can't allocate memory");
        return BT_ERROR;
    }
    cl->dev_id = dev_id;
    cl->conn_num = 10;
    ci = cl->conn_info;

    if (ioctl(s, HCIGETCONNLIST, (void *)cl)) {
        BTMG_ERROR("Can't get connection list");
        free(cl);
        return BT_ERROR;
    }

    for (i = 0; i < cl->conn_num; i++, ci++) {
        char addr[18];
        ba2str(&ci->bdaddr, addr);
        if (!bacmp((bdaddr_t *)arg, &ci->bdaddr) && ci->type == ACL_LINK) {
            char *str;
            str = hci_lmtostr(ci->link_mode);
            BTMG_DEBUG("\t%s %x %s handle %d state %d lm %s\n", ci->out ? "<" : ">", ci->type, addr,
                    ci->handle, ci->state, str);
            bt_free(str);
            if (sco_conn_hci(ci->handle) == BT_ERROR) {
                BTMG_ERROR("sco create fail");
                free(cl);
                return BT_ERROR;
            }
            free(cl);
            return BT_OK;
        }
    }

    BTMG_ERROR("no find device");
    free(cl);
    return BT_ERROR;
}

int bt_hfp_ag_sco_conn_cmd(char *addr)
{
    if (rfcomm_write_at(r.fd, BTMG_AT_TYPE_RESP, "+CIEV", "3,2"))
        return BT_ERROR;

    if (rfcomm_write_at(r.fd, BTMG_AT_TYPE_RESP, "+CIEV", "3,3"))
        return BT_ERROR;

    if (rfcomm_write_at(r.fd, BTMG_AT_TYPE_RESP, "+CIEV", "2,1"))
        return BT_ERROR;

    if (rfcomm_write_at(r.fd, BTMG_AT_TYPE_RESP, "+CIEV", "3,0"))
        return BT_ERROR;

    bdaddr_t bdaddr;
    str2ba(addr, &bdaddr);
    /* uart-ag not needed; the stack creates it when the audio device is opened */
    if (hfp_if_type == PCM_TYPE) {
        hci_for_each_dev(HCI_UP, sco_conn, (long)&bdaddr);
    }
    bt_hfp_hf_pcm_start();

    return BT_OK;
}


int sco_disconn_hci(void)
{
    evt_disconn_complete rp;
    disconnect_cp cp;
    struct hci_request rq;


    if (sco_handle == 0) {
        BTMG_ERROR("no sco handle");
        return BT_OK;
    }
    //continue
    int hdev = 0;
    int s;

    if ((s = hci_open_dev(hdev)) < 0) {
        BTMG_ERROR("Can't open hci device");
        return BT_ERROR;
    }

    memset(&cp, 0, sizeof(cp));
    cp.handle = sco_handle;
    cp.reason = 0x13;

    memset(&rq, 0, sizeof(rq));
    rq.ogf = OGF_LINK_CTL;
    rq.ocf = OCF_DISCONNECT;
    rq.event = EVT_DISCONN_COMPLETE;
    rq.cparam = &cp;
    rq.clen = DISCONNECT_CP_SIZE;
    rq.rparam = &rp;
    rq.rlen = EVT_DISCONN_COMPLETE_SIZE;

    if (hci_send_req(s, &rq, 20000) < 0) {
        BTMG_ERROR("sco_disconn hci send req fail");
        goto end;
    }

    sco_handle = 0x0;


    if (rp.status) {
        BTMG_ERROR("Can't disconnct sco");
        goto end;
    }

    BTMG_DEBUG("sco disconnct_hci success");

    hci_close_dev(s);
    return BT_OK;
end:
    hci_close_dev(s);
    return BT_ERROR;
}

int bt_hfp_ag_sco_disconn_cmd(void)
{
    if (hfp_if_type == PCM_TYPE) {
        sco_disconn_hci();
    }
    bt_hfp_hf_pcm_stop();
    if (rfcomm_write_at(r.fd, BTMG_AT_TYPE_RESP, "+CIEV", "2,0")) {
        BTMG_ERROR("sco_disconn hci send req fail");
        return BT_ERROR;
    }
    BTMG_DEBUG("ag sco disconnct success");
    return BT_OK;
}
/***********************************************************/

/**
 * RESP: Standard indicator update AT command */
static int rfcomm_handler_cind_resp_test_cb(const struct btmg_at_t *at)
{
    char *tmp = at->value;
    size_t i;
    if (strstr(at->value, "service") != NULL) {
        /* parse response for the +CIND TEST command */
        if (at_parse_cind(at->value, r.hfp_ind_map) == -1)
            BTMG_WARNG("Couldn't parse AG indicators: %s", at->value);
    } else {
        /* parse response for the +CIND GET command */
        for (i = 0; i < ARRAYSIZE(r.hfp_ind_map); i++) {
            r.hfp_ind[r.hfp_ind_map[i]] = atoi(tmp);
            if ((tmp = strchr(tmp, ',')) == NULL)
                break;
            tmp += 1;
        }
    }
    return 0;
}

/**
 * RESP: Standard indicator events reporting unsolicited result code */
static int rfcomm_handler_ciev_resp_cb(const struct btmg_at_t *at) {

    unsigned int index;
    unsigned int value;

    if (sscanf(at->value, "%u,%u", &index, &value) == 2 &&
            --index < ARRAYSIZE(r.hfp_ind_map)) {
        r.hfp_ind[r.hfp_ind_map[index]] = value;
        switch (r.hfp_ind_map[index]) {
        case HFP_IND_CALLSETUP:
            if (value == 0) {
                BTMG_DEBUG("Not currently in call set up");
                if (r.hfp_ind[HFP_IND_CALL] == 0) {
                    bt_hfp_hf_pcm_stop();
                }
            } else if (value == 1) {
                BTMG_INFO("An incoming call process is ongoing");
                if (hfp_if_type == PCM_TYPE) { //TODO may have issues opening early on uart
                    bt_hfp_hf_pcm_start();
                }
            } else if (value == 2) {
                BTMG_INFO("An outgoing call set up is ongoing");
                if (hfp_if_type == PCM_TYPE) { //TODO may have issues opening early on uart
                    bt_hfp_hf_pcm_start();
                }
            } else if (value == 3) {
                BTMG_DEBUG("The remote party being alerted in an outgoing call");
            }
            break;
        case HFP_IND_CALLHELD:
            BTMG_INFO("HFP_IND_CALLHELD");
            break;
        case HFP_IND_CALL:
            if (value == 0) {
                BTMG_INFO("No call is active");
                bt_hfp_hf_pcm_stop();
            } else if (value == 1) {
                BTMG_INFO("A call is active");
                bt_hfp_hf_pcm_start();
            }
            break;
        default:
            break;
        }
    }

    return 0;
}

static const struct ba_rfcomm_handler handlers[] = {
    { BTMG_AT_TYPE_RESP, "+CIND", rfcomm_handler_cind_resp_test_cb },
    { BTMG_AT_TYPE_RESP, "+CIEV", rfcomm_handler_ciev_resp_cb }
};

static int rfcomm_at_handle(const struct btmg_at_t *at)
{
    size_t i;

    for (i = 0; i < ARRAYSIZE(handlers); i++) {
        if (handlers[i].type != at->type)
            continue;
        if (strcmp(handlers[i].command, at->command) != 0)
            continue;
        return handlers[i].callback(at);
    }

    return BT_OK;
}

static int rfcomm_read_at(int fd, struct at_reader *reader) {

    char *buffer = reader->buffer;
    char *msg = reader->next;
    char *tmp;

    /* In case of reading more than one message from blueasla io, we have to
    * parse all of them before we can read from the socket once more. */
    if (msg == NULL) {

        ssize_t len;

retry:
        /* Reserve one byte for the NUL terminator written below */
        if ((len = read(fd, buffer, sizeof(reader->buffer) - 1)) == -1) {
            if (errno == EINTR)
                goto retry;
            return -1;
        }

        if (len == 0) {
            errno = ECONNRESET;
            return -1;
        }

        buffer[len] = '\0';
        msg = buffer;
    }

    /* parse AT message received from the RFCOMM */
    if ((tmp = at_parse(msg, &reader->at)) == NULL) {
        reader->next = msg;
        errno = EBADMSG;
        return -1;
    }

    reader->next = tmp[0] != '\0' ? tmp : NULL;
    return 0;
}

/**
 * Write AT message.
 *
 * @param fd RFCOMM socket file descriptor.
 * @param type Type of the AT message.
 * @param command AT command or response code.
 * @param value AT value or NULL if not applicable.
 * @return On success this function returns 0. Otherwise, -1 is returned and
 *   errno is set to indicate the error. */
static int rfcomm_write_at(int fd, enum btmg_at_type type, const char *command, const char *value)
{
    char msg[256];
    size_t len;

    BTMG_INFO("Sending AT message: %s: command:%s, value:%s", at_type2str(type), command, value);

    at_build(msg, sizeof(msg), type, command, value);
    len = strlen(msg);

retry:
    if (write(fd, msg, len) == -1) {
        if (errno == EINTR)
            goto retry;
        return BT_ERROR;
    }

    return BT_OK;
}

static void rfcomm_thread_exit(void *arg)
{
    if (r.fd != -1) {
        close(r.fd);
        r.fd = -1;
    }
    BTMG_INFO("Exiting rfcomm thread");
}

void *rfcomm_thread_main(struct pcm_worker *w)
{
    int hci_dev_id = 0;
    bdaddr_t addr;
    char rfcomm_path[128] = {0};
    DBusError err = DBUS_ERROR_INIT;
	struct at_reader reader = { .next = NULL };
    r.fd = -1;

    if (prctl(PR_SET_NAME, (unsigned long)"rfcomm_thread") == -1) {
        BTMG_ERROR("unable to set rfcomm thread name: %s", strerror(errno));
    }

    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

    pthread_cleanup_push(PTHREAD_CLEANUP(rfcomm_thread_exit), NULL);

    bacpy(&addr, &w->ba_pcm.addr);
    sprintf(rfcomm_path, "/org/bluealsa/hci%d/dev_%.2X_%.2X_%.2X_%.2X_%.2X_%.2X/rfcomm",
            hci_dev_id, addr.b[5], addr.b[4], addr.b[3], addr.b[2], addr.b[1], addr.b[0]);
    if (!bluealsa_dbus_open_rfcomm(&dbus_ctx, rfcomm_path, &r.fd, &err)) {
        BTMG_ERROR("Couldn't open RFCOMM: %s", err.message);
        goto fail;
    }
    if (r.fd < 0) {
        goto fail;
    }
    struct pollfd pfds[] = {
        { r.fd, POLLIN, 0 },
    };
    int timeout = -1;

    BTMG_INFO("start main loop");
    while (main_loop_on) {
        pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

        switch (poll(pfds, ARRAYSIZE(pfds), timeout)) {
        case -1:
            if (errno == EINTR)
                continue;
            BTMG_ERROR("FIFO poll error: %s", strerror(errno));
            goto fail;
        }

        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

        if (pfds[0].revents & POLLIN) {
read:
            if (rfcomm_read_at(pfds[0].fd, &reader) == -1)
                switch (errno) {
                case EBADMSG:
                    BTMG_ERROR("Invalid AT message: %s", reader.next);
                    reader.next = NULL;
                    continue;
                default:
                    goto fail;
                }
            rfcomm_at_handle(&reader.at);
            if (btmg_cb_p && btmg_cb_p->btmg_hfp_cb.hfp_at_event_cb)
                btmg_cb_p->btmg_hfp_cb.hfp_at_event_cb(reader.at);
        } else if (pfds[0].revents & (POLLERR | POLLHUP)) {
            goto fail;
        }
        /* we've got unprocessed data */
        if (reader.next != NULL)
            goto read;
    }

fail:
    BTMG_DEBUG("RFCOMM IO ERROR, EXIT");
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    pthread_cleanup_pop(1);
    return NULL;
}
