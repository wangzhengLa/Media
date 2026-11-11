/*
* Copyright (c) 2018-2022 Allwinner Technology Co. Ltd. All rights reserved.
* Author: BT Team
* Date: 2022.03.12
* Description: spp service function.
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
#include "bt_log.h"
#include "common.h"
#include "bt_rfcomm.h"
#include "bt_device.h"

#define BUFFER_SIZE 1024

extern bt_rfcomm_t *rfcomm;

static struct sockaddr_rc laddr = { 0 }, raddr = { 0 };
int client_socket = 0;
char addrbuf[1024] = { 0 };

static int rfcomm_raw_tty = 1;
static pthread_t recv_thread;
static bool recv_thread_start = false;
static pthread_mutex_t rfcomm_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t clean_mutex = PTHREAD_MUTEX_INITIALIZER;
static sdp_session_t *session = NULL;

static void clean(void *arg);

static void *recv_thread_process(void *arg)
{
    pthread_cleanup_push((void *)clean, "spp_service_recv_thread_process");
    recv_thread_start = true;
    char recv_msg[BUFFER_SIZE];
    int byte_num = 0;

    if (prctl(PR_SET_NAME, "spp_service_recv_thread") == -1) {
        BTMG_ERROR("unable to set revc thread name: %s", strerror(errno));
    }

    BTMG_INFO("Waiting for connection on channel %d", laddr.rc_channel);
    socklen_t alen = sizeof(raddr);
    client_socket = accept(rfcomm->sock, (struct sockaddr *)&raddr, &alen);
    spp_service_regesiter_dev();
    while (recv_thread_start) {
        bzero(recv_msg, BUFFER_SIZE);
        if (rfcomm->rfcomm_fd > 0) {
            byte_num = read(rfcomm->rfcomm_fd, recv_msg, sizeof(recv_msg));
            if (byte_num <= 0) {
                BTMG_DEBUG("spp service disconnect...");
                break;
            }
            if (btmg_cb_p && btmg_cb_p->btmg_spp_server_cb.spp_server_accept_cb)
                btmg_cb_p->btmg_spp_server_cb.spp_server_accept_cb(rfcomm->dst_addr, recv_msg,
                                                                   byte_num);
        }
        usleep(1000 * 10);
    }

    pthread_cleanup_pop(1);
    pthread_exit(NULL);
}

int spp_service_send(char *data, uint32_t len)
{
    int status = 0;
    pthread_mutex_lock(&rfcomm_mutex);
    status = send(client_socket, data, len, 0);
    BTMG_DEBUG("send len:%d", status);
    pthread_mutex_unlock(&rfcomm_mutex);
    if (status < 0) {
        BTMG_ERROR("rfcomm send fail");
        return BT_ERROR;
    }

    return status;
}

sdp_record_t *record = NULL;

sdp_session_t *spp_service_register(uint8_t rfcomm_channel)
{
    // Adapted from http://www.btessentials.com/examples/bluez/sdp-register.c
    uint32_t svc_uuid_int[] = { 0x01110000, 0x00100000, 0x80000080, 0xFB349B5F };
    const char *service_name = "Roto-Rooter Data Router";
    const char *svc_dsc = "An experimental plumbing router";
    const char *service_prov = "Roto-Rooter";

    uuid_t root_uuid, l2cap_uuid, rfcomm_uuid, svc_uuid, svc_class_uuid;
    sdp_list_t *l2cap_list = 0, *rfcomm_list = 0, *root_list = 0, *proto_list = 0,
               *access_proto_list = 0, *svc_class_list = 0, *profile_list = 0;
    sdp_data_t *channel = 0;
    sdp_profile_desc_t profile;
    sdp_session_t *sdpsession = NULL;

    record = sdp_record_alloc();
    if (!record)
        return NULL;

    memset(record, 0, sizeof(sdp_record_t));
    // set the general service ID
    sdp_uuid128_create(&svc_uuid, &svc_uuid_int);
    sdp_set_service_id(record, svc_uuid);

    char str[256] = "";
    sdp_uuid2strn(&svc_uuid, str, 256);

    // set the service class
    sdp_uuid16_create(&svc_class_uuid, SERIAL_PORT_SVCLASS_ID);
    svc_class_list = sdp_list_append(0, &svc_class_uuid);
    sdp_set_service_classes(record, svc_class_list);

    // set the Bluetooth profile information
    sdp_uuid16_create(&profile.uuid, SERIAL_PORT_PROFILE_ID);
    profile.version = 0x0100;
    profile_list = sdp_list_append(0, &profile);
    sdp_set_profile_descs(record, profile_list);

    // make the service record publicly browsable
    sdp_uuid16_create(&root_uuid, PUBLIC_BROWSE_GROUP);
    root_list = sdp_list_append(0, &root_uuid);
    sdp_set_browse_groups(record, root_list);

    // set l2cap information
    sdp_uuid16_create(&l2cap_uuid, L2CAP_UUID);
    l2cap_list = sdp_list_append(0, &l2cap_uuid);
    proto_list = sdp_list_append(0, l2cap_list);

    // register the RFCOMM channel for RFCOMM sockets
    sdp_uuid16_create(&rfcomm_uuid, RFCOMM_UUID);
    channel = sdp_data_alloc(SDP_UINT8, &rfcomm_channel);
    rfcomm_list = sdp_list_append(0, &rfcomm_uuid);
    sdp_list_append(rfcomm_list, channel);
    sdp_list_append(proto_list, rfcomm_list);

    access_proto_list = sdp_list_append(0, proto_list);
    sdp_set_access_protos(record, access_proto_list);

    //set the name, provider, and description
    sdp_set_info_attr(record, service_name, service_prov, svc_dsc);

    // connect to the local SDP server, register the service record,
    // and disconnect
    sdpsession = sdp_connect(BDADDR_ANY, BDADDR_LOCAL, SDP_RETRY_IF_BUSY);
    if (!sdpsession) {
        BTMG_ERROR("can't connect to sdp server on device  %s", strerror(errno));
        goto end;
    }
    sdp_record_register(sdpsession, record, 0);

end:
    // cleanup
    sdp_data_free(channel);
    sdp_list_free(l2cap_list, 0);
    sdp_list_free(rfcomm_list, 0);
    sdp_list_free(root_list, 0);
    sdp_list_free(access_proto_list, 0);
    sdp_list_free(svc_class_list, 0);
    sdp_list_free(profile_list, 0);
    sdp_list_free(proto_list, 0);
    return sdpsession;
}

void spp_service_regesiter_dev(void)
{
    BTMG_DEBUG("enter...");
    int dev = 0, try = 30;
    struct termios ti;
    struct rfcomm_dev_req req;
    char dst[18];
    memset(&req, 0, sizeof(req));
    req.dev_id = dev;
    req.flags = (1 << RFCOMM_REUSE_DLC) | (1 << RFCOMM_RELEASE_ONHUP);
    bacpy(&req.src, &laddr.rc_bdaddr);
    bacpy(&req.dst, &raddr.rc_bdaddr);
    req.channel = raddr.rc_channel;

    dev = ioctl(client_socket, RFCOMMCREATEDEV, &req);
    if (dev < 0) {
        perror("Can't create RFCOMM TTY");
        close(rfcomm->sock);
        return;
    }

    snprintf(rfcomm->dev_name, sizeof(rfcomm->dev_name), "/dev/rfcomm%d", dev);
    while ((rfcomm->rfcomm_fd = open(rfcomm->dev_name, O_RDONLY | O_NOCTTY)) < 0) {
        if (errno == EACCES) {
            perror("Can't open RFCOMM device");
            return;
        }

        snprintf(rfcomm->dev_name, sizeof(rfcomm->dev_name), "/dev/bluetooth/rfcomm/%d", dev);
        if ((rfcomm->rfcomm_fd = open(rfcomm->dev_name, O_RDONLY | O_NOCTTY)) < 0) {
            if (try--) {
                snprintf(rfcomm->dev_name, sizeof(rfcomm->dev_name), "/dev/rfcomm%d", dev);
                usleep(100 * 1000);
                continue;
            }
            perror("Can't open RFCOMM device");
            return;
        }
    }

    if (rfcomm_raw_tty) {
        tcflush(rfcomm->rfcomm_fd, TCIOFLUSH);
        cfmakeraw(&ti);
        tcsetattr(rfcomm->rfcomm_fd, TCSANOW, &ti);
    }

    char remote_name[256] = { 0 };
    dev_node_t *dev_node = NULL;
    ba2str(&req.dst, dst);
    bt_device_get_name(dst, remote_name);
    dev_node = btmg_dev_list_find_device(connected_devices, dst);
    if (dev_node == NULL) {
        btmg_dev_list_add_device(connected_devices, remote_name, dst, BTMG_REMOTE_DEVICE_SPP);
        BTMG_DEBUG("add spp device %s into connected_devices", dst);
    }

    if (btmg_cb_p && btmg_cb_p->btmg_spp_server_cb.spp_server_connection_state_cb) {
        ba2str(&raddr.rc_bdaddr, addrbuf);
        BTMG_INFO("spp connected device:%s", addrbuf);
        btmg_cb_p->btmg_spp_server_cb.spp_server_connection_state_cb(dst,
                                                                     BTMG_SPP_CLIENT_CONNECTED);
    }

    memcpy(rfcomm->dst_addr, dst, sizeof(rfcomm->dst_addr));
    rfcomm->dev = dev;
}

int spp_service_accept(int channelID)
{
    int status;
    int reuse = 1;

    if (rfcomm == NULL) {
        BTMG_ERROR("rfcomm not init,need enable spp profile");
        return BT_ERROR;
    }

    if (recv_thread_start) {
        BTMG_WARNG("spp server already start!");
        return 0;
    }

    session = spp_service_register(channelID);
    laddr.rc_family = AF_BLUETOOTH;
    laddr.rc_bdaddr = *BDADDR_ANY;
    laddr.rc_channel = channelID;

    rfcomm->sock = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (rfcomm->sock < 0) {
        BTMG_ERROR("Can't create RFCOMM socket");
        return BT_ERROR;
    }

    if (setsockopt(rfcomm->sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(int)) == -1) {
        BTMG_ERROR("RFCOMM socket setsockopt failed!");
    }

    status = bind(rfcomm->sock, (struct sockaddr *)&laddr, sizeof(laddr));
    if (status < 0) {
        BTMG_ERROR("RFCOMM socket binding failed!");
        goto release;
    }

    status = listen(rfcomm->sock, 1);
    if (status < 0) {
        BTMG_ERROR("RFCOMM socket listen failed!");
        goto release;
    }

    if (spp_service_start_recv_thread() == -1) {
        BTMG_ERROR("spp_service_start_accept_thread failed!");
        goto end;
    }

    return BT_OK;

end:
    if (btmg_cb_p && btmg_cb_p->btmg_spp_server_cb.spp_server_connection_state_cb) {
        btmg_cb_p->btmg_spp_server_cb.spp_server_connection_state_cb(
                "NULL dst", BTMG_SPP_SERVER_CONNECT_FAILED);
    }
release:
    close(rfcomm->sock);

    return BT_ERROR;
}

int spp_service_disconnect(void)
{
    pthread_mutex_lock(&clean_mutex);
    BTMG_DEBUG("enter...");

    if (btmg_cb_p && btmg_cb_p->btmg_spp_server_cb.spp_server_connection_state_cb) {
        btmg_cb_p->btmg_spp_server_cb.spp_server_connection_state_cb(addrbuf,
                                                                     BTMG_SPP_SERVER_DISCONNECTING);
    }

    if (recv_thread_start == true) {
        recv_thread_start = false;
        if (client_socket > 0)
            rfcomm_release_dev(client_socket, 0);
        if (client_socket) {
            close(client_socket);
            client_socket = 0;
        }
        if (rfcomm->sock > 0) {
            close(rfcomm->sock);
            rfcomm->sock = 0;
        }
        if (session) {
            sdp_record_unregister(session, record);
            sdp_close(session);
            session = NULL;
        }
    }

    spp_service_bt_disconnect(addrbuf);
    pthread_mutex_unlock(&clean_mutex);
    return BT_OK;
}

int spp_service_bt_disconnect(const char *addr)
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
    if (btmg_cb_p && btmg_cb_p->btmg_spp_server_cb.spp_server_connection_state_cb) {
        btmg_cb_p->btmg_spp_server_cb.spp_server_connection_state_cb(addr,
                                                                     BTMG_SPP_SERVER_DISCONNEC_FAILED);
    }
    return BT_ERROR;
}

bool is_spp_service_run(void)
{
    return recv_thread_start;
}

static void clean(void *arg)
{
    pthread_mutex_lock(&clean_mutex);
    BTMG_DEBUG("ENTER...");
    recv_thread_start = false;
    if (client_socket > 0)
        rfcomm_release_dev(client_socket, 0);
    if (client_socket) {
        close(client_socket);
        client_socket = 0;
    }
    if (rfcomm->sock > 0) {
        close(rfcomm->sock);
        rfcomm->sock = 0;
    }
    if (session) {
        sdp_record_unregister(session, record);
        sdp_close(session);
        session = NULL;
    }
    if (btmg_cb_p && btmg_cb_p->btmg_spp_server_cb.spp_server_connection_state_cb) {
        btmg_cb_p->btmg_spp_server_cb.spp_server_connection_state_cb(addrbuf, BTMG_SPP_SERVER_DISCONNECTED);
    }

    pthread_mutex_unlock(&clean_mutex);
    BTMG_INFO("SPP server stop thread ok");
}

int spp_service_start_recv_thread(void)
{
    int ret = pthread_create(&recv_thread, NULL, recv_thread_process, NULL);

    if (ret < 0) {
        BTMG_ERROR("recv thread error");
        return BT_ERROR;
    }

    return BT_OK;
}

void spp_service_stop_recv_thread(void)
{
    if (recv_thread_start) {
        clean((void *)NULL);
    }
}
