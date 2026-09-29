#ifndef OSP_H
#define OSP_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C"
{
#endif

#ifndef IUTIL_SOK
#define IUTIL_SOK 0
#endif
#ifndef IUTIL_SFAIL
#define IUTIL_SFAIL -1
#endif
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef MIN
#define MIN(a, b) ((a) > (b) ? (b) : (a))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef ABS
#define ABS(x) ((x) >= 0 ? (x) : (-(x)))
#endif
#ifndef ALIGN
#define ALIGN(x, a) (((x) + (a)-1) & ~((a)-1))
#endif
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(__ary) (sizeof(__ary) / sizeof(__ary[0]))
#endif

        static const int64_t kNumMillisecsPerSec = INT64_C(1000);
        static const int64_t kNumMicrosecsPerSec = INT64_C(1000000);
        static const int64_t kNumNanosecsPerSec = INT64_C(1000000000);

        static const int64_t kNumMicrosecsPerMillisec = INT64_C(1000);
        static const int64_t kNumNanosecsPerMillisec = INT64_C(1000000);
        static const int64_t kNumNanosecsPerMicrosec = INT64_C(1000);

        typedef void *(*IntelliTskEntryFunc)(void *);
        typedef struct
        {
                void *pvTaskHandle;
                unsigned int cpuCore;
                unsigned int policy;
                unsigned int pri;
                unsigned int stackSize;
                IntelliTskEntryFunc entryFunc;
                char *entryName;
                void *prm;
        } TIntelliUtilTskObj;

        /**************************************************
         *函数名: IntelliUtilSleep
         *功能  : 休眠函数
         *输入  : uint32_t u32SleepUs : 休眠微妙
         *输出  : 无
         *返回值: 无
         *注释  :
         ***************************************************/
        void IntelliUtilSleep(uint32_t u32SleepUs);

        /**************************************************
         *函数名: IntelliUtilSleepMs
         *功能  : 休眠函数
         *输入  : uint32_t u32Millisecs : 休眠毫妙
         *输出  : 无
         *返回值: 无
         *注释  :
         ***************************************************/
        void IntelliUtilSleepMs(uint32_t u32Millisecs);

        /**************************************************
         *函数名: IntelliUtilGetCurTimeInMsec
         *功能  : 获取系统时间
         *输入  :
         *输出  : 无
         *返回值: 当前系统时间 毫秒
         *注释  :
         ***************************************************/
        uint32_t IntelliUtilGetCurTimeInMsec();

        /**************************************************
         *函数名: IntelliUtilTimeMillis
         *功能  : 获取系统时间
         *输入  :
         *输出  : 无
         *返回值: 当前系统时间 毫秒
         *注释  :
         ***************************************************/
        int64_t IntelliUtilTimeMillis();

        /**************************************************
         *函数名: IntelliUtilTimeMicros
         *功能  : 获取系统时间
         *输入  :
         *输出  : 无
         *返回值: 当前系统时间 微妙
         *注释  :
         ***************************************************/
        int64_t IntelliUtilTimeMicros();

        /**************************************************
         *函数名: IntelliUtilTimeNanos
         *功能  : 获取系统时间
         *输入  :
         *输出  : 无
         *返回值: 当前系统时间 纳秒
         *注释  :
         ***************************************************/
        int64_t IntelliUtilTimeNanos();

        /**************************************************
         *函数名: IntelliUtilTimeSeconds
         *功能  : 获取系统时间
         *输入  :
         *输出  : 无
         *返回值: 当前系统时间 秒
         *注释  :
         ***************************************************/
        int64_t IntelliUtilTimeSeconds();

        /**************************************************
         *函数名: IntelliUtilSystem
         *功能  : 执行cmd命令
         *输入  : const char *line : 命令字符串
         *输出  : 无
         *返回值: 成功 0 失败 -1
         *注释  :
         ***************************************************/
        int IntelliUtilSystem(const char *line);

        /**************************************************
         *函数名: IntelliUtilCloseThread
         *功能  : 关闭线程
         *输入  : void *pvThread : 线程对象指针
         *输出  : 无
         *返回值: 无
         *注释  :
         ***************************************************/
        void IntelliUtilCloseThread(void *pvThread);

        /**************************************************
         *函数名: IntelliUtilTaskCreate
         *功能  : 创建线程任务
         *输入  : TIntelliUtilTskObj *ptTskObj : 线程任务指针
         *输出  : 无
         *返回值: 成功 0 失败 -1
         *注释  :
         ***************************************************/
        int IntelliUtilTaskCreate(TIntelliUtilTskObj *ptTskObj);

        /**************************************************
         *函数名: IUtilCheckFileExist
         *功能  : 检查文件是否存在
         *输入  : const char *ps8FilePath : 文件路径
         *输出  : 无
         *返回值: 成功 0 失败 -1
         *注释  :
         ***************************************************/
        int IUtilCheckFileExist(const char *ps8FilePath);

        /**************************************************
         *函数名: IUtilFileSize
         *功能  : 计算文件的大小
         *输入  : const char as8File[] : 文件路径
         *输出  : uint32_t *pu32Size : 文件实际大小
         *返回值: 成功 0 失败 -1
         *注释  :
         ***************************************************/
        int IUtilFileSize(const char as8File[], uint32_t *pu32Size);

        /**************************************************
         *函数名: IUtilMemToFile
         *功能  : 内存到文件
         *输入  : uint8_t *pu8Bytes : 文件内存数据
                 uint32_t length : 文件内存大小
                 const char as8FileName[] : 文件路径
         *输出  : 无
         *返回值: 成功 0 失败 -1
         *注释  :
         ***************************************************/
        int IUtilMemToFile(uint8_t *pu8Bytes, uint32_t length, const char as8FileName[]);

        /**************************************************
         *函数名: IUtilFileToMem
         *功能  : 文件到内存
         *输入  : const char as8File[] : 文件路径
         *输出  : uint32_t *pu32Size : 文件大小
                 uint8_t **ppu8Bytes : 文件实际内存指针
         *返回值: 成功 0 失败 -1
         *注释  :
         ***************************************************/
        int IUtilFileToMem(const char as8File[], uint32_t *pu32Size, uint8_t **ppu8Bytes);

        /**************************************************
         *函数名: IUtilMvfile
         *功能  : 文件重命名
         *输入  : const char *src : 源文件名
                 const char *dst : 目的文件名
         *输出  : 无
         *返回值: 成功 0 失败 -1
         *注释  :
         ***************************************************/
        int IUtilMvfile(const char *src, const char *dst);

        /**************************************************
         *函数名: IUtilRmfile
         *功能  : 文件删除
         *输入  : const char *path : 文件路径
         *输出  : 无
         *返回值: 成功 0 失败 -1
         *注释  :
         ***************************************************/
        int IUtilRmfile(const char *path);

        /**************************************************
         *函数名: IUtilImageToFile
         *功能  : 图片到文件
         *输入  : uint8_t *y : y分量指针
                 uint8_t *u : u分量指针
                 uint8_t *v ：v分量指针
                 uint32_t width : 图片的宽度
                 uint32_t height : 图片的高度
                 uint32_t stride_y : y分量跨度
                 uint32_t stride_u : u分量跨度
                 uint32_t stride_v : v分量跨度
                 uint32_t image_type : 图片类型 0 I420， 1 NV12，2 NV21
                 const char as8FileName[] : 文件名
         *输出  : 无
         *返回值: 成功 0 失败 -1
         *注释  :
         ***************************************************/
        int IUtilImageToFile(uint8_t *y, uint8_t *u, uint8_t *v,
                             uint32_t width, uint32_t height,
                             uint32_t stride_y, uint32_t stride_u, uint32_t stride_v, uint32_t image_type,
                             const char as8FileName[]);

        /**************************************************
         *函数名: IUtilMemToFile2plane
         *功能  : 图片到文件
         *输入  : char *filename : 文件名
                 char *y : y分量指针
                 char *uv ：uv分量指针
                 unsigned int y_size : y分量长度
                 unsigned int uv_size : uv分量长度
         *输出  : 无
         *返回值: 成功 0 失败 -1
         *注释  :
         ***************************************************/
        int IUtilMemToFile2plane(char *filename, char *y, char *uv, unsigned int y_size, unsigned int uv_size);

        /**************************************************
         *函数名: IntelliUtilCreateCritiSec
         *功能  : 创建mutex
         *输入  : 无
         *输出  : 无
         *返回值: 成功 void * 失败 NULL
         *注释  :
         ***************************************************/
        void *IntelliUtilCreateCritiSec();

        /**************************************************
         *函数名: IntelliUtilCloseCritiSec
         *功能  : 关闭mutex
         *输入  : void *pvCriti : 互斥量指针
         *输出  : 无
         *返回值: 无
         *注释  :
         ***************************************************/
        void IntelliUtilCloseCritiSec(void *pvCriti);

        /**************************************************
         *函数名: IntelliUtilLockCritiSec
         *功能  : 锁mutex
         *输入  : void *pvCriti : 互斥量指针
         *输出  : 无
         *返回值: 无意义
         *注释  :
         ***************************************************/
        int IntelliUtilLockCritiSec(void *pvCriti);

        /**************************************************
         *函数名: IntelliUtilUnlockCritiSec
         *功能  : 解锁mutex
         *输入  : void *pvCriti : 互斥量指针
         *输出  : 无
         *返回值: 无意义
         *注释  :
         ***************************************************/
        int IntelliUtilUnlockCritiSec(void *pvCriti);

        /**************************************************
         *函数名: IUtilCondCreate
         *功能  : 创建条件变量
         *输入  : 无
         *输出  : 无
         *返回值: 成功 void * 失败 NULL
         *注释  :
         ***************************************************/
        void *IUtilCondCreate();

        /**************************************************
         *函数名: IUtilCondClose
         *功能  : 关闭条件变量
         *输入  : void *pvCond : 条件变量指针
         *输出  : 无
         *返回值: 成功 0 失败 -1
         *注释  :
         ***************************************************/
        int IUtilCondClose(void *pvCond);

        /**************************************************
         *函数名: IUtilCondWait
         *功能  : 等条件变量
         *输入  : void *pvCond : 条件变量指针
                 void *pvMutex : 互斥量指针
         *输出  : 无
         *返回值: 成功 0 失败 -1
         *注释  :
         ***************************************************/
        int IUtilCondWait(void *pvCond, void *pvMutex);

        /**************************************************
         *函数名: IUtilCondSignal
         *功能  : 发信号
         *输入  : void *pvCond : 条件变量指针
         *输出  : 无
         *返回值: 成功 0 失败 -1
         *注释  :
         ***************************************************/
        int IUtilCondSignal(void *pvCond);

        /**************************************************
         *函数名: IntelliUtilMalloc
         *功能  : 内存申请
         *输入  : uint32_t u32Size : 申请大小
         *输出  : 无
         *返回值: 成功 void * 失败 NULL
         *注释  :
         ***************************************************/
        void *IntelliUtilMalloc(uint32_t u32Size);

        /**************************************************
         *函数名: IntelliUtilFree
         *功能  : 内存释放
         *输入  : void *pvPtr : 内存指针
         *输出  : 无
         *返回值: 无
         *注释  :
         ***************************************************/
        void IntelliUtilFree(void *pvPtr);

        /**************************************************
         *函数名: IntelliUtilMkDir
         *功能  : 创建目录
         *输入  : char *path : 目录路径
         *输出  : 无
         *返回值: 无
         *注释  :
         ***************************************************/
        int IntelliUtilMkDir(char *path);

        // Allocates memory of `size` bytes aligned on an `alignment` boundry.
        // The return value is a pointer to the memory. Note that the memory must
        // be de-allocated using AlignedFree.
        void *IUtilAlignedMalloc(size_t size, size_t alignment);

        // De-allocates memory created using the AlignedMalloc() API.
        void IUtilAlignedFree(void *mem_block);

        void IUtilOspPrintfS(char *ps8String);

        void IUtilOspPrintf(const char *szFormat, ...);

#ifdef __cplusplus
}
#endif

#endif
