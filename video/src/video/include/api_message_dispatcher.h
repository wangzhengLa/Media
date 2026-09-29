#ifndef API_MESSAGE_DISPATCHER_H_
#define API_MESSAGE_DISPATCHER_H_

#include <pthread.h>
#include <string>
#include "queue.h"

#define MAX_MESSAGE_QUEUE_SIZE (30)

namespace ecovacs
{
    struct request_message_t
    {
        std::string msg;
    };

    struct ros_message_queue_t
    {
        TIntelliBufHndl msg_buf_hndl;
        request_message_t msg_buf[MAX_MESSAGE_QUEUE_SIZE];
    };

    int ros_msg_queue_init();

    void ros_msg_queue_uninit();

    int ros_put_msg(const request_message_t *message);

    int ros_get_msg(request_message_t *message);

} // namespace ecovacs

#endif // API_MESSAGE_DISPATCHER_H_