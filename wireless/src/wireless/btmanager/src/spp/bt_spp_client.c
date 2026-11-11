/*
* Copyright (c) 2018-2022 Allwinner Technology Co. Ltd. All rights reserved.
* Author: BT Team
* Date: 2022.03.12
* Description: spp client function.
*/

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <signal.h>
#include <termios.h>
#include <poll.h>
#include <sys/param.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <sys/socket.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/rfcomm.h>
#include "channel.h"
#include "bt_rfcomm.h"
#include "bt_device.h"
#include "common.h"
#include "bt_log.h"

#define BUFFER_SIZE 1024

extern bt_rfcomm_t *rfcomm;
static struct sockaddr_rc laddr;

static int rfcomm_raw_tty = 1;
static pthread_t recv_thread;
static bool recv_thread_start = false;
static pthread_mutex_t rfcomm_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t clean_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint8_t SPP_UUID[16] = { 0x00, 0x00, 0x11, 0x01, 0x00, 0x00, 0x10, 0x00,
                                    0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb };

static void clean(void *arg);

static void *recv_thread_process(void *arg)
{
    pthread_cleanup_push((void *)clean, "spp_client_recv_thread_process");
    recv_thread_start = true;
    char recv_msg[BUFFER_SIZE];
    int byte_num = 0;

    if (prctl(PR_SET_NAME, "spp_client_recv_thread") == -1) {
        BTMG_ERROR("unable to set recv thread name: %s", strerror(errno));
    }
    while (recv_thread_start) {
        bzero(recv_msg, BUFFER_SIZE);
        if (rfcomm->rfcomm_fd > 0) {
            byte_num = read(rfcomm->rfcomm_fd, recv_msg, BUFFER_SIZE);
            if (byte_num <= 0) {
                BTMG_DEBUG("SPP client disconnect...");
                break;
            }
            if (btmg_cb_p && btmg_cb_p->btmg_spp_client_cb.spp_client_recvdata_cb) {
                btmg_cb_p->btmg_spp_client_cb.spp_client_recvdata_cb(rfcomm->dst_addr, recv_msg,
                                                                     byte_num);
            }
        }
        usleep(1000 * 10);
    }

    pthread_cleanup_pop(1);
    pthread_exit(NULL);
}

int spp_client_send(char *data, uint32_t len)
{
    int status = 0;
    pthread_mutex_lock(&rfcomm_mutex);
    status = send(rfcomm->sock, data, len, 0);
    BTMG_DEBUG("send len:%d", status);
    pthread_mutex_unlock(&rfcomm_mutex);
    if (status < 0) {
        BTMG_ERROR("rfcomm send fail");
        return BT_ERROR;
    }

    return status;
}

int spp_client_regesiter_dev(int dev, const char *dst)
{
    struct rfcomm_dev_req req;
    dev_node_t *dev_node = NULL;
    char remote_name[256] = { 0 };
    struct termios ti;
    int try = 30, ctl;

    memset(&req, 0, sizeof(req));
    req.dev_id = dev;
    req.flags = (1 << RFCOMM_REUSE_DLC) | (1 << RFCOMM_RELEASE_ONHUP);

    memcpy(&req.dst, &laddr.rc_bdaddr, sizeof(bdaddr_t));
    req.channel = laddr.rc_channel;

    dev = ioctl(rfcomm->sock, RFCOMMCREATEDEV, &req);
    if (dev < 0) {
        BTMG_ERROR("Can't create RFCOMM TTY");
        return BT_ERROR;
    }

    snprintf(rfcomm->dev_name, sizeof(rfcomm->dev_name), "/dev/rfcomm%d", dev);
    while ((rfcomm->rfcomm_fd = open(rfcomm->dev_name, O_RDWR | O_NOCTTY)) < 0) {
        if (errno == EACCES) {
            BTMG_ERROR("Can't open RFCOMM device");
            goto release;
        }
        snprintf(rfcomm->dev_name, sizeof(rfcomm->dev_name), "/dev/bluetooth/rfcomm/%d", dev);
        if ((rfcomm->rfcomm_fd = open(rfcomm->dev_name, O_RDWR | O_NOCTTY)) < 0) {
            if (try--) {
                snprintf(rfcomm->dev_name, sizeof(rfcomm->dev_name), "/dev/rfcomm%d", dev);
                usleep(100 * 1000);
                continue;
            }
            BTMG_ERROR("Can't open RFCOMM device");
            goto release;
        }
    }

    if (rfcomm_raw_tty) {
        tcflush(rfcomm->rfcomm_fd, TCIOFLUSH);
        cfmakeraw(&ti);
        tcsetattr(rfcomm->rfcomm_fd, TCSANOW, &ti);
    }

    bt_device_get_name(dst, remote_name);
    dev_node = btmg_dev_list_find_device(connected_devices, dst);
    if (dev_node == NULL) {
        btmg_dev_list_add_device(connected_devices, remote_name, dst, BTMG_REMOTE_DEVICE_SPP);
        BTMG_DEBUG("add spp device %s into connected_devices", dst);
    }

    if (btmg_cb_p && btmg_cb_p->btmg_spp_client_cb.spp_client_connection_state_cb) {
        BTMG_DEBUG("spp connected,device:%s", dst);
        btmg_cb_p->btmg_spp_client_cb.spp_client_connection_state_cb(dst,
                                                                     BTMG_SPP_CLIENT_CONNECTED);
    }

    memcpy(rfcomm->dst_addr, dst, sizeof(rfcomm->dst_addr));
    rfcomm->dev = dev;
    return BT_OK;
release:
    memset(&req, 0, sizeof(req));
    req.dev_id = dev;
    req.flags = (1 << RFCOMM_HANGUP_NOW);
    if(ioctl(rfcomm->sock, RFCOMMRELEASEDEV, &req) < 0) {
        BTMG_ERROR("Can't RELEAS RFCOMM TTY");
    }
    return BT_ERROR;
}

int spp_client_connect(int dev, const char *dst)
{
    if (rfcomm == NULL) {
        BTMG_ERROR("rfcomm not init,need enable spp profile");
        return BT_ERROR;
    }
    int channel = getChannel(SPP_UUID, dst);
    if (channel <= 0) {
        BTMG_ERROR("Can't get channel number %d", channel);
        return -1;
    }
    BTMG_DEBUG("channel %d", channel);
    str2ba(dst, &laddr.rc_bdaddr);
    laddr.rc_family = AF_BLUETOOTH;
    laddr.rc_channel = channel;

    if (btmg_cb_p && btmg_cb_p->btmg_spp_client_cb.spp_client_connection_state_cb) {
        btmg_cb_p->btmg_spp_client_cb.spp_client_connection_state_cb(dst,
                                                                     BTMG_SPP_CLIENT_CONNECTING);
    }
    if (rfcomm->sock > 0) {
        rfcomm_release_dev(rfcomm->sock, dev);
    }

    rfcomm->sock = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (rfcomm->sock < 0) {
        BTMG_ERROR("Can't create RFCOMM socket");
        goto end;
    }

    if (connect(rfcomm->sock, (struct sockaddr *)&laddr, sizeof(laddr)) < 0) {
        BTMG_ERROR("Can't connect RFCOMM socket");
        goto release;
    }

    if (spp_client_regesiter_dev(dev, dst) == -1) {
        goto release;
    }

    if (spp_client_start_recv_thread() == -1) {
        goto release;
    }

    return BT_OK;

release:
    close(rfcomm->sock);

end:
    if (btmg_cb_p && btmg_cb_p->btmg_spp_client_cb.spp_client_connection_state_cb) {
        btmg_cb_p->btmg_spp_client_cb.spp_client_connection_state_cb(
                dst, BTMG_SPP_CLIENT_CONNECT_FAILED);
    }
    return BT_ERROR;
}

int spp_client_disconnect(int dev, const char *dst)
{
    BTMG_DEBUG("disconnect addr:%s", dst);
    pthread_mutex_lock(&clean_mutex);

    if (btmg_cb_p && btmg_cb_p->btmg_spp_client_cb.spp_client_connection_state_cb) {
        btmg_cb_p->btmg_spp_client_cb.spp_client_connection_state_cb(dst,
                                                                     BTMG_SPP_CLIENT_DISCONNECTING);
    }

    if (recv_thread_start == true) {
        recv_thread_start = false;
        if (rfcomm->sock > 0)
            rfcomm_release_dev(rfcomm->sock, dev);
    }
    spp_client_bt_disconnect(dst);
    pthread_mutex_unlock(&clean_mutex);

    return BT_OK;
}

int spp_client_bt_disconnect(const char *addr)
{
    struct hci_conn_info_req *cr = NULL;
    bdaddr_t bdaddr;
    int opt, dd, dev_id;

    str2ba(addr, &bdaddr);

    dev_id = hci_for_each_dev(HCI_UP, find_conn, (long)&bdaddr);
    if (dev_id < 0) {
        BTMG_ERROR("Device:%s not connected", addr);
        goto ret;
    }

    dd = hci_open_dev(dev_id);
    if (dd < 0) {
        BTMG_ERROR("HCI device open failed");
        return BT_ERROR;
    }

    cr = malloc(sizeof(*cr) + sizeof(struct hci_conn_info));
    if (!cr) {
        BTMG_ERROR("Can't allocate memory");
        goto end;
    }

    bacpy(&cr->bdaddr, &bdaddr);
    cr->type = ACL_LINK;
    if (ioctl(dd, HCIGETCONNINFO, (unsigned long)cr) < 0) {
        BTMG_ERROR("Get connection info failed");
        goto end;
    }

    if (hci_disconnect(dd, htobs(cr->conn_info->handle), HCI_OE_USER_ENDED_CONNECTION, 10000) < 0) {
        BTMG_ERROR("Disconnect failed");
        goto end;
    }

    free(cr);
    cr = NULL;
    hci_close_dev(dd);
ret:
    return BT_OK;

end:
    if (cr) {
        free(cr);
        cr = NULL;
    }
    hci_close_dev(dd);
    if (btmg_cb_p && btmg_cb_p->btmg_spp_client_cb.spp_client_connection_state_cb) {
        btmg_cb_p->btmg_spp_client_cb.spp_client_connection_state_cb(
                addr, BTMG_SPP_CLIENT_DISCONNEC_FAILED);
    }
    return BT_ERROR;
}

bool is_spp_client_run(void)
{
    return recv_thread_start;
}

static void clean(void *arg)
{
    pthread_mutex_lock(&clean_mutex);
    BTMG_DEBUG("enter...");
    recv_thread_start = false;
    char dst[1024] = { 0 };
    ba2str(&laddr.rc_bdaddr, dst);
    if (rfcomm->sock > 0)
        rfcomm_release_dev(rfcomm->sock, 0);
    if (btmg_cb_p && btmg_cb_p->btmg_spp_client_cb.spp_client_connection_state_cb) {
        btmg_cb_p->btmg_spp_client_cb.spp_client_connection_state_cb(dst, BTMG_SPP_CLIENT_DISCONNECTED);
    }
    BTMG_DEBUG("end...");
    pthread_mutex_unlock(&clean_mutex);
}

int spp_client_start_recv_thread(void)
{
    int ret = pthread_create(&recv_thread, NULL, recv_thread_process, NULL);

    if (ret < 0) {
        BTMG_ERROR("revc thread error");
        return BT_ERROR;
    }

    return BT_OK;
}

void spp_client_stop_recv_thread(void)
{
    if (recv_thread_start) {
        clean((void *)NULL);
    }
}
