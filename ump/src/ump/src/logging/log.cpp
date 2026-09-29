#include "log.h"

pthread_mutex_t log_module::mutex = PTHREAD_MUTEX_INITIALIZER;
log_module *log_module::log_instance = NULL;

log_module ::log_module()
{
}

log_module::~log_module()
{
}

int log_module::init_zlog()
{
    if (NULL == log_instance)
    {
        pthread_mutex_lock(&mutex);
        if (NULL == log_instance)
        {
            log_instance = new log_module();
            if (NULL != log_instance)
            {
                if (access(ZLOG_CONDATA_PATH, F_OK) == 0)
                {
                    if (-1 == zlog_init(ZLOG_CONDATA_PATH))
                    {
                        printf("zlog_init failed %s\r\n", ZLOG_CONDATA_PATH);
                        return LOG_INIT_SFAIL;
                    }
                }
                else
                {
                    if (-1 == zlog_init(ZLOG_CONFIG_PATH))
                    {
                        printf("zlog_init failed %s\r\n", ZLOG_CONFIG_PATH);
                        return LOG_INIT_SFAIL;
                    }
                }

                log_instance->zlog_category = zlog_get_category("ump");
                if (NULL == log_instance->zlog_category)
                {
                    zlog_fini();
                    delete log_instance;
                    log_instance = NULL;
                }
            }
        }
        pthread_mutex_unlock(&mutex);
    }

    if (NULL == log_instance)
        return LOG_INIT_SFAIL;
    else
        return LOG_INIT_SOK;
}

zlog_category_t *log_module::get_zlog_handle()
{
    if (NULL == log_instance)
        return NULL;
    return log_instance->zlog_category;
}

void log_module::uninit_zlog()
{
    pthread_mutex_lock(&mutex);
    if (log_instance)
    {
        zlog_fini();
        delete log_instance;
        log_instance = NULL;
    }
    pthread_mutex_unlock(&mutex);
}
