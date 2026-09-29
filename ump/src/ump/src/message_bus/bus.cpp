#include <cstdio>
#include <cstring>
#include "bus.h"
#include "queue.h"
#include "osp.h"

#define MAX_MSG_QUEUE_SIZE (30)

typedef struct
{
    uint32_t u32Length;
    char *ps8Data;
} TMsgBuffer;

typedef struct
{
    TIntelliBufHndl message_buf_hndl;
    TMsgBuffer message_buf[MAX_MSG_QUEUE_SIZE];
} TBus;

static TBus g_bus;

int bus_init()
{
    // 初始化message数据队列
    if (IPC_INTELLI_SOK != IntelliBufCreate(&g_bus.message_buf_hndl, MAX_MSG_QUEUE_SIZE))
    {
        printf("---------------------------message_buf_hndl create failed---------------------------\r\n");
        return -1;
    }
    memset(g_bus.message_buf, 0, sizeof(TMsgBuffer) * MAX_MSG_QUEUE_SIZE);
    for (int idy = 0; idy < MAX_MSG_QUEUE_SIZE; idy++)
    {
        while (IPC_INTELLI_SOK != IntelliPutBuffer(&g_bus.message_buf_hndl, &g_bus.message_buf[idy], IPC_INTELLI_EMPTY_BUFFER_FLAG))
        {
            printf("---------------------------IntelliPutBufferbuf[%d] failed---------------------------\r\n", idy);
            IntelliBufDelete(&g_bus.message_buf_hndl);
            return -1;
        }
    }

    return 0;
}

int bus_deinit()
{
    // 回收消息队列
    TMsgBuffer *ptMessageData = NULL;
    ptMessageData = (TMsgBuffer *)IntelliGetBuffer(&g_bus.message_buf_hndl, IPC_INTELLI_FULL_BUFFER_FLAG, IPC_INTELLI_TIMEOUT_NONE);
    while (NULL != ptMessageData)
    {
        if (NULL != ptMessageData->ps8Data)
        {
            IntelliUtilFree(ptMessageData->ps8Data);
            ptMessageData->ps8Data = NULL;
        }
        memset(ptMessageData, 0, sizeof(TMsgBuffer));
        IntelliPutBuffer(&g_bus.message_buf_hndl, ptMessageData, IPC_INTELLI_EMPTY_BUFFER_FLAG);

        ptMessageData = NULL;
        ptMessageData = (TMsgBuffer *)IntelliGetBuffer(&g_bus.message_buf_hndl, IPC_INTELLI_FULL_BUFFER_FLAG, IPC_INTELLI_TIMEOUT_NONE);
    }
    IntelliBufDelete(&g_bus.message_buf_hndl);

    return 0;
}

int bus_put_message(char *buffer, int length)
{
    TMsgBuffer *ptMessageData = (TMsgBuffer *)IntelliGetBuffer(&g_bus.message_buf_hndl, IPC_INTELLI_EMPTY_BUFFER_FLAG, IPC_INTELLI_TIMEOUT_NONE);
    if (NULL != ptMessageData)
    {
        if (NULL != ptMessageData->ps8Data)
        {
            IntelliUtilFree(ptMessageData->ps8Data);
            ptMessageData->ps8Data = NULL;
        }

        ptMessageData->u32Length = length;
        ptMessageData->ps8Data = (char *)IntelliUtilMalloc((ptMessageData->u32Length + 1) * sizeof(char));
        if (NULL != ptMessageData->ps8Data)
        {
            memcpy(ptMessageData->ps8Data, buffer, length);
            IntelliPutBuffer(&g_bus.message_buf_hndl, ptMessageData, IPC_INTELLI_FULL_BUFFER_FLAG);
        }
        else
        {
            printf("----------------Malloc msg buffer failed----------------\r\n");
            IntelliPutBuffer(&g_bus.message_buf_hndl, ptMessageData, IPC_INTELLI_EMPTY_BUFFER_FLAG);
            return -1;
        }
    }
    else
    {
        printf("----------------IntelliGetBuffer Empty Failed----------------\r\n");
        return -1;
    }

    return 0;
}

int bus_get_message(char **buffer, int *length)
{
    TMsgBuffer *ptMessageData = NULL;
    ptMessageData = (TMsgBuffer *)IntelliGetBuffer(&g_bus.message_buf_hndl, IPC_INTELLI_FULL_BUFFER_FLAG, IPC_INTELLI_TIMEOUT_NONE);
    if (NULL == ptMessageData)
    {
        return -1;
    }

    *length = ptMessageData->u32Length;
    *buffer = NULL;
    while (NULL == *buffer)
        *buffer = (char *)IntelliUtilMalloc((ptMessageData->u32Length + 1) * sizeof(char));

    memset(*buffer, 0, ptMessageData->u32Length + 1);
    memcpy(*buffer, ptMessageData->ps8Data, ptMessageData->u32Length);

    IntelliUtilFree(ptMessageData->ps8Data);
    ptMessageData->ps8Data = NULL;

    memset(ptMessageData, 0, sizeof(TMsgBuffer));
    IntelliPutBuffer(&g_bus.message_buf_hndl, ptMessageData, IPC_INTELLI_EMPTY_BUFFER_FLAG);
    ptMessageData = NULL;

    return 0;
}

unsigned int bus_get_empty_count()
{
    return IntelliBufGetEmptyCount(&g_bus.message_buf_hndl);
}

unsigned int bus_get_full_count()
{
    return IntelliBufGetFullCount(&g_bus.message_buf_hndl);
}