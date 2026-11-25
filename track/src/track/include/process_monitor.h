#ifndef TRACK_PROCESS_MONITOR_H
#define TRACK_PROCESS_MONITOR_H

#include "config.h"
#include <string>

// 进程存活检测结果
enum class ProcessState {
    ALIVE,
    DEAD
};

// 检测单个进程是否存活，返回状态和当前 pid
ProcessState check_process(const ProcessConfig& proc, int& out_pid);

// 拉起进程：执行 restart_cmd，等待 restart_delay，返回新 pid（0=拉起失败）
int restart_process(ProcessConfig& proc, const TrackConfig& cfg);

// 进程稳定运行超过 rapid_death_window 时重置崩溃计数
void reset_crash_count_if_stable(ProcessConfig& proc, const TrackConfig& cfg);

// 根据 comm 遍历 /proc 查找进程 pid，返回 0 表示没找到
int find_pid_by_comm(const std::string& comm);

// 读 pidfile 获取 pid，返回 0 表示失败
int read_pidfile(const std::string& path);

#endif // TRACK_PROCESS_MONITOR_H
