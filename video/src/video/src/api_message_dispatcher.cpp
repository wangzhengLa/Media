#include "api_message_dispatcher.h"
#include "osp.h"
#include <map>
#include <vector>
#include <pthread.h>
#include "log.h"

namespace ecovacs
{
    using namespace std;

    /************************************Locals**********************************/
    static ros_message_queue_t *g_ros_message_queue_handle = NULL; /* ROS消息队列句柄 */

    /****************************ros消息队列管理***************************/
    int ros_msg_queue_init()
    {
        // 申请消息队列内存
        g_ros_message_queue_handle = new ros_message_queue_t();
        if (NULL == g_ros_message_queue_handle)
        {
            ump_error("---------------------------g_ros_message_queue_handle malloc failed---------------------------\r\n");
            goto ERR_OUT;
        }

        // 消息队列创建
        if (IPC_INTELLI_SOK != IntelliBufCreate(&g_ros_message_queue_handle->msg_buf_hndl, MAX_MESSAGE_QUEUE_SIZE))
        {
            ump_error("---------------------------g_ros_message_queue_handle->msg_buf_hndl create failed---------------------------\r\n");
            goto ERR_OUT_FREE_MSG_QUEUE_HANDLE;
        }

        // 将msg_buf放进empty队列中
        for (int idy = 0; idy < MAX_MESSAGE_QUEUE_SIZE; idy++)
        {
            if (IPC_INTELLI_SOK != IntelliPutBuffer(&g_ros_message_queue_handle->msg_buf_hndl, &g_ros_message_queue_handle->msg_buf[idy], IPC_INTELLI_EMPTY_BUFFER_FLAG))
            {
                ump_error("---------------------------IntelliPutBuffer msg_buf[%d] failed---------------------------\r\n", idy);
                goto ERR_OUT_INTELLI_BUF_DELETE;
            }
        }

        return IPC_INTELLI_SOK;

    ERR_OUT_INTELLI_BUF_DELETE:
        IntelliBufDelete(&g_ros_message_queue_handle->msg_buf_hndl);
    ERR_OUT_FREE_MSG_QUEUE_HANDLE:
        delete g_ros_message_queue_handle;
        g_ros_message_queue_handle = NULL;
    ERR_OUT:
        return IPC_INTELLI_SFAIL;
    }

    void ros_msg_queue_uninit()
    {
        request_message_t *ptMessage = NULL;

        if (IntelliBufGetFullCount(&g_ros_message_queue_handle->msg_buf_hndl) > 0)
        {
            while ((ptMessage = (request_message_t *)IntelliGetBuffer(&g_ros_message_queue_handle->msg_buf_hndl, IPC_INTELLI_FULL_BUFFER_FLAG, IPC_INTELLI_TIMEOUT_NONE)) != NULL)
            {
                IntelliPutBuffer(&g_ros_message_queue_handle->msg_buf_hndl, ptMessage, IPC_INTELLI_EMPTY_BUFFER_FLAG);
                ptMessage = NULL;
            }
        }

        IntelliBufDelete(&g_ros_message_queue_handle->msg_buf_hndl);

        delete g_ros_message_queue_handle;
        g_ros_message_queue_handle = NULL;
    }

    int ros_put_msg(const request_message_t *message)
    {
        if (NULL == message)
        {
            ump_error("----------------message = %p----------------\r\n", message);
            return -1;
        }

        request_message_t *ptMessage = (request_message_t *)IntelliGetBuffer(&g_ros_message_queue_handle->msg_buf_hndl, IPC_INTELLI_EMPTY_BUFFER_FLAG, IPC_INTELLI_TIMEOUT_NONE);
        if (NULL != ptMessage)
        {
            *ptMessage = *message;
            IntelliPutBuffer(&g_ros_message_queue_handle->msg_buf_hndl, ptMessage, IPC_INTELLI_FULL_BUFFER_FLAG);
        }
        else
        {
            ump_error("----------------IntelliGetBuffer Empty Failed----------------\r\n");
            return -1;
        }

        return 0;
    }

    int ros_get_msg(request_message_t *message)
    {
        if (NULL == message)
        {
            ump_error("----------------message = %p----------------\r\n", message);
            return -1;
        }

        request_message_t *ptMsgData = NULL;
        ptMsgData = (request_message_t *)IntelliGetBuffer(&g_ros_message_queue_handle->msg_buf_hndl, IPC_INTELLI_FULL_BUFFER_FLAG, IPC_INTELLI_TIMEOUT_NONE);
        if (NULL == ptMsgData)
        {
            // ump_error("----------------IntelliGetBuffer Full Failed----------------\r\n");
            return -1;
        }

        message->msg = ptMsgData->msg;

        IntelliPutBuffer(&g_ros_message_queue_handle->msg_buf_hndl, ptMsgData, IPC_INTELLI_EMPTY_BUFFER_FLAG);
        ptMsgData = NULL;

        return 0;
    }

} // namespace name
