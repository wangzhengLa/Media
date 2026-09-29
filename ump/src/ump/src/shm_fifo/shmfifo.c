#include <sys/types.h>
#include <sys/msg.h>
#include <errno.h>
#include <unistd.h>
#include "shmfifo.h"

#define ERR_EXIT(m)         \
    do                      \
    {                       \
        perror(m);          \
        exit(EXIT_FAILURE); \
    } while (0);

union semun
{
    int val;               /* Value for SETVAL */
    struct semid_ds *buf;  /* Buffer for IPC_STAT, IPC_SET */
    unsigned short *array; /* Array for GETALL, SETALL */
    struct seminfo *__buf; /* Buffer for IPC_INFO (Linux-specific) */
};

static int sem_creat(key_t key)
{
    int semid;
    semid = semget(key, 1, IPC_CREAT | IPC_EXCL | 0666);
    if (semid == -1)
    {
        ERR_EXIT("semget");
    }
    return semid;
}

static int sem_getval(int semid)
{
    int ret;
    ret = semctl(semid, 0, GETVAL);
    if (ret == -1)
    {
        ERR_EXIT("getval")
    }
    return ret;
}

static int sem_setval(int semid, int val)
{
    union semun su;
    su.val = val;
    int ret;
    ret = semctl(semid, 0, SETVAL, su);
    if (ret == -1)
    {
        ERR_EXIT("setval")
    }
    return 0;
}

static int sem_p(int semid)
{
    struct sembuf sembuf;
    sembuf.sem_num = 0;
    sembuf.sem_op = -1;
    sembuf.sem_flg = 0;
    int ret;
    ret = semop(semid, &sembuf, 1);
    if (ret == -1)
    {
        ERR_EXIT("sem_p")
    }
    return ret;
}

static int sem_v(int semid)
{
    struct sembuf sembuf;
    sembuf.sem_num = 0;
    sembuf.sem_op = 1;
    sembuf.sem_flg = 0;
    int ret;
    ret = semop(semid, &sembuf, 1);
    if (ret == -1)
    {
        ERR_EXIT("sem_v")
    }
    return ret;
}

static int sem_d(int semid)
{
    int ret;
    ret = semctl(semid, 0, IPC_RMID, NULL);
    if (ret == -1)
    {
        ERR_EXIT("rm_sem")
    }
    return 0;
}

static int sem_open(key_t key)
{
    int semid;
    semid = semget(key, 0, 0);
    if (semid == -1)
    {
        ERR_EXIT("semget");
    }
    return semid;
}

shmfifo_t *shmfifo_init(int key, int blocksize, int blocks)
{
    shmfifo_t *fifo = (shmfifo_t *)malloc(sizeof(shmfifo_t));
    if (fifo == NULL)
    {
        printf("--------------fifo malloc failed------------\n");
        goto MALLOC_FAIL;
    }

    memset(fifo, 0, sizeof(shmfifo_t));
    int shmid = shmget(key, 0, 0); // 打开共享内存

    int size = sizeof(shmhead_t) + blocks * blocksize; // blocks * blocksize：每块大小乘以总块数
    if (shmid == -1)                                   // 打不开共享内存，则创建
    {
        // 创建共享内存
        fifo->shmid = shmget((key_t)key, size, 0666 | IPC_CREAT);
        if (fifo->shmid == -1)
        {
            printf("--------------shmget failed------------\n");
            goto SHMGET_FAIL;
        }

        // 初始化共享内存的头部指针
        fifo->p_shm = (shmhead_t *)shmat(fifo->shmid, NULL, 0); // NULL：表示内核自动选择，0:这段共享内存段既可读也可写
        if (fifo->p_shm == (void *)-1)
        {
            printf("--------------shmat failed------------\n");
            goto SHMAT_FAIL;
        }

        // system v共享内存的key可以和信号量的key可以一样的，但是2个共享内存或者2个信号量的key不能是一样的
        fifo->sem_mutex = sem_creat(key + 1);
        fifo->sem_full = sem_creat(key + 2);
        fifo->sem_empty = sem_creat(key + 3);

        sem_setval(fifo->sem_mutex, 1);
        sem_setval(fifo->sem_full, 0);       // 可以消费的产品个数为0
        sem_setval(fifo->sem_empty, blocks); // 初始化仓库的大小

        fifo->p_payloadd = (char *)(fifo->p_shm + 1); // 有效负载的位置

        // 初始化其他成员变量
        memset(fifo->p_shm, 0, sizeof(shmhead_t));
        fifo->p_shm->blocks = blocks;
        fifo->p_shm->blocksize = blocksize;
        fifo->p_shm->rd_index = 0;
        fifo->p_shm->wr_index = 0;
    }
    else // 打开共享内存，则连接
    {
        fifo->shmid = shmid;
        fifo->p_shm = (shmhead_t *)shmat(fifo->shmid, NULL, 0);
        if (fifo->p_shm == (void *)-1)
        {
            printf("--------------shmat failed------------\n");
            goto SHMAT_FAIL;
        }

        // 因为共享内存存在，则信号量也存在
        fifo->sem_mutex = sem_open(key + 1);
        fifo->sem_full = sem_open(key + 2);
        fifo->sem_empty = sem_open(key + 3);
        fifo->p_payloadd = (char *)(fifo->p_shm + 1);
    }

    return fifo;

SHMAT_FAIL:
    shmctl(fifo->shmid, IPC_RMID, 0);
SHMGET_FAIL:
    free(fifo);
MALLOC_FAIL:
    return NULL;
}

int shmfifo_put(shmfifo_t *fifo, const void *buf, shmfifo_timeout_e type)
{
    int sem_empty = sem_getval(fifo->sem_empty);
    if (0 == sem_empty && SHMFIFO_INTELLI_TIMEOUT_NONE == type)
    {
        printf("--------------shmfifo has no empty buffer------------\n");
        return -1;
    }

    sem_p(fifo->sem_empty);
    sem_p(fifo->sem_mutex);

    // 生产产品
    memcpy(fifo->p_payloadd + fifo->p_shm->blocksize * fifo->p_shm->wr_index, buf, fifo->p_shm->blocksize);

    // 下一次要生产的位置要发生改变，若加到了队列末尾，需要循环到头部开始(环形缓冲区)，所以模%上总块数
    fifo->p_shm->wr_index = (fifo->p_shm->wr_index + 1) % fifo->p_shm->blocks;

    sem_v(fifo->sem_mutex);
    sem_v(fifo->sem_full);

    return 0;
}

int shmfifo_get(shmfifo_t *fifo, void *buf, shmfifo_timeout_e type)
{
    int sem_full = sem_getval(fifo->sem_full);
    if (0 == sem_full && SHMFIFO_INTELLI_TIMEOUT_NONE == type)
    {
        printf("--------------shmfifo has no full buffer------------\n");
        return -1;
    }

    sem_p(fifo->sem_full);
    sem_p(fifo->sem_mutex);

    // 消费产品
    memcpy(buf, fifo->p_payloadd + fifo->p_shm->blocksize * fifo->p_shm->rd_index, fifo->p_shm->blocksize);

    // 下一次要消费的位置要发生改变，若加到了队列末尾，需要循环到头部开始(环形缓冲区)，所以模%上总块数
    fifo->p_shm->rd_index = (fifo->p_shm->rd_index + 1) % fifo->p_shm->blocks;

    sem_v(fifo->sem_mutex);
    sem_v(fifo->sem_empty);

    return 0;
}

void shmfifo_destroy(shmfifo_t *fifo)
{
    sem_d(fifo->sem_mutex);
    sem_d(fifo->sem_empty);
    sem_d(fifo->sem_full);

    shmdt(fifo->p_shm);
    shmctl(fifo->shmid, IPC_RMID, 0);
    free(fifo);
}

int shmfifo_get_empty_count(shmfifo_t *fifo)
{
    return sem_getval(fifo->sem_empty);
}

int shmfifo_get_full_count(shmfifo_t *fifo)
{
    return sem_getval(fifo->sem_full);
}
