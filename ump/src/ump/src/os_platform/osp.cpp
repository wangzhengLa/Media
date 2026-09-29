#include <malloc.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <dlfcn.h>
#include <net/if.h>
#include <dirent.h>
#include <string>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <fcntl.h>
#include "osp.h"

#define SHELL_PATH "/bin/sh" /* Path of the shell.  */
#define SHELL_NAME "sh"      /* Name to give it.  */
#define ASYNC_IO_OK 0
#define ASYNC_IO_CLOSED -1
#define ASYNC_IO_FAILED -2

typedef struct
{
    pthread_mutex_t tMutex;
} TCritiSec;
typedef struct
{
    pthread_cond_t cond;
} TCond;
typedef struct
{
    pthread_t tHandle;
} TThread;
typedef struct
{
    unsigned int year;
    unsigned int month;
    unsigned int day;
    unsigned int hour;
    unsigned int min;
    unsigned int sec;
    unsigned int msec;
} TDateTime;

// 工具函数
void IntelliUtilSleep(uint32_t u32SleepUs)
{
    struct timespec tm;

    if (u32SleepUs)
    {
        tm.tv_sec = u32SleepUs / 1000000;
        tm.tv_nsec = u32SleepUs % 1000000 * 1000;
        nanosleep(&tm, NULL);
    }
    else
    {
#if defined(ARCH_ARM_X64_LINUX) || defined(ARCH_ARM_X86_LINUX)

#else
        pthread_yield();
#endif
    }
}
void IntelliUtilSleepMs(uint32_t u32Millisecs)
{
    struct timespec short_wait;
    struct timespec remainder;
    short_wait.tv_sec = u32Millisecs / 1000;
    short_wait.tv_nsec = (u32Millisecs % 1000) * 1000 * 1000;
    nanosleep(&short_wait, &remainder);
}
uint32_t IntelliUtilGetCurTimeInMsec()
{
    static int isInit = false;
    static unsigned int initTime = 0;
    unsigned int curTime;
    long long timestamp;
    struct timespec ts;

    if (isInit == false)
    {
        if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        {
            return -1;
        }
        else
        {
            timestamp = (long long)ts.tv_sec * 1000;
            timestamp += ts.tv_nsec / (1000 * 1000);
            initTime = (unsigned int)timestamp;
            isInit = true;
        }
    }

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
        return -1;
    }
    else
    {
        timestamp = (long long)ts.tv_sec * 1000;
        timestamp += ts.tv_nsec / (1000 * 1000);
        curTime = (unsigned int)timestamp;

        return (curTime - initTime);
    }
}
static int64_t IntelliUtilSystemTimeNanos()
{
    int64_t ticks;
    struct timespec ts;

    // TODO(deadbeef): Do we need to handle the case when CLOCK_MONOTONIC is not supported?
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ticks = kNumNanosecsPerSec * static_cast<int64_t>(ts.tv_sec) + static_cast<int64_t>(ts.tv_nsec);

    return ticks;
}
int64_t IntelliUtilTimeNanos()
{
    return IntelliUtilSystemTimeNanos();
}
int64_t IntelliUtilTimeMillis()
{
    return IntelliUtilSystemTimeNanos() / kNumNanosecsPerMillisec;
}
int64_t IntelliUtilTimeMicros()
{
    return IntelliUtilSystemTimeNanos() / kNumNanosecsPerMicrosec;
}
int64_t IntelliUtilTimeSeconds()
{
    return IntelliUtilSystemTimeNanos() / kNumNanosecsPerSec;
}
void *IntelliUtilLoadLibSymbol(char *pLibDirPath, char *pLibName, char *pSymbolName)
{
    void *pRetSymbol = NULL;
    void *pAppHandle = NULL;
    struct dirent *pDirent = NULL;
    DIR *dpDir = NULL;
    char achLibPath[256] = "";

    do
    {
        if (pLibDirPath == NULL || pLibName == NULL || pSymbolName == NULL)
        {
            break;
        }
        if (NULL == (dpDir = opendir(pLibDirPath)))
        {
            printf("%s %d: opendir %s Failed !\r\n", __FUNCTION__, __LINE__, pLibDirPath);
            break;
        }
        while (NULL != (pDirent = readdir(dpDir)))
        {
            if (0 == strcmp(pDirent->d_name, pLibName))
            {
                sprintf(achLibPath, "%s/%s", pLibDirPath, pDirent->d_name);
                pAppHandle = dlopen(achLibPath, RTLD_LAZY | RTLD_GLOBAL);
                if (NULL == pAppHandle)
                {
                    printf("%s %d: dlopen %s Failed: %s !\r\n", __FUNCTION__, __LINE__, achLibPath, dlerror());
                    break;
                }

                pRetSymbol = (void *)dlsym(pAppHandle, pSymbolName);
                if (NULL == pRetSymbol)
                {
                    printf("%s %d: dlsym %s Failed !\r\n", __FUNCTION__, __LINE__, pSymbolName);
                }
                break;
            }
        }
        closedir(dpDir);
    } while (0);

    return pRetSymbol;
}
static void IntelliUtilCloseFrom(int lowfd)
{
    long fd, maxfd;
    char fdpath[PATH_MAX], *endp;
    struct dirent *dent;
    DIR *dirp;
    int len;
    struct rlimit fdlimit = {0, 0};
    if (0 == getrlimit(RLIMIT_NOFILE, &fdlimit))
    {
        maxfd = fdlimit.rlim_max;
    }
    else
    {
        maxfd = 1024;
    }
    /* Check for a /proc/$$/fd directory. */
    len = snprintf(fdpath, sizeof(fdpath), "/proc/%ld/fd", (long)getpid());
    if (len > 0 && (size_t)len <= sizeof(fdpath) && (dirp = opendir(fdpath)))
    {
        while ((dent = readdir(dirp)) != NULL)
        {
            fd = strtol(dent->d_name, &endp, 10);
            if (dent->d_name != endp && *endp == '\0' &&
                fd >= 0 && fd < maxfd && fd >= lowfd && fd != dirfd(dirp))
                (void)close((int)fd);
        }
        (void)closedir(dirp);
    }
    else
    {
        for (fd = lowfd; fd < maxfd; fd++)
        {
            (void)close((int)fd);
        }
    }
}
int IntelliUtilSystem(const char *line)
{
    int status = 0;
    int save = 0;
    pid_t pid;
    struct sigaction sa, intr, quit;
    sigset_t block, omask;
    if (line == NULL)
        return IntelliUtilSystem("exit 0") == 0;

    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, &intr) < 0)
        return -1;
    if (sigaction(SIGQUIT, &sa, &quit) < 0)
    {
        save = errno;
        (void)sigaction(SIGINT, &intr, (struct sigaction *)NULL);
        errno = save;
        return -1;
    }
    sigemptyset(&block);
    sigaddset(&block, SIGCHLD);
    // SIGCHLD信号行为被设置为SIG_IGN时，waitpid()函数有可能因为找不到子进程而报ECHILD错误,
    // 在调用system()函数前重新设置SIGCHLD信号为缺省值，即signal(SIGCHLD, SIG_DFL)
    // 解决出现system系统调用waitpid错误 no child process
    signal(SIGCHLD, SIG_DFL);
    save = errno;
    if (sigprocmask(SIG_BLOCK, &block, &omask) < 0)
    {
        if (errno == ENOSYS)
            errno = save;
        else
        {
            save = errno;
            (void)sigaction(SIGINT, &intr, (struct sigaction *)NULL);
            (void)sigaction(SIGQUIT, &quit, (struct sigaction *)NULL);
            errno = save;
            return -1;
        }
    }
    pid = vfork();
    if (pid == (pid_t)0)
    {
        const char *new_argv[4];
        new_argv[0] = SHELL_NAME;
        new_argv[1] = "-c";
        new_argv[2] = line;
        new_argv[3] = NULL;
        // 关闭继承来的fd
        IntelliUtilCloseFrom(3);
        /* Restore the signals.  */
        (void)sigaction(SIGINT, &intr, (struct sigaction *)NULL);
        (void)sigaction(SIGQUIT, &quit, (struct sigaction *)NULL);
        (void)sigprocmask(SIG_SETMASK, &omask, (sigset_t *)NULL);
        /* Exec the shell.  */
        (void)execv(SHELL_PATH, (char *const *)new_argv);
        _exit(127);
    }
    /* The fork failed.  */
    else if (pid < (pid_t)0)
        status = -1;
    /* Parent side.  */
    else
    {
        int n;
        do
        {
            n = waitpid(pid, &status, 0);
        } while (n == -1 && errno == EINTR);
        if (n != pid)
            status = -1;
    }
    save = errno;
    if ((sigaction(SIGINT, &intr, (struct sigaction *)NULL) | sigaction(SIGQUIT, &quit, (struct sigaction *)NULL) | sigprocmask(SIG_SETMASK, &omask, (sigset_t *)NULL)) != 0)
    {
        if (errno == ENOSYS)
            errno = save;
        else
            return -1;
    }
    return status;
}
static void *IntelliUtilCreateThreadEx(void *(*thread_proc)(void *), void *pvArg, const pthread_attr_t *ptThreadAttr)
{
    TThread *ptThread = (TThread *)malloc(sizeof(TThread));
    if (ptThread)
    {
        if (pthread_create(&ptThread->tHandle, ptThreadAttr, thread_proc, pvArg) != 0)
        {
            free(ptThread);
            return 0;
        }
    }

    return ptThread;
}
void IntelliUtilCloseThread(void *pvThread)
{
    TThread *ptThread = (TThread *)pvThread;
    if (ptThread)
    {
        pthread_join(ptThread->tHandle, 0);
        free(ptThread);
    }
}
int IntelliUtilTaskCreate(TIntelliUtilTskObj *ptTskObj)
{
    int status;

    unsigned int policy;
    int pri;
    unsigned int stackSize;
    pthread_attr_t thread_attr;
    struct sched_param schedprm;

    status = IUTIL_SOK;

    if (NULL == ptTskObj->entryFunc)
    {
        printf("IntelliUtilTaskCreate entryFunc=NULL\n");
        return IUTIL_SFAIL;
    }

    if (NULL != ptTskObj->entryName)
    {
        printf("IntelliUtilTaskCreate %s\n", ptTskObj->entryName);
    }

    stackSize = ptTskObj->stackSize;
    policy = ptTskObj->policy;
    pri = ptTskObj->pri;

    status = pthread_attr_init(&thread_attr);
    if (status != IUTIL_SOK)
    {
        printf("pthread_attr_init() - Could not initialize thread attributes\n");
    }

    if (SCHED_FIFO == policy)
    {
        pthread_attr_setschedpolicy(&thread_attr, SCHED_FIFO);
        if (pri == 0)
        {
            pri = (sched_get_priority_min(SCHED_FIFO) + (sched_get_priority_max(SCHED_FIFO) - sched_get_priority_min(SCHED_FIFO)) / 2);
        }
        if (pri > sched_get_priority_max(SCHED_FIFO))
        {
            pri = sched_get_priority_max(SCHED_FIFO);
        }
        if (pri < sched_get_priority_min(SCHED_FIFO))
        {
            pri = sched_get_priority_min(SCHED_FIFO);
        }
    }
    else
    {
        pthread_attr_setschedpolicy(&thread_attr, SCHED_OTHER);
        if (pri == 0)
        {
            pri = (sched_get_priority_min(SCHED_OTHER) + (sched_get_priority_max(SCHED_OTHER) - sched_get_priority_min(SCHED_OTHER)) / 2);
        }
        if (pri > sched_get_priority_max(SCHED_OTHER))
        {
            pri = sched_get_priority_max(SCHED_OTHER);
        }
        if (pri < sched_get_priority_min(SCHED_OTHER))
        {
            pri = sched_get_priority_min(SCHED_OTHER);
        }
    }
    schedprm.sched_priority = pri;
    status |= pthread_attr_setschedparam(&thread_attr, &schedprm);
    if (status != IUTIL_SOK)
    {
        printf("OSA_thrCreate() - Could not initialize thread attributes\n");
    }

    if (stackSize != 0)
    {
        pthread_attr_setstacksize(&thread_attr, stackSize);
    }

    ptTskObj->pvTaskHandle = IntelliUtilCreateThreadEx(ptTskObj->entryFunc, ptTskObj->prm, &thread_attr);
    if (NULL == ptTskObj->pvTaskHandle)
    {
        printf("IntelliUtilCreateThreadEx Could not create thread [%d]\n", status);
        status = IUTIL_SFAIL;
    }

    pthread_attr_destroy(&thread_attr);

    ptTskObj->stackSize = stackSize;
    ptTskObj->policy = policy;
    ptTskObj->pri = pri;

    return status;
}

// 文件内存转换
char *IUtilStrncpy(char *szDst, const char *szSrc, uint32_t dwSize)
{
    if (NULL != szDst && NULL != szSrc && 0 < dwSize)
    {
        szDst = strncpy(szDst, szSrc, dwSize);
        szDst[dwSize - 1] = 0;
    }

    return szDst;
}
static void replaceS(char *szDirName, char oldS, char newS)
{
    char *tempDir = NULL;
    if (szDirName == NULL)
    {
        return;
    }
    tempDir = szDirName;
    while (*tempDir != '\0')
    {
        if (*tempDir == oldS)
        {
            *tempDir = newS;
        }
        tempDir++;
    }
    return;
}
static void ReplaceSlash(char *szDirName)
{
    replaceS(szDirName, '\\', '/');
}
static int IUtilAcess(const char *path, int val)
{
    char as8Path[1024] = {0};

    IUtilStrncpy(as8Path, path, sizeof(as8Path));
    ReplaceSlash(as8Path);

    return access(as8Path, val);
}
int IUtilCheckFileExist(const char *ps8FilePath)
{
    if (0 != IUtilAcess(ps8FilePath, 0))
    {
        return -1;
    }

    return 0;
}
int IUtilFileSize(const char as8File[], uint32_t *pu32Size)
{
    struct stat tStatBuf = {0};
    uint32_t u32FileLength = 0;
    FILE *pFile = NULL;
    int32_t l32Ret = 0;

    do
    {
        if (0 != IUtilCheckFileExist(as8File))
        {
            l32Ret = -1;
            break;
        }

        // 获取文件大小属性
        if (stat(as8File, &tStatBuf) < 0)
        {
            pFile = fopen(as8File, "rb");
            if (NULL == pFile)
            {
                l32Ret = -1;
                break;
            }

            fseek(pFile, 0, SEEK_END);
            u32FileLength = ftell(pFile);
            fseek(pFile, 0, SEEK_SET);
        }
        else
        {
            u32FileLength = tStatBuf.st_size;
        }

        *pu32Size = u32FileLength;
    } while (0);

    if (NULL != pFile)
    {
        fclose(pFile);
        pFile = NULL;
    }

    return l32Ret;
}
int IUtilMemToFile(uint8_t *pu8Bytes, uint32_t length, const char as8FileName[])
{
    int l32Ret = 0;
    FILE *pFile = NULL;

    do
    {
        pFile = fopen(as8FileName, "wb");
        if (NULL == pFile)
        {
            l32Ret = -1;
            break;
        }

        fwrite(pu8Bytes, length, 1, pFile);
    } while (0);

    if (NULL != pFile)
    {
        fflush(pFile);

        //.conf文件特殊处理，强制刷入磁盘
        if (NULL != strstr(as8FileName, ".conf"))
        {
            fsync(fileno(pFile));
        }

        fclose(pFile);
        pFile = NULL;
    }

    return l32Ret;
}
int IUtilMemToFile2plane(char *filename, char *y, char *uv, unsigned int y_size, unsigned int uv_size)
{

    FILE *yuvFd = NULL;
    char *buffer = NULL;

    yuvFd = fopen(filename, "w+");

    if (yuvFd == NULL)
    {
        return -1;
    }

    buffer = (char *)malloc(y_size + uv_size);

    if (buffer == NULL)
    {
        fclose(yuvFd);
        yuvFd = NULL;
        return -1;
    }

    memcpy(buffer, y, y_size);
    memcpy(buffer + y_size, uv, uv_size);

    fwrite(buffer, 1, y_size + uv_size, yuvFd);
    fflush(yuvFd);

    fclose(yuvFd);
    yuvFd = NULL;

    free(buffer);
    buffer = NULL;

    return 0;
}
int IUtilImageToFile(uint8_t *y, uint8_t *u, uint8_t *v, uint32_t width, uint32_t height, uint32_t stride_y, uint32_t stride_u, uint32_t stride_v, uint32_t image_type, const char as8FileName[])
{
    int l32Ret = 0;
    FILE *pFile = NULL;

    pFile = fopen(as8FileName, "wb+");
    if (NULL == pFile)
    {
        l32Ret = -1;
        return l32Ret;
    }

    for (int i = 0; i < height; i++)
    {
        fwrite(y + stride_y * i, 1, width, pFile);
    }
    // I420
    if (0 == image_type)
    {
        for (int i = 0; i < height / 2; i++)
        {
            fwrite(u + stride_u * i, 1, width / 2, pFile);
        }
        for (int i = 0; i < height / 2; i++)
        {
            fwrite(v + stride_v * i, 1, width / 2, pFile);
        }
    }
    // NV12 NV21
    else if (1 == image_type || 2 == image_type)
    {
        for (int i = 0; i < height / 2; i++)
        {
            fwrite(u + stride_u * i, 1, width, pFile);
        }
    }

    fflush(pFile);
    fclose(pFile);
    pFile = NULL;

    return l32Ret;
}
int IUtilFileToMem(const char as8File[], uint32_t *pu32Size, uint8_t **ppu8Bytes)
{
    int l32Ret = 0;
    FILE *pFile = NULL;
    uint32_t u32FileLength = 0;

    do
    {
        l32Ret = IUtilFileSize(as8File, &u32FileLength);
        if (l32Ret != 0)
        {
            l32Ret = -1;
            break;
        }

        pFile = fopen(as8File, "rb");
        if (NULL == pFile)
        {
            l32Ret = -1;
            break;
        }

        *ppu8Bytes = (uint8_t *)IntelliUtilMalloc(u32FileLength + 1);
        if (NULL == *ppu8Bytes)
        {
            l32Ret = -1;
            break;
        }
        memset(*ppu8Bytes, 0, (size_t)u32FileLength + 1);

        // 将文件读到内存
        if (1 != fread(*ppu8Bytes, u32FileLength, 1, pFile))
        {
            l32Ret = -1;
            break;
        }

        *pu32Size = u32FileLength;
    } while (0);

    if (NULL != pFile)
    {
        fclose(pFile);
        pFile = NULL;
    }

    return l32Ret;
}
int IUtilMvfile(const char *src, const char *dst)
{
    if (src == NULL || dst == NULL)
        return -1;

    char as8Src[500] = {0};
    char as8Dst[500] = {0};

    if (strlen(src) >= sizeof(as8Src))
        return -1;
    if (strlen(dst) >= sizeof(as8Dst))
        return -1;

    IUtilStrncpy(as8Src, src, sizeof(as8Src));
    ReplaceSlash(as8Src);
    IUtilStrncpy(as8Dst, dst, sizeof(as8Dst));
    ReplaceSlash(as8Dst);

    char as8cmd[1024] = {0};
    snprintf(as8cmd, sizeof(as8cmd), "mv %s %s", as8Src, as8Dst);
    if (-1 == IntelliUtilSystem(as8cmd))
        return -1;

    return 0;
}
int IUtilRmfile(const char *path)
{
    char as8Path[1000] = {0};

    if (path == NULL)
        return -1;

    if (strlen(path) >= sizeof(as8Path))
        return -1;

    IUtilStrncpy(as8Path, path, sizeof(as8Path));
    ReplaceSlash(as8Path);

    // 存在*号的是删除一组文件
    if (NULL != strchr(as8Path, '*'))
    {
        char as8Cmd[1024] = {0};
        snprintf(as8Cmd, sizeof(as8Cmd), "rm -rf %s", as8Path);
        IntelliUtilSystem(as8Cmd);
    }
    else
    {
        remove(as8Path);
    }

    return 0;
}

// 临界区
void *IntelliUtilCreateCritiSec()
{
    TCritiSec *ptCS = (TCritiSec *)malloc(sizeof(TCritiSec));
    if (ptCS)
    {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE_NP);
        pthread_mutex_init(&ptCS->tMutex, &attr);
        pthread_mutexattr_destroy(&attr);
    }

    return ptCS;
}
void IntelliUtilCloseCritiSec(void *pvCriti)
{
    TCritiSec *ptCS = (TCritiSec *)pvCriti;
    if (ptCS)
    {
        pthread_mutex_destroy(&ptCS->tMutex);
        free(ptCS);
    }
}
int IntelliUtilLockCritiSec(void *pvCriti)
{
    TCritiSec *ptCS = (TCritiSec *)pvCriti;
    if (ptCS)
    {
        pthread_mutex_lock(&ptCS->tMutex);
    }

    return 0;
}
int IntelliUtilUnlockCritiSec(void *pvCriti)
{
    TCritiSec *ptCS = (TCritiSec *)pvCriti;
    if (ptCS)
    {
        pthread_mutex_unlock(&ptCS->tMutex);
    }

    return 0;
}

// 条件变量
void *IUtilCondCreate()
{
    TCond *ptCond = (TCond *)malloc(sizeof(TCond));
    pthread_condattr_t cond_attr;

    if (ptCond)
    {
        pthread_condattr_init(&cond_attr);
        pthread_cond_init(&ptCond->cond, &cond_attr);
        pthread_condattr_destroy(&cond_attr);
    }

    return ptCond;
}
int IUtilCondClose(void *pvCond)
{
    TCond *ptCond;

    if (NULL == pvCond)
    {
        return -1;
    }

    ptCond = (TCond *)pvCond;
    pthread_cond_destroy(&ptCond->cond);
    free(ptCond);

    return 0;
}
int IUtilCondWait(void *pvCond, void *pvMutex)
{
    int l32Ret = 0;
    TCond *ptCond;
    TCritiSec *ptMutex;

    if (pvCond == NULL || pvMutex == NULL)
        return -1;

    ptCond = (TCond *)pvCond;
    ptMutex = (TCritiSec *)pvMutex;
    l32Ret = pthread_cond_wait(&ptCond->cond, &ptMutex->tMutex);
    if (0 != l32Ret)
    {
        printf("IUtilCondWait failed %d\n", l32Ret);
    }

    return l32Ret;
}
int IUtilCondSignal(void *pvCond)
{
    TCond *ptCond;

    if (pvCond == NULL)
    {
        return -1;
    }

    ptCond = (TCond *)pvCond;
    pthread_cond_signal(&ptCond->cond);

    return 0;
}

// 内存管理
static void *AlignedMalloc(size_t l32Size, size_t l32AlignedSize)
{
    void *pvMallocPtr;
    void *pvAlignedPtr;

    /* Error if l32AlignedSize is not a power of two.  */
    if (l32AlignedSize & (l32AlignedSize - 1))
    {
        return ((void *)0);
    }

    if (l32Size == 0)
    {
        return ((void *)0);
    }

    /* Assume malloc'd pointer is aligned at least to sizeof (void*).
     * If necessary, add another sizeof (void*) to store the value
     * returned by malloc. Effectively this enforces a minimum alignment
     * of sizeof double. */
    // if (l32AlignedSize < 2 * sizeof(void *))
    // {
    //     l32AlignedSize = 2 * sizeof(void *);
    // }

    pvMallocPtr = malloc(l32Size + l32AlignedSize - 1 + sizeof(void *));
    if (!pvMallocPtr)
    {
        return ((void *)0);
    }

    /* Align  We have at least sizeof (void *) space below malloc'd ptr. */
    pvAlignedPtr = (void *)(((size_t)pvMallocPtr + l32AlignedSize - 1 + sizeof(void *)) & ~((size_t)(l32AlignedSize)-1));

    /* Store the original pointer just before p.*/
    ((void **)pvAlignedPtr)[-1] = pvMallocPtr;

    return pvAlignedPtr;
}
static void AlignedFree(void *pvAlignedPtr)
{
    if (pvAlignedPtr)
    {
        free(((void **)pvAlignedPtr)[-1]);
    }
}
static void *IntelliUtilAlignedMalloc(uint32_t u32Size, int32_t l32AlignSize)
{
    return AlignedMalloc(u32Size, l32AlignSize);
}
static void IntelliUtilAlignedFree(void *pvPtr)
{
    AlignedFree(pvPtr);
}
void *IntelliUtilMalloc(uint32_t u32Size)
{
    return IntelliUtilAlignedMalloc(u32Size, 8);
}
void IntelliUtilFree(void *pvPtr)
{
    IntelliUtilAlignedFree(pvPtr);
}

// webrtc内存管理
static uintptr_t GetRightAlign(uintptr_t start_pos, size_t alignment)
{
    // The pointer should be aligned with `alignment` bytes. The - 1 guarantees
    // that it is aligned towards the closest higher (right) address.
    return (start_pos + alignment - 1) & ~(alignment - 1);
}
static bool ValidAlignment(size_t alignment)
{
    if (!alignment)
    {
        return false;
    }
    // Alignment must be an integer power of two.
    return (alignment & (alignment - 1)) == 0;
}
static void *GetRightAlign(const void *pointer, size_t alignment)
{
    if (!pointer)
    {
        return NULL;
    }
    if (!ValidAlignment(alignment))
    {
        return NULL;
    }
    uintptr_t start_pos = reinterpret_cast<uintptr_t>(pointer);
    return reinterpret_cast<void *>(GetRightAlign(start_pos, alignment));
}
void *IUtilAlignedMalloc(size_t size, size_t alignment)
{
    if (size == 0)
    {
        return NULL;
    }
    if (!ValidAlignment(alignment))
    {
        return NULL;
    }

    // The memory is aligned towards the lowest address that so only
    // alignment - 1 bytes needs to be allocated.
    // A pointer to the start of the memory must be stored so that it can be
    // retreived for deletion, ergo the sizeof(uintptr_t).
    void *memory_pointer = malloc(size + sizeof(uintptr_t) + alignment - 1);
    // RTC_CHECK(memory_pointer) << "Couldn't allocate memory in AlignedMalloc";

    // Aligning after the sizeof(uintptr_t) bytes will leave room for the header
    // in the same memory block.
    uintptr_t align_start_pos = reinterpret_cast<uintptr_t>(memory_pointer);
    align_start_pos += sizeof(uintptr_t);
    uintptr_t aligned_pos = GetRightAlign(align_start_pos, alignment);
    void *aligned_pointer = reinterpret_cast<void *>(aligned_pos);

    // Store the address to the beginning of the memory just before the aligned
    // memory.
    uintptr_t header_pos = aligned_pos - sizeof(uintptr_t);
    void *header_pointer = reinterpret_cast<void *>(header_pos);
    uintptr_t memory_start = reinterpret_cast<uintptr_t>(memory_pointer);
    memcpy(header_pointer, &memory_start, sizeof(uintptr_t));

    return aligned_pointer;
}
void IUtilAlignedFree(void *mem_block)
{
    if (mem_block == NULL)
    {
        return;
    }
    uintptr_t aligned_pos = reinterpret_cast<uintptr_t>(mem_block);
    uintptr_t header_pos = aligned_pos - sizeof(uintptr_t);

    // Read out the address of the AlignedMemory struct from the header.
    uintptr_t memory_start_pos = *reinterpret_cast<uintptr_t *>(header_pos);
    void *memory_start = reinterpret_cast<void *>(memory_start_pos);
    free(memory_start);
}

// 文件目录操作
int IUtilRmdir(const char *path)
{
    char as8Cmd[1024] = {0}; // 消除gcc8编译警告，改为 >sizeof(as8Path) 长度
    char as8Path[1000] = {0};
    if (strlen(path) >= sizeof(as8Path))
    {
        printf("path=[%s] > [%d]\r\n", path, sizeof(as8Path));
        return -1;
    }
    IUtilStrncpy(as8Path, path, sizeof(as8Path));
    ReplaceSlash(as8Path);
    snprintf(as8Cmd, sizeof(as8Cmd), "rm -rf %s", as8Path);
    IntelliUtilSystem(as8Cmd);
    return 0;
}
int IUtilMkdir(const char *ps8Dir, int32_t l32IsDelete)
{
    char as8Cmd[1024] = {0};
    char as8NewDir[512] = {0};
    int l32Ret = 0;
    IUtilStrncpy(as8NewDir, ps8Dir, sizeof(as8NewDir));
    ReplaceSlash(as8NewDir);
    l32Ret = IUtilCheckFileExist(as8NewDir);
    if (l32Ret == 0 && l32IsDelete == 1)
    {
        IUtilRmdir(as8NewDir);
        l32Ret = -1;
    }
    if (l32Ret != 0)
    {
        snprintf(as8Cmd, sizeof(as8Cmd), "mkdir -p %s", as8NewDir);
        IntelliUtilSystem(as8Cmd);
        l32Ret = IUtilCheckFileExist(as8NewDir);
    }
    return l32Ret;
}
int IntelliUtilMkDir(char *path)
{
    return IUtilMkdir(path, TRUE);
}
const char *IUtilWorkDir()
{
    static std::string s_dir;
    char as8Buf[512] = {0};
    char *ps8Ret = NULL;
    if (s_dir.length() == 0)
    {
        ps8Ret = getcwd(as8Buf, sizeof(as8Buf));
        if (NULL != ps8Ret)
        {
            s_dir = std::string(as8Buf);
        }
    }
    return s_dir.c_str();
}

// 获取系统当前时间（UTC）
static void IUtilGetTime(long long *pSysLocalTime)
{
    struct timeval tv;
    struct timezone tz;
    long long sysTime = 0;
    long long s64sec = 0;
    long long s64usec = 0;

    if (gettimeofday(&tv, &tz) < 0)
    {
        *pSysLocalTime = -1;
        return;
    }

    s64sec = (long long)(tv.tv_sec);
    s64usec = (long long)tv.tv_usec;
    sysTime = s64sec * 1000 + s64usec / 1000;

    *pSysLocalTime = sysTime; // 返回ms

    return;
}
static void IUtilMsec2TimeStruct(long long Int64MscTime, TDateTime *pTDateTime)
{
    time_t timeInSec;
    struct tm tNowTime;

    if (Int64MscTime == -1 || pTDateTime == NULL)
    {
        return;
    }

    timeInSec = (time_t)(Int64MscTime / 1000);

    gmtime_r(&timeInSec, &tNowTime);

    pTDateTime->year = tNowTime.tm_year + 1900;
    pTDateTime->month = tNowTime.tm_mon + 1;
    pTDateTime->day = tNowTime.tm_mday;
    pTDateTime->hour = tNowTime.tm_hour;
    pTDateTime->min = tNowTime.tm_min;
    pTDateTime->sec = tNowTime.tm_sec;
    pTDateTime->msec = (unsigned int)(Int64MscTime % 1000);
}
static int IUtilWriteLogToFile(char *ps8LogInfo)
{
#define INTELLI_IPC_LOG_DIR "/data/log"
#define INTELLI_UTILS_LOG_NAME "ump"

    // 错误日志有两个，交替使用。以保证之前一段时间的错误记录都可见。
    static const int l32MaxLogSize = 512000; // 最大512KB
    static unsigned char u8CurFileIdx = 0;
    static int bFirstWrite = 1;
    static void *pvIUtilLogLock = NULL;

    uint32_t u32Size1 = 0, u32Size2 = 0;
    int nRet0, nRet1;
    FILE *pFile = NULL;
    long long s64LocalTime;
    TDateTime tDateTime;
    char as8LogFileName[255] = {0};
    char as8CurTimeInfo[255] = {0};

    if (NULL == ps8LogInfo)
    {
        return -1;
    }

    // 如果日志目录不存在，则创建
    IUtilMkdir(INTELLI_IPC_LOG_DIR, 0);

    // 判断已有日志文件情况下，写入那个文件中
    if (bFirstWrite)
    {
        snprintf(as8LogFileName, 255, "%s%s0.log", INTELLI_IPC_LOG_DIR, INTELLI_UTILS_LOG_NAME);
        nRet0 = IUtilFileSize(as8LogFileName, &u32Size1);

        snprintf(as8LogFileName, 255, "%s%s1.log", INTELLI_IPC_LOG_DIR, INTELLI_UTILS_LOG_NAME);
        nRet1 = IUtilFileSize(as8LogFileName, &u32Size2);

        if (nRet0 == 0 && nRet1 == 0 && u32Size2 < u32Size1)
        {
            u8CurFileIdx = 1;
        }

        // 创建锁
        pvIUtilLogLock = IntelliUtilCreateCritiSec();
        if (NULL == pvIUtilLogLock)
            return -1;

        bFirstWrite = 0;
    }

    // 确认写入那个文件中
    snprintf(as8LogFileName, 255, "%s%s%u.log", INTELLI_IPC_LOG_DIR, INTELLI_UTILS_LOG_NAME, u8CurFileIdx);

    IntelliUtilLockCritiSec(pvIUtilLogLock);

    pFile = fopen(as8LogFileName, "a+");
    if (NULL != pFile)
    {
        int dwFileLen;
        fseek(pFile, 0, SEEK_END);
        dwFileLen = ftell(pFile);
        if (dwFileLen > l32MaxLogSize)
        {
            // 一个文件写满，切换到另外一个文件
            fclose(pFile);
            u8CurFileIdx = (u8CurFileIdx + 1) % 2;
            snprintf(as8LogFileName, 255, "%s%s%u.log",
                     INTELLI_IPC_LOG_DIR, INTELLI_UTILS_LOG_NAME, u8CurFileIdx);

            pFile = fopen(as8LogFileName, "w");
        }
    }

    if (NULL != pFile)
    {
        IUtilGetTime(&s64LocalTime);
        IUtilMsec2TimeStruct(s64LocalTime, &tDateTime);

        snprintf(as8CurTimeInfo, sizeof(as8CurTimeInfo), "%04d-%02d-%02d %02d:%02d:%02d ",
                 tDateTime.year, tDateTime.month, tDateTime.day,
                 tDateTime.hour, tDateTime.min, tDateTime.sec);

        fputs(as8CurTimeInfo, pFile); // 写入时间
        fputs(ps8LogInfo, pFile);     // 写入log信息
        fclose(pFile);
    }

    IntelliUtilUnlockCritiSec(pvIUtilLogLock);

    return 0;
}
void IUtilOspPrintfS(char *ps8String)
{
    // 实现单次打印4096字节
    const int l32BufTmpLen = 4098;
    char as8StrTmp[l32BufTmpLen];
    int l32StrLen = (int)strlen(ps8String);
    int l32CharLeft = l32StrLen; // 剩余未打印的字符数
    char *ps8Tmp = ps8String;

    do
    {
        memset(as8StrTmp, 0, l32BufTmpLen);

        IUtilStrncpy(as8StrTmp, ps8Tmp, l32BufTmpLen - 2);

        l32CharLeft = l32CharLeft - (int)strlen(as8StrTmp); // 更新剩余字符数
        ps8Tmp = ps8Tmp + strlen(as8StrTmp);                // 待拷贝位置

        // 日志保存到文件
        as8StrTmp[strlen(as8StrTmp)] = '\n'; // 补充换行符(使log时间位于行首)
        IUtilWriteLogToFile(as8StrTmp);

    } while (l32CharLeft > 0);

    return;
}
void IUtilOspPrintf(const char *szFormat, ...)
{
    char as8MsgBuf[4096] = {0};
    va_list pvList;
    int nActLen = 0;

    va_start(pvList, szFormat);
    nActLen = vsnprintf(as8MsgBuf, sizeof(as8MsgBuf), szFormat, pvList);
    va_end(pvList);

    // 错误日志保存到文件
    IUtilWriteLogToFile(as8MsgBuf);
}
