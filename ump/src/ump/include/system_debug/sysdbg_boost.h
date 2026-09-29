#ifndef SYSDBG_BOOST_H
#define SYSDBG_BOOST_H

/**
 * @file sysdbg_boost.h
 * @brief 崩溃堆栈捕获工具对外接口
 * @note 依赖 Boost 1.65.0 stacktrace 模块，仅支持 Linux/aarch64 环境
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 注册崩溃信号处理器
 * 
 * 功能说明：
 * 1. 捕获常见崩溃信号（SIGSEGV/SIGFPE/SIGILL/SIGBUS/SIGABRT）；
 * 2. 崩溃时自动将堆栈日志写入 /var/log/app_crash/（无权限则降级到 /tmp）；
 * 3. 保存日志后重置信号为系统默认行为（触发 core dump + 进程终止）；
 * 4. 日志文件命名格式：进程名_YYYYMMDD_HHMMSS.crash。
 * 
 * 调用时机：建议在程序启动初期（main 函数开头）调用，仅需调用一次。
 * 注意事项：
 * - 需确保目标设备有 /var/log/app_crash/ 或 /tmp 目录的写入权限；
 * - 编译时需链接 Boost stacktrace_basic 库和 libbacktrace 库；
 * - 程序需保留符号表（编译时加 -g），否则堆栈仅显示内存地址。
 */
void register_crash_handler();

#ifdef __cplusplus
}
#endif

#endif // SYSDBG_BOOST_H