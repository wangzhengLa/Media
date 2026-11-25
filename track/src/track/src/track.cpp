#include "config.h"
#include "logger.h"
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

// 获取当前时间戳（毫秒，单调时钟）
static long long now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ========== 子进程：实际监控逻辑 ==========
static int child_main(TrackConfig& cfg) {
    // 子进程：设置进程名便于 ps 区分
    prctl(PR_SET_NAME, "track-child", 0, 0, 0);
    Logger::log(LogLevel::INFO, "track child start");

    // 打印监控的进程列表
    for (const auto& p : cfg.processes) {
        if (p.pidfile.empty()) {
            Logger::log(LogLevel::INFO, "watch: name=%s comm=%s (no pidfile)",
                        p.name.c_str(), p.comm.c_str());
        } else {
            Logger::log(LogLevel::INFO, "watch: name=%s pidfile=%s comm=%s",
                        p.name.c_str(), p.pidfile.c_str(), p.comm.c_str());
        }
    }

    long long start_ms = now_ms();
    long long last_heartbeat_ms = 0;
    long long grace_until = start_ms + (long long)cfg.startup_grace * 1000;

    if (cfg.startup_grace > 0) {
        Logger::log(LogLevel::INFO, "startup grace: %ds (no restart during this period)",
                    cfg.startup_grace);
    }

    while (!g_stop) {
        long long now = now_ms();
        bool in_grace = (now < grace_until);

        // ---- 1. 进程存活检测 ----
        std::string heartbeat_str;

        for (auto& proc : cfg.processes) {
            int pid = 0;
            ProcessState st = check_process(proc, pid);

            if (st == ProcessState::ALIVE) {
                proc.last_pid = pid;
                // 进程稳定运行超过 rapid_death_window，重置崩溃计数
                reset_crash_count_if_stable(proc, cfg);
                char tmp[128];
                snprintf(tmp, sizeof(tmp), "%s=alive(pid=%d) ", proc.name.c_str(), pid);
                heartbeat_str += tmp;
            } else {
                char tmp[128];
                snprintf(tmp, sizeof(tmp), "%s=dead ", proc.name.c_str());
                heartbeat_str += tmp;

                // 启动宽限期内不拉起，只记录
                if (in_grace) {
                    continue;
                }

                // 拉起（连续崩溃达阈值会在 restart_process 内触发系统重启）
                restart_process(proc, cfg);
            }
        }

        // ---- 2. 心跳日志 ----
        if (cfg.heartbeat_interval > 0) {
            if (now - last_heartbeat_ms >= (long long)cfg.heartbeat_interval * 1000) {
                double uptime = (now - start_ms) / 1000.0;
                Logger::log(LogLevel::INFO, "heartbeat: %suptime=%.0fs",
                            heartbeat_str.c_str(), uptime);
                last_heartbeat_ms = now;
            }
        } else {
            // 每轮都打
            double uptime = (now - start_ms) / 1000.0;
            Logger::log(LogLevel::INFO, "heartbeat: %suptime=%.0fs",
                        heartbeat_str.c_str(), uptime);
            last_heartbeat_ms = now;
        }

        // ---- 3. 资源监控（非阻塞，内部按时间节拍采样）----
        cpu_monitor_tick(cfg);
        mem_monitor_tick(cfg);
        flash_monitor_tick(cfg);

        // ---- 4. 内核日志采集（读 /proc/kmsg，增量写入 dmesg.log）----
        dmesg_monitor_tick(cfg);

        // ---- 5. sleep ----
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

    Logger::log(LogLevel::INFO, "track start: log_size=%dKB log_count=%d",
                cfg.log_size_kb, cfg.log_count);

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
            Logger::log(LogLevel::WARN, "child exited, code=%d, respawning", WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            Logger::log(LogLevel::WARN, "child killed by signal %d, respawning", WTERMSIG(status));
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
