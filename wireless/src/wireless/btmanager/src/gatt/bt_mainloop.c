/*
* Copyright (c) 2018-2022 Allwinner Technology Co., Ltd. All rights reserved.
* Author: BT Team
* Date: 2022.03.12
* Description: ble mainloop function.
*/

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <src/shared/mainloop.h>
#include "bt_log.h"

typedef struct {
    pthread_t thread_id;
    bool thread_created;   /* pthread_create succeeded; a join is owed */
    bool thread_running;   /* mainloop_run() is in progress */
    bool start_done;       /* thread has reported its startup result */
    int  start_result;     /* 0 = ready, -1 = startup failed */
    int  sockfds[2];
    int  ref;
    pthread_t tid;
} mainloop_handle_t;

static mainloop_handle_t mainloop_handle = {
    .thread_created = false,
    .thread_running = false,
    .start_done = false,
    .start_result = -1,
    .sockfds = { -1, -1 },
    .ref = 0,
};

static pthread_mutex_t mainloop_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mainloop_start_cond = PTHREAD_COND_INITIALIZER;

#define MAIN_LOOP_EXIT_CODE 0xf0

static void bt_mainloop_thread_quit_cb(int fd, uint32_t events, void *user_data)
{
    ssize_t result;
    uint8_t c;

    BTMG_DEBUG("enter");

    if (events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
        BTMG_ERROR("Events error");
        mainloop_quit();
        return;
    }

    result = read(fd, &c, 1);
    if (result < 0) {
        BTMG_ERROR("Read channel error");
        mainloop_quit();
        return;
    }
    if (result == 1 && c == MAIN_LOOP_EXIT_CODE) {
        BTMG_DEBUG("Received bta thread exit code");
        mainloop_quit();
    } else {
        BTMG_ERROR("Read msg ERR, result:%zd, c:%d", result, c);
        mainloop_quit();
    }
}

/* Publish the startup result so bt_bluez_mainloop_init() cannot report
 * success for a thread that failed to create its socket pair or register
 * the quit fd (WR-023). */
static void mainloop_report_start(int result)
{
    pthread_mutex_lock(&mainloop_mutex);
    mainloop_handle.thread_running = (result == 0);
    if (result == 0)
        mainloop_handle.tid = pthread_self();
    mainloop_handle.start_result = result;
    mainloop_handle.start_done = true;
    pthread_cond_broadcast(&mainloop_start_cond);
    pthread_mutex_unlock(&mainloop_mutex);
}

static void *bt_mainloop_thread(void *arg)
{
    int ret;
    int start_result = 0;

    (void)arg;
    BTMG_DEBUG("enter");

    /* mainloop_init() lives here so a startup failure is always paired
     * with mainloop_run()'s cleanup of epoll_fd and mainloop_list. */
    mainloop_init();

    ret = socketpair(PF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                     mainloop_handle.sockfds);
    if (ret == -1) {
        BTMG_ERROR("Couldn't create paired sockets");
        mainloop_handle.sockfds[0] = -1;
        mainloop_handle.sockfds[1] = -1;
        start_result = -1;
    } else {
        ret = mainloop_add_fd(mainloop_handle.sockfds[0],
                EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR,
                bt_mainloop_thread_quit_cb, NULL, NULL);
        if (ret < 0) {
            BTMG_ERROR("Failed to add fd (%d)\n", ret);
            start_result = -1;
        }
    }

    mainloop_report_start(start_result);

    if (start_result != 0) {
        /* No quit fd is registered, so make mainloop_run() return at
         * once; it still drains mainloop_list and closes epoll_fd. */
        mainloop_quit();
    }
    mainloop_run();

    pthread_mutex_lock(&mainloop_mutex);
    mainloop_handle.thread_running = false;
    pthread_mutex_unlock(&mainloop_mutex);

    if (mainloop_handle.sockfds[0] >= 0)
        close(mainloop_handle.sockfds[0]);
    if (mainloop_handle.sockfds[1] >= 0)
        close(mainloop_handle.sockfds[1]);
    mainloop_handle.sockfds[0] = -1;
    mainloop_handle.sockfds[1] = -1;
    BTMG_DEBUG("quit");

    return NULL;
}

pthread_t bt_bluez_thread_tid_get(void)
{
    return mainloop_handle.tid;
}

int bt_bluez_mainloop_deinit(void)
{
    uint8_t c = MAIN_LOOP_EXIT_CODE;
    pthread_t tid = 0;
    bool do_join = false;
    int result;

    BTMG_DEBUG("enter");

    pthread_mutex_lock(&mainloop_mutex);

    if (mainloop_handle.ref == 0) {
        pthread_mutex_unlock(&mainloop_mutex);
        return 0;
    }

    mainloop_handle.ref--;
    BTMG_DEBUG("reference:%d", mainloop_handle.ref);

    if (mainloop_handle.ref > 0) {
        pthread_mutex_unlock(&mainloop_mutex);
        return 0;
    }

    if (mainloop_handle.thread_created) {
        if (mainloop_handle.sockfds[1] >= 0) {
            result = write(mainloop_handle.sockfds[1], &c, 1);
            if (result < 0)
                BTMG_ERROR("Couldn't send exit code, %s", strerror(errno));
        }
        mainloop_handle.thread_running = false;
        mainloop_handle.thread_created = false;
        tid = mainloop_handle.thread_id;
        do_join = true;
    }

    /* Join outside the mutex: the mainloop thread takes mainloop_mutex
     * when it leaves mainloop_run(), so holding the lock here would
     * deadlock (WR-020/WR-023). */
    pthread_mutex_unlock(&mainloop_mutex);

    if (do_join)
        pthread_join(tid, NULL);

    BTMG_DEBUG("quit.");
    return 0;
}

int bt_bluez_mainloop_init(void)
{
    int ret;
    pthread_t tid;

    BTMG_DEBUG("enter");

    pthread_mutex_lock(&mainloop_mutex);

    /* An existing user only takes a reference; the startup handshake
     * below is for the creator only, so no second thread is started. */
    if (mainloop_handle.ref > 0 || mainloop_handle.thread_created) {
        mainloop_handle.ref++;
        pthread_mutex_unlock(&mainloop_mutex);
        BTMG_DEBUG("mainloop thread has been already running");
        return 0;
    }

    mainloop_handle.ref = 1;
    mainloop_handle.start_done = false;
    mainloop_handle.start_result = -1;
    mainloop_handle.thread_running = false;

    ret = pthread_create(&mainloop_handle.thread_id, NULL, bt_mainloop_thread, NULL);
    if (ret != 0) {
        BTMG_ERROR("mainloop thread create failed.");
        mainloop_handle.ref = 0;   /* roll back, do not leak a reference */
        pthread_mutex_unlock(&mainloop_mutex);
        return -1;
    }
    mainloop_handle.thread_created = true;

    /* Wait for the thread to finish mainloop_init/socketpair/add_fd
     * before reporting success. cond_wait releases mainloop_mutex so the
     * thread can publish its result. */
    while (!mainloop_handle.start_done)
        pthread_cond_wait(&mainloop_start_cond, &mainloop_mutex);

    if (mainloop_handle.start_result != 0) {
        tid = mainloop_handle.thread_id;
        mainloop_handle.thread_created = false;
        mainloop_handle.ref = 0;
        pthread_mutex_unlock(&mainloop_mutex);
        pthread_join(tid, NULL);
        return -1;
    }

    pthread_mutex_unlock(&mainloop_mutex);
    BTMG_DEBUG("reference:%d", mainloop_handle.ref);
    return 0;
}
