#include <iostream>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "sysdbg_boost.h"
#include <boost/stacktrace.hpp>
#include <boost/lexical_cast.hpp>

// 正常栈回溯测试（不崩溃，验证编译选项和 Boost 是否正常）
static void test_normal_stacktrace()
{
    std::cout << "===== 正常栈回溯测试 =====" << std::endl;
    std::string stack_str = boost::lexical_cast<std::string>(
        boost::stacktrace::stacktrace(0, 32));
    std::cout << stack_str << std::endl;
    std::cout << "==========================" << std::endl;
}

// 空指针崩溃：SIGSEGV
static void crash_func_c()
{
    volatile int *p = nullptr;
    *p = 123;
}
static void crash_func_b() { crash_func_c(); }
static void crash_func_a() { crash_func_b(); }

// 除零崩溃：SIGFPE
static void crash_divide_by_zero()
{
    volatile int a = 1;
    volatile int b = 0;
    volatile int c = a / b;
    (void)c;
}

// noinline 防止内联；递归后加语句防止尾调用优化
__attribute__((noinline))
static void crash_stack_overflow()
{
    volatile char buf[1024];   // volatile 防止 buf 被优化掉
    buf[0] = 0;
    crash_stack_overflow();
    buf[0] = 1;                // ← 递归后的语句，编译器无法做 TCO
}

// 非法指令崩溃：SIGILL
// 使用 __builtin_trap()，GCC/Clang 会生成平台相关的非法指令
//   x86:  ud2 指令 (0F 0B)
//   ARM:  udf 指令
static void crash_sigill()
{
    __builtin_trap();
}

// 总线错误崩溃：SIGBUS（mmap 文件截断法，跨平台可靠）
// 原理：mmap 映射一个文件后将文件截断，访问映射区域时
//       虚拟地址合法但没有对应的物理页 → SIGBUS
static void crash_sigbus_mmap()
{
    const char *tmpfile = "/tmp/sigbus_test_file";
    // 创建一个 4096 字节的文件

    int fd = open(tmpfile, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        std::cerr << "[ERROR] 无法创建测试文件: " << tmpfile << std::endl;
        return;
    }
    ftruncate(fd, 4096);

    // mmap 映射整个文件
    volatile int *p = (volatile int *)mmap(
        nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    
        if (p == MAP_FAILED)
    {
        std::cerr << "[ERROR] mmap 失败" << std::endl;
        close(fd);
        return;
    }

    // 先写一下确保映射生效
    *p = 1;
    // 截断文件为 0，使所有映射页失效
    ftruncate(fd, 0);
    // 访问已失效的映射区域 → SIGBUS
    *p = 42;

    // 不会执行到这里
    munmap((void *)p, 4096);
    close(fd);
}


int main(int argc, char **argv)
{
    // 注册崩溃栈捕获（必须最开头调用）
    register_crash_handler();

    std::cout << "========================================" << std::endl;
    std::cout << "  UMP 崩溃捕获测试程序" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "用法:" << std::endl;
    std::cout << "  " << argv[0] << " null    - 空指针崩溃 (SIGSEGV)" << std::endl;
    std::cout << "  " << argv[0] << " div     - 除零崩溃 (SIGFPE)" << std::endl;
    std::cout << "  " << argv[0] << " stack   - 栈溢出崩溃 (SIGSEGV)" << std::endl;
    std::cout << "  " << argv[0] << " abort   - 主动终止 (SIGABRT)" << std::endl;
    std::cout << "  " << argv[0] << " ill     - 非法指令崩溃 (SIGILL)" << std::endl;
    std::cout << "  " << argv[0] << " bus     - 总线错误崩溃 mmap法 (SIGBUS)" << std::endl;
    std::cout << "  " << argv[0] << " normal  - 正常栈回溯测试（不崩溃）" << std::endl;
    std::cout << "========================================" << std::endl;

    if (argc > 1 && strcmp(argv[1], "normal") == 0)
    {
        test_normal_stacktrace();
        return 0;
    }

    if (argc < 2)
    {
        std::cout << "[INFO] 未指定崩溃类型，默认 3 秒后触发空指针崩溃..." << std::endl;
        sleep(3);
        crash_func_a();
        return 0;
    }

    std::cout << "[TEST] 3 秒后触发崩溃，请稍候..." << std::endl;
    sleep(3);

    if (strcmp(argv[1], "null") == 0)
    {
        std::cout << "[TEST] 触发空指针崩溃..." << std::endl;
        crash_func_a();
    }
    else if (strcmp(argv[1], "div") == 0)
    {
        std::cout << "[TEST] 触发除零崩溃..." << std::endl;
        crash_divide_by_zero();
    }
    else if (strcmp(argv[1], "stack") == 0)
    {
        std::cout << "[TEST] 触发栈溢出崩溃（递归约 8000 次，需几秒）..." << std::endl;
        crash_stack_overflow();
    }
    else if (strcmp(argv[1], "abort") == 0)
    {
        std::cout << "[TEST] 触发 abort() (SIGABRT)..." << std::endl;
        abort();
    }
    else if (strcmp(argv[1], "ill") == 0)
    {
        std::cout << "[TEST] 触发非法指令 (SIGILL)..." << std::endl;
        crash_sigill();
    }
    else if (strcmp(argv[1], "bus") == 0)
    {
        std::cout << "[TEST] 触发总线错误 mmap法 (SIGBUS)..." << std::endl;
        crash_sigbus_mmap();
    }
    else
    {
        std::cout << "[ERROR] 未知崩溃类型: " << argv[1] << std::endl;
        return 1;
    }

    std::cout << "[ERROR] 崩溃未触发！" << std::endl;
    return 0;
}