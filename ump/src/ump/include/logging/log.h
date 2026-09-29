#ifndef LOG_H
#define LOG_H

#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <pthread.h>
#include "zlog.h"

#define ZLOG_CONFIG_PATH "/etc/conf/ump_log.conf"
#define ZLOG_CONDATA_PATH "/userdata/conf/ump_log.conf"
#define LOG_INIT_SOK (0)
#define LOG_INIT_SFAIL (-1)

class log_module
{
private:
    log_module();
    ~log_module();
    log_module(const log_module &) = delete;             // 禁止拷贝构造
    log_module &operator=(const log_module &) = delete;  // 禁止拷贝赋值
    log_module(const log_module &&) = delete;            // 禁止移动构造
    log_module &operator=(const log_module &&) = delete; // 禁止移动赋值

public:
    static int init_zlog();
    static zlog_category_t *get_zlog_handle();
    static void uninit_zlog();

private:
    zlog_category_t *zlog_category;

private:
    static log_module *log_instance;
    static pthread_mutex_t mutex;
};

#define ump_error(format, ...) zlog_error(log_module::get_zlog_handle(), format, ##__VA_ARGS__)
#define ump_warn(format, ...) zlog_warn(log_module::get_zlog_handle(), format, ##__VA_ARGS__)
#define ump_info(format, ...) zlog_info(log_module::get_zlog_handle(), format, ##__VA_ARGS__)
#define ump_debug(format, ...) zlog_debug(log_module::get_zlog_handle(), format, ##__VA_ARGS__)

#endif