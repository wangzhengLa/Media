#ifndef TRACK_CONFIG_H
#define TRACK_CONFIG_H

#include <string>
#include <vector>

// 被监控进程的配置
struct ProcessConfig {
    std::string name;           // 进程标识名（日志用）
    std::string comm;           // /proc/<pid>/comm 匹配的进程名
    std::string pidfile;        // pidfile 路径（可选，空则按 comm 检测）
    std::string restart_cmd;    // 拉起命令
    int         restart_delay;  // 拉起后等待秒数
    // 运行时状态
    int         last_pid;       // 上次检测到的 pid
    int         crash_count;    // 连续快速崩溃计数
    long long   last_restart_ms; // 上次拉起时间（ms）
    long long   last_death_ms;  // 上次死亡时间（ms）
};

// 全局配置
struct TrackConfig {
    std::string log_dir;            // 日志目录
    int         log_size_kb;        // 单个日志文件大小（KB）
    int         log_count;          // 日志保留份数
    int         check_interval;     // 存活检测间隔（秒）
    int         rapid_death_window; // 快速崩溃判定窗口（秒），进程拉起后在此时间内死亡算快速崩溃
    int         circuit_break_threshold; // 连续快速崩溃多少次后重启系统
    int         snapshot_cooldown;  // 告警期间快照重打间隔（秒），cpu/mem/flash 共用
    int         snapshot_top_n;     // 快照取前 N 项，cpu/mem/flash 共用
    // ---------- CPU 监控 ----------
    int         cpu_total_threshold;    // 整机归一化% 告警阈值（0~100）
    // ---------- 内存监控 ----------
    int         mem_available_threshold_mb;  // MemAvailable 低于此值(MB)告警（绝对阈值）
    // ---------- Flash 监控 ----------
    std::string flash_paths;                 // 监控路径（空格分隔）；同 f_fsid 去重，只读分区跳过告警
    int         flash_used_pct_threshold;    // 已用百分比告警阈值（0~100）
    std::vector<ProcessConfig> processes;
};

// 解析配置文件，返回是否成功
bool parse_config(const std::string& path, TrackConfig& cfg);

#endif // TRACK_CONFIG_H
