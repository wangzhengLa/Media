
#ifndef __SHMFIFO_H__
#define __SHMFIFO_H__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <assert.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define SHM_SIZE (3 * 1024 * 1024)

    typedef enum
    {
        SHMFIFO_INTELLI_TIMEOUT_NONE = 0,   // 非阻塞，立即返回
        SHMFIFO_TIMEOUT_FOREVER = 0xFFFFFFF // 阻塞
    } shmfifo_timeout_e;

    // 共享内存首部定义
    typedef struct shmhead
    {
        unsigned int blocksize; // 块大小,假定放到缓冲区的数据块的定长的；
        unsigned int blocks;    // 总块数
        unsigned int rd_index;  // 读索引,表示消费位置，消费者进程从哪个位置消费产品
        unsigned int wr_index;  // 写索引,表示生产位置，生产者进程从哪个位置生产产品
    } shmhead_t;

    // 共享内存消息队列
    typedef struct shmfifo
    {
        shmhead_t *p_shm; // 共享内存头部指针
        char *p_payloadd; // 有效负载的起始地址

        int shmid; // 共享内存ID

        // 典型的生产者和消费者问题需要3个信号量
        int sem_mutex; // 用来互斥用的信号量,多个生产者进程可以生产产品，所以需要互斥信号量，消费者类似
        int sem_full;  // 用来控制共享内存是否满的信号量,当前可以消费的产品的个数
        int sem_empty; // 用来控制共享内存是否空的信号量,当前可以生产的产品的个数
    } shmfifo_t;

    // 共享内存类型
    typedef enum
    {
        IMG,
        AI_RESULT
    } shm_type_e;

    // 共享内存内容
    typedef struct
    {
        shm_type_e type;
        int32_t memory_length;
        char memory[SHM_SIZE];
    } img_ai_result_t;

    /**************************************************
    *函数名: shmfifo_init
    *功能  : 创建共享内存队列
    *输入  : int key : 共享内存键值
            int blocksize : 共享内存块大小
            int blocks : 共享内存块总数
    *输出  : 无
    *返回值: 成功 shmfifo_t *，失败 NULL
    *注释  :
    ***************************************************/
    shmfifo_t *shmfifo_init(int key, int blocksize, int blocks);

    /**************************************************
    *函数名: shmfifo_put
    *功能  : 入共享内存队列
    *输入  : shmfifo_t *fifo : 共享内存句柄指针
            const void *buf : 入共享内存队列缓存指针
            shmfifo_timeout_e type : 入共享内存队列方式，当前支持非阻塞和阻塞两种方式
    *输出  : 无
    *返回值: 成功 0，失败 -1
    *注释  :
    ***************************************************/
    int shmfifo_put(shmfifo_t *fifo, const void *buf, shmfifo_timeout_e type);

    /**************************************************
    *函数名: shmfifo_get
    *功能  : 出共享内存队列
    *输入  : shmfifo_t *fifo : 共享内存句柄指针
            void *buf : 出共享内存队列缓存指针
            shmfifo_timeout_e type : 出共享内存队列方式，当前支持非阻塞和阻塞两种方式
    *输出  : 无
    *返回值: 成功 0，失败 -1
    *注释  :
    ***************************************************/
    int shmfifo_get(shmfifo_t *fifo, void *buf, shmfifo_timeout_e type);

    /**************************************************
     *函数名: shmfifo_destroy
     *功能  : 销毁共享内存队列
     *输入  : shmfifo_t *fifo : 共享内存句柄指针
     *输出  : 无
     *返回值: 无
     *注释  :
     ***************************************************/
    void shmfifo_destroy(shmfifo_t *fifo);

    /**************************************************
     *函数名: shmfifo_get_empty_count
     *功能  : 获取empty队列可用资源数
     *输入  : shmfifo_t *fifo : 共享内存句柄指针
     *输出  : 无
     *返回值: empty队列可用资源数
     *注释  :
     ***************************************************/
    int shmfifo_get_empty_count(shmfifo_t *fifo);

    /**************************************************
     *函数名: shmfifo_get_full_count
     *功能  : 获取full队列可用资源数
     *输入  : shmfifo_t *fifo : 共享内存句柄指针
     *输出  : 无
     *返回值: full队列可用资源数
     *注释  :
     ***************************************************/
    int shmfifo_get_full_count(shmfifo_t *fifo);

#ifdef __cplusplus
}
#endif

#endif //__SHMFIFO_H__