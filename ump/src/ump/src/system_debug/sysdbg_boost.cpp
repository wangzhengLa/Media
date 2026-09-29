#include "sysdbg_boost.h"

#include <iostream>
#include <fstream>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <ctime>
#include <string>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <boost/stacktrace.hpp>
#include <boost/lexical_cast.hpp>

// 配置：堆栈日志保存目录（可根据需求修改）
const std::string CRASH_LOG_DIR = "/var/log/app_crash/";

// 获取当前进程名（修复长度限制，兼容长短名称）
static std::string get_process_name()
{
    // 方案1：优先从 /proc/self/cmdline 获取完整命令行（无长度限制）
    std::ifstream cmdline_file("/proc/self/cmdline");
    std::string cmdline;
    if (cmdline_file && std::getline(cmdline_file, cmdline, '\0')) // cmdline 以 \0 分隔参数
    {
        cmdline_file.close();
        // 提取纯进程名（去掉路径，只保留最后一个 / 后的部分）
        size_t last_slash = cmdline.find_last_of('/');
        if (last_slash != std::string::npos)
        {
            return cmdline.substr(last_slash + 1);
        }
        return cmdline; // 无路径则直接返回
    }

    // 方案2：降级到 /proc/self/comm（兼容无 cmdline 的场景，接受 15 字符限制）
    std::ifstream comm_file("/proc/self/comm");
    std::string name;
    if (comm_file && std::getline(comm_file, name))
    {
        comm_file.close();
        // 去除换行符/回车符（若有）
        name.erase(name.find_last_not_of("\n\r") + 1);
        return name;
    }

    // 方案3：最终兜底
    return "unknown_process";
}

// 生成带日期时间的文件名（格式：进程名_YYYYMMDD_HHMMSS.crash），增加降级逻辑
static std::string generate_crash_filename()
{
    // 1. 获取进程名
    std::string proc_name = get_process_name();

    // 2. 获取当前时间并格式化（线程安全）
    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    char time_buf[32] = {0};
    strftime(time_buf, sizeof(time_buf), "%Y%m%d_%H%M%S", &tm_now);

    // 3. 拼接文件名，增加降级到/tmp的逻辑
    std::string log_file = CRASH_LOG_DIR + proc_name + "_" + time_buf + ".crash";

    // 检查配置目录是否可写，不可写则用/tmp
    struct stat st;
    if (stat(CRASH_LOG_DIR.c_str(), &st) != 0 || !(st.st_mode & S_IWUSR))
    {
        log_file = "/tmp/" + proc_name + "_" + time_buf + ".crash";
    }

    return log_file;
}

// 创建目录（支持递归创建多级目录）
static void create_dir_if_not_exists(const std::string &dir)
{
    std::string temp_dir;
    for (size_t i = 0; i < dir.size(); ++i)
    {
        temp_dir += dir[i];
        // 遇到目录分隔符且不是最后一个字符时，创建当前层级
        if (dir[i] == '/' && i != dir.size() - 1)
        {
            struct stat st;
            if (stat(temp_dir.c_str(), &st) != 0)
            {
                mkdir(temp_dir.c_str(), 0755);
            }
        }
    }
    // 确保最终目录存在（兜底）
    struct stat st;
    if (stat(dir.c_str(), &st) != 0)
    {
        mkdir(dir.c_str(), 0755);
    }
}

// 写入崩溃堆栈到文件（优化为C标准IO，信号处理更安全）
static void write_crash_log(int sig)
{
    // 1. 确保日志目录存在
    create_dir_if_not_exists(CRASH_LOG_DIR);

    // 2. 生成日志文件名
    std::string log_file = generate_crash_filename();

    // 3. 用C标准IO打开文件（信号处理函数中更安全）
    FILE *fp = fopen(log_file.c_str(), "w");
    if (fp != nullptr)
    {
        // 写入崩溃基本信息
        fprintf(fp, "===== CRASH REPORT =====\n");
        fprintf(fp, "Signal: %d (%s)\n", sig, strsignal(sig) ? strsignal(sig) : "unknown signal");

        // 写入崩溃时间（线程安全）
        time_t now = time(nullptr);
        char time_str[64] = {0};
        ctime_r(&now, time_str);
        fprintf(fp, "Crash time: %s", time_str);

        // 直接用 boost::stacktrace 获取当前调用栈（比混合 backtrace+frame 更可靠）
        boost::stacktrace::stacktrace st = boost::stacktrace::stacktrace();
        fprintf(fp, "Stack trace:\n%s\n", boost::stacktrace::to_string(st).c_str());

        fclose(fp);
        // 输出到stderr（方便终端调试）
        fprintf(stderr, "[CRASH] Log saved to: %s\n", log_file.c_str());
    }
    else
    {
        fprintf(stderr, "[CRASH] Failed to open log file: %s\n", log_file.c_str());
    }
}

// 崩溃信号处理函数：先写日志，再重置信号并重发
static void signal_handler(int sig)
{
    
    // 步骤1：保存堆栈日志到文件（核心需求）
    write_crash_log(sig);

    // 步骤2：重置信号处理为系统默认（关键：恢复默认行为）
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = SIG_DFL;
    if (sigaction(sig, &sa, NULL) == -1)
    {
        fprintf(stderr, "[CRASH] Failed to reset signal %d handler\n", sig);
    }

    // 步骤3：重新发送该信号，触发系统默认行为（如生成core dump）
    kill(getpid(), sig);
}

// 注册崩溃信号（捕获常见崩溃类型，增加注册失败日志）
void register_crash_handler()
{
    // ===== 关键修复 1：设置备用信号栈 =====
    // 栈溢出时当前栈已不可用，信号处理器必须在独立栈上运行
    stack_t alt_stack;
    alt_stack.ss_sp = malloc(SIGSTKSZ);   // SIGSTKSZ 通常 8192 字节，足够写日志
    alt_stack.ss_size = SIGSTKSZ;
    alt_stack.ss_flags = 0;
    if (alt_stack.ss_sp == nullptr)
    {
        fprintf(stderr, "[WARNING] Failed to allocate alternate signal stack\n");
    }
    else if (sigaltstack(&alt_stack, nullptr) == -1)
    {
        fprintf(stderr, "[WARNING] Failed to set alternate signal stack: %s\n", strerror(errno));
        free(alt_stack.ss_sp);
    }

    // ===== 关键修复 2：SA_ONSTACK 标志 =====
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    // 关键标志：
    // - SA_RESETHAND：信号处理后重置为默认（双重保障）
    // - SA_NODEFER：处理信号时不屏蔽该信号（重发时能触发）
    // - SA_ONSTACK：使用备用栈（避免栈溢出时无法处理）
    sa.sa_flags = SA_RESETHAND | SA_NODEFER | SA_ONSTACK;
    sa.sa_handler = signal_handler;

    // 封装信号注册逻辑，增加失败日志
    auto register_sig = [&](int sig)
    {
        if (sigaction(sig, &sa, NULL) == -1)
        {
            fprintf(stderr, "[WARNING] Failed to register handler for signal %d\n", sig);
        }
    };

    // 注册需要捕获的崩溃信号
    register_sig(SIGSEGV); // 段错误（访问空指针、越界）
    register_sig(SIGFPE);  // 浮点错误（除以0）
    register_sig(SIGILL);  // 非法指令
    register_sig(SIGBUS);  // 总线错误
    register_sig(SIGABRT); // abort()触发
}
