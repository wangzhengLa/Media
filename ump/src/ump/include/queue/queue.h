#ifndef INTELLI_QUE_MANGER_H
#define INTELLI_QUE_MANGER_H

#include <stdint.h>
#include <assert.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define IPC_INTELLI_SOK (0)
#define IPC_INTELLI_SFAIL (-1)
#define IPC_INTELLI_ASSERT(x)                                                        \
    {                                                                                \
        if ((x) == 0)                                                                \
        {                                                                            \
            fprintf(stderr, " ASSERT (%s|%s|%d)\r\n", __FILE__, __func__, __LINE__); \
            while (getchar() != 'q')                                                 \
                ;                                                                    \
        }                                                                            \
    }

    typedef enum
    {
        IPC_INTELLI_EMPTY_BUFFER_FLAG = 0,
        IPC_INTELLI_FULL_BUFFER_FLAG
    } EIntelliBufType;

    typedef enum
    {
        IPC_INTELLI_TIMEOUT_NONE = 0,  // 非阻塞，立即返回
        IPC_INTELLI_TIMEOUT_2MSEC = 2, // 非阻塞，2ms超时
        TIMEOUT_FOREVER = 0xFFFFFFF    // 阻塞
    } EIntelliBufTimeOutType;

    typedef struct
    {
        uint32_t u32CurRd;
        uint32_t u32CurWr;
        uint32_t u32MaxLen;
        uint32_t u32Count;

        uintptr_t *pu64Queue; // 存放内存地址，默认u64型

        void *pvMutex;
        void *pvCondRd;
        void *pvCondWr;
    } TIntelliQueHndl;

    typedef struct
    {
        TIntelliQueHndl emptyQue;
        TIntelliQueHndl fullQue;
    } TIntelliBufHndl;

    /**************************************************
    *函数名: IntelliBufCreate
    *功能  : 创建队列
    *输入  : TIntelliBufHndl *ptQueHdl : 队列句柄指针
            uint32_t u32MaxBufNum : 最大队列长度
    *输出  : 无
    *返回值: 成功 0，失败 -1
    *注释  :
    ***************************************************/
    int IntelliBufCreate(TIntelliBufHndl *ptQueHdl, uint32_t u32MaxBufNum);

    /**************************************************
     *函数名: IntelliBufDelete
     *功能  : 销毁队列
     *输入  : TIntelliBufHndl *ptQueHdl : 队列句柄指针
     *输出  : 无
     *返回值: 成功 0，失败 -1
     *注释  :
     ***************************************************/
    int IntelliBufDelete(TIntelliBufHndl *ptQueHdl);

    /**************************************************
    *函数名: IntelliPutBuffer
    *功能  : 入队列中
    *输入  : TIntelliBufHndl *ptQueHdl : 队列句柄指针
            void *bufInfo : 入队列缓存指针
            EIntelliBufType bufType : 入队列类型，目前支持empty队列和full队列
    *输出  : 无
    *返回值: 成功 0，失败 -1
    *注释  :
    ***************************************************/
    int IntelliPutBuffer(TIntelliBufHndl *hndl, void *bufInfo, EIntelliBufType bufType);

    /**************************************************
    *函数名: IntelliPutBuffer
    *功能  : 出队列中
    *输入  : TIntelliBufHndl *ptQueHdl : 队列句柄指针
            EIntelliBufType bufType : 出队列类型，目前支持empty队列和full队列
            EIntelliBufTimeOutType bufTimeOutType : 出队列方式，当前支持非阻塞和阻塞两种方式
    *输出  : 无
    *返回值: 出队列缓存指针
    *注释  :
    ***************************************************/
    void *IntelliGetBuffer(TIntelliBufHndl *hndl, EIntelliBufType bufType, EIntelliBufTimeOutType bufTimeOutType);

    /**************************************************
     *函数名: IntelliBufGetEmptyCount
     *功能  : 获取empty队列可用资源数
     *输入  : TIntelliBufHndl *ptQueHdl : 队列句柄指针
     *输出  : 无
     *返回值: empty队列可用资源数
     *注释  :
     ***************************************************/
    unsigned int IntelliBufGetEmptyCount(TIntelliBufHndl *hndl);

    /**************************************************
     *函数名: IntelliBufGetFullCount
     *功能  : 获取full队列可用资源数
     *输入  : TIntelliBufHndl *ptQueHdl : 队列句柄指针
     *输出  : 无
     *返回值: full队列可用资源数
     *注释  :
     ***************************************************/
    unsigned int IntelliBufGetFullCount(TIntelliBufHndl *hndl);

#ifdef __cplusplus
}
#endif

#endif