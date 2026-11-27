// 内存监控模块：纯读 /proc/meminfo，不阻塞主循环。

#include "mem_monitor.h"
#include "logger.h"
#include "monitor_common.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <unistd.h>
#include <dirent.h>
#include <time.h>

namespace {

// ---------- 采集：/proc/meminfo ----------
// 解析 MemTotal / MemAvailable（单位 kB）。
bool read_meminfo(long& total_kb, long& avail_kb) {
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return false;
    total_kb = avail_kb = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64] = {0};
        long val = 0;
        // 形如 "MemTotal:       492528 kB"
        if (sscanf(line, "%63[^:]: %ld", key, &val) != 2) continue;
        if (strcmp(key, "MemTotal") == 0)       total_kb = val;
        else if (strcmp(key, "MemAvailable") == 0) avail_kb = val;
    }
    fclose(f);
    return total_kb > 0 && avail_kb > 0;
}

// ---------- 状态（模块私有，收拢所有 file-scope 变量）----------
struct State {
    bool inited = false;
    long long start_ms = 0;
    long long last_snapshot_ms = 0;
    long page_kb = 4;

    // 最近一次采样值（供日志/快照使用）
    long total_kb = 0, avail_kb = 0;

    // 告警状态（MemAvailable 低于阈值）
    AlertState alert;
    double min_avail_mb = -1.0;   // 告警期间 MemAvailable 最小值（MB），<0 表示未在告警
};
State state;

// 启动 / 告警共用的单行内存摘要：avail / thr / total / used% 四个字段。
void log_mem_info(LogLevel lvl, const TrackConfig& cfg, double avail_mb) {
    double total_mb = state.total_kb / 1024.0;
    double used_pct = (total_mb > 0) ? (100.0 * (total_mb - avail_mb) / total_mb) : 0.0;
    Logger::log(lvl, "mem: avail=%.1fMB thr=%dMB total=%.1fMB used=%.1f%%",
                avail_mb, cfg.mem_available_threshold_mb, total_mb, used_pct);
}

// ---------- 拍 TOP N 内存快照（写入主日志 track.log）----------
// 告警时扫描 /proc 一次：RSS 是瞬时值，无需 baseline，故只在快照时扫描，稳态零额外开销。
void take_snapshot(const TrackConfig& cfg) {
    long long now = now_ms();
    if (now - state.last_snapshot_ms < (long long)cfg.snapshot_cooldown * 1000) return;
    state.last_snapshot_ms = now;

    struct Top {
        int pid; char comm[64]; char state;
        long rss_kb; int threads;
    };
    std::vector<Top> tops;

    DIR* d = opendir("/proc");
    if (!d) return;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        int pid = atoi(de->d_name);
        if (pid <= 0) continue;
        char comm[64]; uint64_t u = 0, s = 0; long rss = 0; int nt = 0; char st = 0;
        if (!read_pid_stat(pid, comm, sizeof(comm), u, s, &nt, &rss, &st)) continue;
        Top t;
        t.pid = pid;
        t.rss_kb = rss * state.page_kb;
        t.threads = nt;
        t.state = st;
        strncpy(t.comm, comm, sizeof(t.comm) - 1);
        t.comm[sizeof(t.comm) - 1] = '\0';
        tops.push_back(t);
    }
    closedir(d);

    std::sort(tops.begin(), tops.end(), [](const Top& a, const Top& b){ return a.rss_kb > b.rss_kb; });
    int n = clip_top_n((int)tops.size(), cfg.snapshot_top_n);

    double avail_mb = state.avail_kb / 1024.0;
    double total_mb = state.total_kb / 1024.0;
    double used_pct = (total_mb > 0) ? (100.0 * (total_mb - avail_mb) / total_mb) : 0.0;
    Logger::log(LogLevel::WARN,
        "mem: SNAPSHOT uptime=%llds avail=%.1fMB total=%.1fMB used=%.1f%% thr=%dMB top=%d",
        (now - state.start_ms) / 1000, avail_mb, total_mb, used_pct,
        cfg.mem_available_threshold_mb, n);
    Logger::log(LogLevel::WARN, "mem: SNAP rank pid    comm             st rssMB    threads");
    for (int i = 0; i < n; i++) {
        const Top& t = tops[i];
        Logger::log(LogLevel::WARN, "mem: SNAP %-4d %-6d %-16s %-2c %8.1f %7d",
                    i + 1, t.pid, t.comm, t.state ? t.state : '-',
                    t.rss_kb / 1024.0, t.threads);
    }
}

// ---------- 每 tick 采样 + 告警评估 ----------
void sample(const TrackConfig& cfg) {
    long total = 0, avail = 0;
    if (read_meminfo(total, avail)) {
        state.total_kb = total; state.avail_kb = avail;
    }
    double avail_mb = state.avail_kb / 1024.0;
    bool over = (avail_mb < (double)cfg.mem_available_threshold_mb);
    int ch = eval_alert(state.alert, over, avail_mb);

    // 跟踪告警期间 MemAvailable 最小值
    if (state.alert.active) {
        if (state.min_avail_mb < 0 || avail_mb < state.min_avail_mb) state.min_avail_mb = avail_mb;
    }

    if (ch & 1) {
        // 刚触发告警：第1行提示 → 第2行单行摘要；快照由下方 active 块统一拍
        Logger::log(LogLevel::WARN, "mem: memory usage has exceeded the threshold");
        log_mem_info(LogLevel::WARN, cfg, avail_mb);
    } else if (ch & 2) {
        Logger::log(LogLevel::INFO,
            "mem: RECOVERED avail=%.1fMB thr=%dMB min=%.1fMB",
            avail_mb, cfg.mem_available_threshold_mb,
            state.min_avail_mb >= 0 ? state.min_avail_mb : avail_mb);
        state.min_avail_mb = -1.0;
    }

    // 告警持续期间（含刚触发）按冷却拍快照；恢复后 state.alert.active 为假即停
    if (state.alert.active) {
        take_snapshot(cfg);
    }
}

} // namespace

void mem_monitor_tick(const TrackConfig& cfg) {
    long long now = now_ms();
    if (!state.inited) {
        state.inited = true;
        state.start_ms = now;
        long ps = sysconf(_SC_PAGESIZE);
        state.page_kb = (ps > 0) ? (ps / 1024) : 4;
        // 首轮读一次并打启动行；内存为瞬时值，无需 baseline，下一轮即可告警
        long total = 0, avail = 0;
        if (read_meminfo(total, avail)) {
            state.total_kb = total; state.avail_kb = avail;
        }
        log_mem_info(LogLevel::INFO, cfg, state.avail_kb / 1024.0);
        return;
    }
    sample(cfg);
}
