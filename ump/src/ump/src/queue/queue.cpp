/*
 *  队列管理操作函数
 *
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif //_GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include "queue.h"
#include "osp.h"

// 队列管理
static int IntelliQueCreate(TIntelliQueHndl *ptQueueHdl, uint32_t u32MaxLen)
{
    ptQueueHdl->u32CurRd = ptQueueHdl->u32CurWr = 0;
    ptQueueHdl->u32Count = 0;
    ptQueueHdl->u32MaxLen = u32MaxLen;
    ptQueueHdl->pu64Queue = (uintptr_t *)IntelliUtilMalloc(sizeof(uintptr_t) * ptQueueHdl->u32MaxLen);

    ptQueueHdl->pvMutex = IntelliUtilCreateCritiSec();
    ptQueueHdl->pvCondRd = IUtilCondCreate();
    ptQueueHdl->pvCondWr = IUtilCondCreate();

    if (NULL == ptQueueHdl->pvMutex || NULL == ptQueueHdl->pvCondRd || NULL == ptQueueHdl->pvCondWr)
        return IPC_INTELLI_SFAIL;

    return IPC_INTELLI_SOK;
}
static int IntelliQueDelete(TIntelliQueHndl *hndl)
{
    if (hndl->pu64Queue != NULL)
    {
        IntelliUtilFree(hndl->pu64Queue);
    }

    IUtilCondClose(hndl->pvCondRd);
    IUtilCondClose(hndl->pvCondWr);
    IntelliUtilCloseCritiSec(hndl->pvMutex);

    return 0;
}
static int IntelliQuePut(TIntelliQueHndl *ptQueHdl, uintptr_t u64BufAddr, uint32_t timeout)
{
    int status = IPC_INTELLI_SFAIL;

    IntelliUtilLockCritiSec(ptQueHdl->pvMutex);

    while (1)
    {
        if (ptQueHdl->u32Count < ptQueHdl->u32MaxLen)
        {
            ptQueHdl->pu64Queue[ptQueHdl->u32CurWr] = u64BufAddr;
            ptQueHdl->u32CurWr = (ptQueHdl->u32CurWr + 1) % ptQueHdl->u32MaxLen;
            ptQueHdl->u32Count++;
            status = IPC_INTELLI_SOK;
            IUtilCondSignal(ptQueHdl->pvCondRd);
            break;
        }
        else
        {
            if (timeout == IPC_INTELLI_TIMEOUT_NONE)
            {
                break;
            }
            status = IUtilCondWait(ptQueHdl->pvCondWr, ptQueHdl->pvMutex);
        }
    }

    IntelliUtilUnlockCritiSec(ptQueHdl->pvMutex);

    return status;
}
static int IntelliQueGet(TIntelliQueHndl *ptQueHdl, uintptr_t *pu64Value, uint32_t timeout)
{
    int status = IPC_INTELLI_SFAIL;

    IntelliUtilLockCritiSec(ptQueHdl->pvMutex);

    while (1)
    {
        if (ptQueHdl->u32Count > 0)
        {
            if (pu64Value != NULL)
            {
                *pu64Value = ptQueHdl->pu64Queue[ptQueHdl->u32CurRd];
            }

            ptQueHdl->u32CurRd = (ptQueHdl->u32CurRd + 1) % ptQueHdl->u32MaxLen;
            ptQueHdl->u32Count--;
            status = IPC_INTELLI_SOK;
            IUtilCondSignal(ptQueHdl->pvCondWr);
            break;
        }
        else
        {
            if (timeout == IPC_INTELLI_TIMEOUT_NONE)
            {
                break;
            }
            status = IUtilCondWait(ptQueHdl->pvCondRd, ptQueHdl->pvMutex);
        }
    }

    IntelliUtilUnlockCritiSec(ptQueHdl->pvMutex);

    return status;
}
static int IntelliQueGetQueuedCount(TIntelliQueHndl *ptQueHdl)
{
    unsigned int u32QueuedCount = 0;

    IntelliUtilLockCritiSec(ptQueHdl->pvMutex);
    u32QueuedCount = ptQueHdl->u32Count;
    IntelliUtilUnlockCritiSec(ptQueHdl->pvMutex);
    return u32QueuedCount;
}
static int IntelliBufGetEmpty(TIntelliBufHndl *ptQueHdl, void **ppvBufInfo, uint32_t u32TimeOut)
{
    int status;
    uintptr_t u64BufAddr;

    if (ptQueHdl == NULL || ppvBufInfo == NULL)
    {
        return IPC_INTELLI_SFAIL;
    }

    status = IntelliQueGet(&ptQueHdl->emptyQue, &u64BufAddr, u32TimeOut);
    if (status != IPC_INTELLI_SOK)
    {
        *ppvBufInfo = NULL;
        return status;
    }

    *ppvBufInfo = (void *)u64BufAddr;

    return status;
}
static int IntelliBufPutEmpty(TIntelliBufHndl *ptQueHdl, void *pvBufInfo)
{
    int status;

    if (ptQueHdl == NULL || NULL == pvBufInfo)
    {
        return IPC_INTELLI_SFAIL;
    }

    status = IntelliQuePut(&ptQueHdl->emptyQue, (uintptr_t)pvBufInfo, IPC_INTELLI_TIMEOUT_NONE);

    return status;
}
static int IntelliBufGetFull(TIntelliBufHndl *ptQueHdl, void **ppvBufInfo, uint32_t u32TimeOut)
{
    int status;
    uintptr_t u64BufAddr;

    if (ptQueHdl == NULL || ppvBufInfo == NULL)
    {
        return IPC_INTELLI_SFAIL;
    }

    status = IntelliQueGet(&ptQueHdl->fullQue, &u64BufAddr, u32TimeOut);
    if (status != IPC_INTELLI_SOK)
    {
        *ppvBufInfo = NULL;
        return status;
    }

    *ppvBufInfo = (void *)u64BufAddr;

    return status;
}
static int IntelliBufPutFull(TIntelliBufHndl *ptQueHdl, void *pvBufInfo)
{
    int status;

    if (ptQueHdl == NULL || NULL == pvBufInfo)
    {
        return IPC_INTELLI_SFAIL;
    }

    status = IntelliQuePut(&ptQueHdl->fullQue, (uintptr_t)pvBufInfo, IPC_INTELLI_TIMEOUT_NONE);

    return status;
}

unsigned int IntelliBufGetEmptyCount(TIntelliBufHndl *ptQueHdl)
{
    return IntelliQueGetQueuedCount(&ptQueHdl->emptyQue);
}
unsigned int IntelliBufGetFullCount(TIntelliBufHndl *ptQueHdl)
{
    return IntelliQueGetQueuedCount(&ptQueHdl->fullQue);
}
int IntelliBufCreate(TIntelliBufHndl *ptQueHdl, uint32_t u32MaxBufNum)
{
    int status;

    status = IntelliQueCreate(&ptQueHdl->emptyQue, u32MaxBufNum);
    if (status != IPC_INTELLI_SOK)
    {
        return IPC_INTELLI_SFAIL;
    }

    status = IntelliQueCreate(&ptQueHdl->fullQue, u32MaxBufNum);
    if (status != IPC_INTELLI_SOK)
    {
        IntelliQueDelete(&ptQueHdl->emptyQue);
        return IPC_INTELLI_SFAIL;
    }

    return status;
}
int IntelliBufDelete(TIntelliBufHndl *ptQueHdl)
{
    int status = IPC_INTELLI_SOK;

    if (ptQueHdl == NULL)
    {
        return IPC_INTELLI_SFAIL;
    }

    status = IntelliQueDelete(&ptQueHdl->emptyQue);
    status |= IntelliQueDelete(&ptQueHdl->fullQue);

    return status;
}
int IntelliPutBuffer(TIntelliBufHndl *ptQueHdl, void *pvBufInfo, EIntelliBufType eBufType)
{
    if (ptQueHdl == NULL)
    {
        return IPC_INTELLI_SFAIL;
    }

    if (IPC_INTELLI_EMPTY_BUFFER_FLAG == eBufType)
    {
        IntelliBufPutEmpty(ptQueHdl, pvBufInfo);
    }
    else if (IPC_INTELLI_FULL_BUFFER_FLAG == eBufType)
    {
        IntelliBufPutFull(ptQueHdl, pvBufInfo);
    }

    return IPC_INTELLI_SOK;
}
void *IntelliGetBuffer(TIntelliBufHndl *ptQueHdl, EIntelliBufType eBufType, EIntelliBufTimeOutType eBufTimeOutType)
{
    int nRet;
    void *pvBuffer = NULL;
    unsigned int u32TimeOut = 0;
    int l32TryFlag = 1;

    if (TIMEOUT_FOREVER == eBufTimeOutType)
    {
        u32TimeOut = TIMEOUT_FOREVER;
    }
    else
    {
        u32TimeOut = IPC_INTELLI_TIMEOUT_NONE;
    }

    do
    {
        if (IPC_INTELLI_EMPTY_BUFFER_FLAG == eBufType)
        {
            nRet = IntelliBufGetEmpty(ptQueHdl, &pvBuffer, u32TimeOut);
        }
        else
        {
            nRet = IntelliBufGetFull(ptQueHdl, &pvBuffer, u32TimeOut);
        }

        if (nRet != IPC_INTELLI_SOK && eBufTimeOutType == IPC_INTELLI_TIMEOUT_2MSEC && l32TryFlag == 1)
        {
            IntelliUtilSleep((uint32_t)eBufTimeOutType);
            l32TryFlag = 0;
            continue;
        }
        else
        {
            break;
        }
    } while (1);

    return pvBuffer;
}
