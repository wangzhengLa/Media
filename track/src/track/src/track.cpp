#include "config.h"
#include "logger.h"
#include "monitor_common.h"
#include "process_monitor.h"
#include "cpu_monitor.h"
#include "mem_monitor.h"
#include "flash_monitor.h"
#include "dmesg_monitor.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <time.h>

static const char* CONFIG_PATH = "/etc/track/track.conf";
static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

// ========== 子进程：实际监控逻辑 ==========
static int child_main(TrackConfig& cfg) {
    // 子进程：设置进程名便于 ps 区分
    prctl(PR_SET_NAME, "track-child", 0, 0, 0);
    // fork 后重新打开自己的日志 fd：与父进程彻底分离，避免继承同一 fd 表项后
    // 一方轮转导致另一方的 fd 仍指向旧 inode（写入落到 track.log.1）。
    // open() 幂等，会先关掉继承来的旧 fd。
    Logger::init(cfg.log_dir, cfg.log_size_kb, cfg.log_count);
    Logger::log(LogLevel::INFO, "track child start");

    while (!g_stop) {
        // ---- 1. 进程存活检测：死亡即拉起（无启动宽限期）----
        for (auto& proc : cfg.processes) {
            int pid = 0;
            ProcessState st = check_process(proc, pid);

            if (st == ProcessState::ALIVE) {
                proc.last_pid = pid;
                // 进程稳定运行超过 rapid_death_window，重置崩溃计数
                reset_crash_count_if_stable(proc, cfg);
            } else {
                // 拉起（连续崩溃达阈值会在 restart_process 内触发系统重启）
                restart_process(proc, cfg);
            }
        }

        // ---- 2. 资源监控（非阻塞，内部按时间节拍采样）----
        cpu_monitor_tick(cfg);
        mem_monitor_tick(cfg);
        flash_monitor_tick(cfg);

        // ---- 3. 内核日志采集（读 /proc/kmsg，增量写入 dmesg.log）----
        dmesg_monitor_tick(cfg);

        // ---- 4. sleep ----
        sleep(cfg.check_interval);
    }

    Logger::log(LogLevel::INFO, "track child stopping");
    dmesg_monitor_close();
    Logger::close();
    return 0;
}

// ========== 父进程：在 main 中直接处理 fork+waitpid+重启 ==========
int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    // 解析配置
    TrackConfig cfg;
    if (!parse_config(CONFIG_PATH, cfg)) {
        fprintf(stderr, "track: cannot parse config %s\n", CONFIG_PATH);
        return 1;
    }

    // 初始化日志
    if (!Logger::init(cfg.log_dir, cfg.log_size_kb, cfg.log_count)) {
        fprintf(stderr, "track: cannot init log dir %s\n", cfg.log_dir.c_str());
        return 1;
    }

    Logger::log(LogLevel::INFO, "track start");

    // 注册信号（优雅停止）
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);


    // 创建日志目录已在 Logger::init 中处理

    // 第一次 fork：父进程监控，子进程干活
    pid_t child_pid = fork();
    if (child_pid < 0) {
        Logger::log(LogLevel::ERROR, "fork failed");
        return 1;
    }

    if (child_pid == 0) {
        // 子进程
        return child_main(cfg);
    }

    // 父进程：设置进程名便于 ps 区分
    prctl(PR_SET_NAME, "track-parent", 0, 0, 0);
    while (!g_stop) {
        int status;
        pid_t ret = waitpid(child_pid, &status, 0);
        if (ret < 0) {
            if (g_stop) break;
            continue;
        }

        // 子进程退出，记录原因
        if (WIFEXITED(status)) {
            Logger::log(LogLevel::ERROR, "child exited, code=%d, respawning", WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            Logger::log(LogLevel::ERROR, "child killed by signal %d, respawning", WTERMSIG(status));
        }

        // 重新 fork 子进程
        child_pid = fork();
        if (child_pid < 0) {
            Logger::log(LogLevel::ERROR, "fork failed, retry in 1s");
            sleep(1);
            continue;
        }
        if (child_pid == 0) {
            return child_main(cfg);
        }
    }

    Logger::log(LogLevel::INFO, "track parent stopping");
    Logger::close();
    return 0;
}
