#include "process_monitor.h"
#include "logger.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>

// 读取 /proc/<pid>/comm，返回进程名（最多 15 字符）
static std::string read_comm(int pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    FILE* f = fopen(path, "r");
    if (!f) return "";
    char buf[32];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return ""; }
    fclose(f);
    // 去掉换行
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) {
        buf[--len] = '\0';
    }
    return std::string(buf);
}

int read_pidfile(const std::string& path) {
    if (path.empty()) return 0;
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return 0;
    int pid = 0;
    if (fscanf(f, "%d", &pid) != 1) pid = 0;
    fclose(f);
    return pid;
}

int find_pid_by_comm(const std::string& comm) {
    DIR* dir = opendir("/proc");
    if (!dir) return 0;
    struct dirent* ent;
    int found = 0;
    while ((ent = readdir(dir)) != nullptr) {
        // 只处理数字目录（pid）
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9') continue;
        int pid = atoi(ent->d_name);
        if (pid <= 0) continue;
        std::string c = read_comm(pid);
        if (c == comm) {
            found = pid;
            break;
        }
    }
    closedir(dir);
    return found;
}

ProcessState check_process(const ProcessConfig& proc, int& out_pid) {
    out_pid = 0;

    // 模式 A：有 pidfile
    if (!proc.pidfile.empty()) {
        int pid = read_pidfile(proc.pidfile);
        if (pid > 0) {
            // 先用 kill(pid, 0) 快速判断进程是否存在
            if (kill(pid, 0) == 0) {
                // 进程存在，再校验进程名是否匹配（防止 PID 复用）
                std::string c = read_comm(pid);
                if (c == proc.comm) {
                    out_pid = pid;
                    return ProcessState::ALIVE;
                }
            }
        }
        // pidfile 里的 pid 不对，兜底遍历 /proc
        int found = find_pid_by_comm(proc.comm);
        if (found > 0) {
            out_pid = found;
            return ProcessState::ALIVE;
        }
        return ProcessState::DEAD;
    }

    // 模式 B：无 pidfile，直接遍历 /proc
    int found = find_pid_by_comm(proc.comm);
    if (found > 0) {
        out_pid = found;
        return ProcessState::ALIVE;
    }
    return ProcessState::DEAD;
}

// 获取当前时间戳（毫秒）
static long long now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int restart_process(ProcessConfig& proc, const TrackConfig& cfg) {
    // 每次重启都计数（不管之前是否"成功"拉起）
    proc.crash_count++;
    proc.last_death_ms = now_ms();

    Logger::log(LogLevel::ERROR, "process '%s' died: last_pid=%d crash_count=%d",
                proc.name.c_str(), proc.last_pid, proc.crash_count);

    // 连续崩溃达阈值，重启系统
    if (proc.crash_count >= cfg.circuit_break_threshold) {
        Logger::log(LogLevel::ERROR,
                    "'%s' crashed %d times consecutively, rebooting system...",
                    proc.name.c_str(), proc.crash_count);
        sync();
        sleep(1);
        system("reboot");
        return 0;
    }

    // 固定延迟，不做指数退避
    int delay = proc.restart_delay;

    Logger::log(LogLevel::INFO, "restarting '%s' (attempt %d, delay=%ds)",
                proc.name.c_str(), proc.crash_count, delay);

    // 执行拉起命令
    int ret = system(proc.restart_cmd.c_str());
    (void)ret;

    // 等待进程启动
    sleep(delay);

    // 验证是否拉起成功
    int new_pid = 0;
    ProcessState st = check_process(proc, new_pid);
    if (st == ProcessState::ALIVE) {
        long long now = now_ms();
        double downtime = (now - proc.last_death_ms) / 1000.0;
        proc.last_pid = new_pid;
        proc.last_restart_ms = now;
        Logger::log(LogLevel::INFO, "'%s' restarted: pid=%d downtime=%.1fs",
                    proc.name.c_str(), new_pid, downtime);
        return new_pid;
    }

    // 拉起失败（进程没起来）
    Logger::log(LogLevel::ERROR, "'%s' restart failed (crash_count=%d)",
                proc.name.c_str(), proc.crash_count);
    return 0;
}

// 进程稳定运行超过 rapid_death_window 时，重置崩溃计数
void reset_crash_count_if_stable(ProcessConfig& proc, const TrackConfig& cfg) {
    if (proc.crash_count == 0 || proc.last_restart_ms == 0) return;
    long long now = now_ms();
    if (now - proc.last_restart_ms >= (long long)cfg.rapid_death_window * 1000) {
        proc.crash_count = 0;
        Logger::log(LogLevel::INFO, "'%s' stable for %ds, crash_count reset",
                    proc.name.c_str(), cfg.rapid_death_window);
    }
}
