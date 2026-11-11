/*
* Copyright (c) 2018-2022 Allwinner Technology Co. Ltd. All rights reserved.
* Author: BT Team
* Date: 2022.03.12
* Description: gatt server connect function.
*/

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <semaphore.h>
#include <pthread.h>
#include <sys/ioctl.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include "lib/l2cap.h"
#include "lib/uuid.h"
#include "src/shared/mainloop.h"
#include "src/shared/util.h"
#include "src/shared/att.h"
#include "src/shared/queue.h"
#include "src/shared/gatt-db.h"
#include "src/shared/gatt-server.h"
#include "bt_manager.h"
#include "common.h"
#include "bt_log.h"
#include "bt_mainloop.h"
#include "bt_gatt_server.h"
#include "bt_gatt_inner.h"
#include "bt_le_hci.h"

struct service_data {
    struct gatt_db_attribute *match;
    bool found;
    uint16_t handle;
};

static pthread_t gatt_tid;
gatt_server_t *gatt_server = NULL;
static int fd = -1;
static bool verbose = true;
static pthread_mutex_t clean_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool gatts_connected_flag;

/* Implemented in bt_gatt_db.c; declared once here instead of inside
 * bt_conn_gatts_init() so its return value can be checked. */
int bt_gatt_db_add_default_services(gatt_client_t *server);

bool get_gatts_connect_status(void)
{
    return gatts_connected_flag;
}

void set_gatts_connect_status(bool status)
{
    gatts_connected_flag = status;
}

static void att_debug_cb(const char *str, void *user_data)
{
    const char *prefix = user_data;

    BTMG_DEBUG("%s""%s",prefix, str);
}

static void gatt_debug_cb(const char *str, void *user_data)
{
    const char *prefix = user_data;

    BTMG_DEBUG("%s %s", prefix, str);
}

static void server_destroy(gatt_server_t *server)
{
    BTMG_DEBUG("Freeing gatt");
    bt_gatt_server_unref(server->gatts);
    BTMG_DEBUG("Freeing db");
    gatt_db_unref(server->dbs);
}

static void att_disconnect_cb(int err, void *user_data)
{
    BTMG_DEBUG("enter");
    BTMG_DEBUG("Device disconnected: %s", strerror(err));

    /* TODO: Report Disconnection Event */

    /* TODO: Whether we should  re-enable advertising */
    gatts_connection_event_t event = BT_GATT_DISCONNECT;

    if (btmg_cb_p && btmg_cb_p->btmg_gatt_server_cb.gatts_connection_event_cb) {
        if (gatt_server && gatt_server->bd_addr) {
            btmg_cb_p->btmg_gatt_server_cb.gatts_connection_event_cb(gatt_server->bd_addr, event, 0);
        }
    }
    set_gatts_connect_status(false);

    pthread_mutex_lock(&clean_mutex);
    if (gatt_server && gatt_server->gatts) {
        bt_gatt_server_unref(gatt_server->gatts);
        gatt_server->gatts = NULL;
    }
    if (gatt_server && gatt_server->att && gatt_server->id) {
        bt_att_unregister_disconnect(gatt_server->att, gatt_server->id);
        gatt_server->id = 0;
        bt_att_unref(gatt_server->att);
        gatt_server->att = NULL;
    }
    /* WR-021: bt_att_set_close_on_unref(true) handed the accepted fd to
     * ATT; bt_att_unref() above already closed it. Only clear the stored
     * integer here, never close it a second time. */
    if (gatt_server)
        gatt_server->fd = -1;
    pthread_mutex_unlock(&clean_mutex);
    BTMG_DEBUG("quit");
}

static int l2cap_le_att_accept(int sk)
{
    int nsk;
    socklen_t optlen;
    struct sockaddr_l2 addr;
    char ba[18];

    BTMG_DEBUG("enter");

    if (sk < 0)
        goto fail;

    memset(&addr, 0, sizeof(addr));
    optlen = sizeof(addr);
    nsk = accept(sk, (struct sockaddr *)&addr, &optlen);
    if (nsk < 0) {
        BTMG_ERROR("Accept failed");
        goto fail;
    }

    ba2str(&addr.l2_bdaddr, ba);
    memcpy(gatt_server->bd_addr, ba, AG_MAX_BDADDR_LEN);
    BTMG_INFO("Connect from %s", ba);

    return nsk;
fail:
    return -1;
}

static void server_listen_cb(int fd, uint32_t events, void *user_data)
{
    gatt_server_t *server = user_data;
    int accept_fd;
    int mtu = 517;
    gatts_connection_event_t event;
    int connect_handle;
    /* True once bt_att_set_close_on_unref() has handed the accepted fd
     * to ATT; from then on only bt_att_unref() may close it (WR-021). */
    bool att_owns_fd = false;

    BTMG_DEBUG("enter");
    if (events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
        mainloop_remove_fd(fd);
        return;
    }

    accept_fd = l2cap_le_att_accept(fd);
    if (accept_fd < 0) {
        BTMG_ERROR("Accept error");
        return;
    }

    BTMG_DEBUG("accept_fd %d", accept_fd);
    server->fd = accept_fd;
    server->att = bt_att_new(accept_fd, false);
    if (!server->att) {
        BTMG_ERROR("Failed to initialze ATT transport layer, fd:%d", accept_fd);
        goto fail;
    }

    if (!bt_att_set_close_on_unref(server->att, true)) {
        BTMG_ERROR("Failed to set up ATT transport layer");
        goto fail;
    }
    att_owns_fd = true;
    /* Clear the application-side copy so no deinit/cleanup path closes
     * a fd that ATT now owns. */
    server->fd = -1;

    server->id = bt_att_register_disconnect(server->att, att_disconnect_cb, server, NULL);
    if (!server->id) {
        BTMG_ERROR("Failed to set ATT disconnect handler");
        goto fail;
    }

    server->gatts = bt_gatt_server_new(server->dbs, server->att, mtu, 0);
    if (!server->gatts) {
        BTMG_ERROR("Failed to create GATT server");
        goto fail;
    }

    if (verbose) {
        bt_att_set_debug(server->att, 0, att_debug_cb, "att: ", NULL);
        bt_gatt_server_set_debug(server->gatts, gatt_debug_cb, "server: ", NULL);
    }

    BTMG_INFO("gatt connected");
    event = BT_GATT_CONNECTION;
    connect_handle = find_ledev_conn_handle(0, gatt_server->bd_addr);

    if (btmg_cb_p && btmg_cb_p->btmg_gatt_server_cb.gatts_connection_event_cb) {
        btmg_cb_p->btmg_gatt_server_cb.gatts_connection_event_cb(gatt_server->bd_addr, event, connect_handle);
    }
    set_gatts_connect_status(true);

    return;
fail:
    BTMG_ERROR("set gatt conn failed");

    /* Unwind in reverse order. If ATT owns the fd, unref'ing it closes
     * the socket exactly once; otherwise close the accepted fd here. */
    if (server->gatts) {
        bt_gatt_server_unref(server->gatts);
        server->gatts = NULL;
    }
    if (server->att) {
        if (server->id) {
            bt_att_unregister_disconnect(server->att, server->id);
            server->id = 0;
        }
        bt_att_unref(server->att);
        server->att = NULL;
    } else if (!att_owns_fd) {
        close(accept_fd);
    }
    server->fd = -1;

    event = BT_GATT_CONNECT_FAIL;
    if (btmg_cb_p && btmg_cb_p->btmg_gatt_server_cb.gatts_connection_event_cb) {
        btmg_cb_p->btmg_gatt_server_cb.gatts_connection_event_cb(gatt_server->bd_addr, event, 0);
    }
    return;
}

static int l2cap_le_att_listen(bdaddr_t *src, int sec, uint8_t src_type)
{
    int sk;
    struct sockaddr_l2 srcaddr;
    struct bt_security btsec;

    BTMG_DEBUG("enter");
    sk = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (sk < 0) {
        BTMG_ERROR("Failed to create L2CAP socket");
        return -1;
    }

    /* Set up source address */
    memset(&srcaddr, 0, sizeof(srcaddr));
    srcaddr.l2_family = AF_BLUETOOTH;
    srcaddr.l2_cid = htobs(ATT_CID);
    srcaddr.l2_bdaddr_type = src_type;
    bacpy(&srcaddr.l2_bdaddr, src);

    if (bind(sk, (struct sockaddr *)&srcaddr, sizeof(srcaddr)) < 0) {
        BTMG_ERROR("Failed to bind L2CAP socket");
        goto fail;
    }

    /* Set the security level */
    memset(&btsec, 0, sizeof(btsec));
    btsec.level = sec;
    if (setsockopt(sk, SOL_BLUETOOTH, BT_SECURITY, &btsec, sizeof(btsec)) != 0) {
        BTMG_ERROR("Failed to set L2CAP security level");
        goto fail;
    }

    if (listen(sk, 10) < 0) {
        BTMG_ERROR("Listening on socket failed");
        goto fail;
    }

    BTMG_DEBUG("Started listening on ATT channel");

    return sk;

fail:
    close(sk);
    return -1;
}

int bt_conn_gatts_init()
{
    bdaddr_t src_addr;
    int sec = BT_SECURITY_LOW;
    uint8_t src_type = BDADDR_LE_PUBLIC;
    int ret = -1;

    BTMG_DEBUG("enter");

    gatt_server = new0(gatt_server_t, 1);
    if (!gatt_server)
        return -1;
    gatt_server->fd = -1;

    gatt_server->dbs = gatt_db_new();
    if (!gatt_server->dbs) {
        BTMG_ERROR("failed to create GATT database");
        goto free_server;
    }

    if (bt_bluez_mainloop_init() != 0) {
        BTMG_ERROR("failed to init bluez mainloop");
        goto free_db;
    }

    if (bt_gatt_db_add_default_services(gatt_server) != 0) {
        BTMG_ERROR("failed to add default GATT services");
        goto deinit_mainloop;
    }

    bacpy(&src_addr, BDADDR_ANY);
    fd = l2cap_le_att_listen(&src_addr, sec, src_type);
    if (fd < 0) {
        BTMG_ERROR("failed to listen on ATT channel");
        goto deinit_mainloop;
    }

    ret = mainloop_add_fd(fd, EPOLLIN, server_listen_cb, gatt_server, NULL);
    if (ret < 0) {
        BTMG_ERROR("Failed to add listen socket(%d),error(%d)", fd, ret);
        close(fd);
        fd = -1;
        ret = -1;
        goto deinit_mainloop;
    }

    return 0;

deinit_mainloop:
    bt_bluez_mainloop_deinit();
free_db:
    gatt_db_unref(gatt_server->dbs);
    gatt_server->dbs = NULL;
free_server:
    free(gatt_server);
    gatt_server = NULL;
    return ret;
}

int bt_conn_gatts_deinit()
{
    BTMG_DEBUG("enter");

    /* WR-020: stop the event producer and join the mainloop thread
     * BEFORE destroying the objects its callbacks use. BlueZ releases all
     * fd event data when mainloop_run() returns, so no epoll callback can
     * still touch the server/db/ATT below. The join is done outside the
     * mainloop mutex (see bt_mainloop.c) and clean_mutex is not held here,
     * so no lock cycle is introduced. */
    bt_bluez_mainloop_deinit();

    if (fd >= 0) {
        /* No-op after mainloop_run() drained the fd list; still needed if
         * another mainloop user keeps the loop alive (dual-role case). */
        mainloop_remove_fd(fd);
        close(fd);
        fd = -1;
    }

    pthread_mutex_lock(&clean_mutex);
    if (gatt_server) {
        BTMG_DEBUG("enter");
        if (gatt_server->gatts) {
            bt_gatt_server_unref(gatt_server->gatts);
            gatt_server->gatts = NULL;
        }
        if (gatt_server->att && gatt_server->id) {
            bt_att_unregister_disconnect(gatt_server->att, gatt_server->id);
            gatt_server->id = 0;
            bt_att_unref(gatt_server->att);
            gatt_server->att = NULL;
        }
        /* WR-021: ATT owns the accepted fd (close_on_unref); only clear
         * the stored integer, never close it a second time. */
        gatt_server->fd = -1;
        gatt_db_unref(gatt_server->dbs);
        free(gatt_server);
        gatt_server = NULL;
    }
    pthread_mutex_unlock(&clean_mutex);

    return 0;
}
