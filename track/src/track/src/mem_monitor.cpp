// 内存监控模块：纯读 /proc/meminfo，不阻塞主循环。
// 口径：
//   - 告警指标：MemAvailable（绝对值，MB），低于 mem_available_threshold_mb 告警
//   - 周期概览：avail / total / used% / free / swap
//   - 告警快照：扫描 /proc 一次，按 RSS 降序取 TOP N（瞬时值，无需 baseline）
// 与 CPU 的差异：内存是瞬时值（非差分），不需要首轮建 baseline，首轮即可参与告警。
// 去抖：mem_alert_duration（默认 5，比 CPU 更抗抖动）。
// 观察式：仅日志 + 快照，不自动干预。

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

// ---------- 时间 ----------
long long now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ---------- 采集：/proc/meminfo ----------
// 解析 MemTotal / MemAvailable / MemFree / SwapTotal / SwapFree（单位 kB）。
bool read_meminfo(long& total_kb, long& avail_kb, long& free_kb,
                  long& swap_total_kb, long& swap_free_kb) {
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return false;
    total_kb = avail_kb = free_kb = swap_total_kb = swap_free_kb = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64] = {0};
        long val = 0;
        // 形如 "MemTotal:       492528 kB"
        if (sscanf(line, "%63[^:]: %ld", key, &val) != 2) continue;
        if (strcmp(key, "MemTotal") == 0)       total_kb = val;
        else if (strcmp(key, "MemAvailable") == 0) avail_kb = val;
        else if (strcmp(key, "MemFree") == 0)    free_kb = val;
        else if (strcmp(key, "SwapTotal") == 0)  swap_total_kb = val;
        else if (strcmp(key, "SwapFree") == 0)   swap_free_kb = val;
    }
    fclose(f);
    return total_kb > 0 && avail_kb > 0;
}

// ---------- 状态 ----------
bool g_inited = false;
long long g_start_ms = 0;
long long g_last_log_ms = 0;
long long g_last_snapshot_ms = 0;
long g_page_kb = 4;

// 最近一次采样值（供日志/快照使用）
long g_total_kb = 0, g_avail_kb = 0, g_free_kb = 0;
long g_swap_total_kb = 0, g_swap_free_kb = 0;

// 告警状态（MemAvailable 低于阈值）
AlertState st_avail;
double g_min_avail_mb = -1.0;   // 告警期间 MemAvailable 最小值（MB），<0 表示未在告警

bool any_alert_active() {
    return st_avail.active;
}

// ---------- 拍 TOP N 内存快照（写入主日志 track.log，不另建文件）----------
// 告警时扫描 /proc 一次：RSS 是瞬时值，无需 baseline，故只在快照时扫描，稳态零额外开销。
void take_snapshot(const TrackConfig& cfg) {
    if (!cfg.mem_snapshot_on_alert) return;
    long long now = now_ms();
    if (now - g_last_snapshot_ms < (long long)cfg.mem_snapshot_cooldown * 1000) return;
    g_last_snapshot_ms = now;

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
        t.rss_kb = rss * g_page_kb;
        t.threads = nt;
        t.state = st;
        strncpy(t.comm, comm, sizeof(t.comm) - 1);
        t.comm[sizeof(t.comm) - 1] = '\0';
        tops.push_back(t);
    }
    closedir(d);

    std::sort(tops.begin(), tops.end(), [](const Top& a, const Top& b){ return a.rss_kb > b.rss_kb; });
    int n = (int)tops.size();
    if (n > cfg.mem_top_n) n = cfg.mem_top_n;

    double avail_mb = g_avail_kb / 1024.0;
    double total_mb = g_total_kb / 1024.0;
    double used_pct = (total_mb > 0) ? (100.0 * (total_mb - avail_mb) / total_mb) : 0.0;
    Logger::log(LogLevel::WARN,
        "mem: SNAPSHOT uptime=%llds avail=%.1fMB total=%.1fMB used=%.1f%% thr=%dMB top=%d",
        (now - g_start_ms) / 1000, avail_mb, total_mb, used_pct,
        cfg.mem_available_threshold_mb, n);
    Logger::log(LogLevel::WARN, "mem: SNAP swap total=%.1fMB free=%.1fMB",
        g_swap_total_kb / 1024.0, g_swap_free_kb / 1024.0);
    Logger::log(LogLevel::WARN, "mem: SNAP rank pid    comm             st rssMB    threads");
    for (int i = 0; i < n; i++) {
        const Top& t = tops[i];
        Logger::log(LogLevel::WARN, "mem: SNAP %-4d %-6d %-16s %-2c %8.1f %7d",
                    i + 1, t.pid, t.comm, t.state ? t.state : '-',
                    t.rss_kb / 1024.0, t.threads);
    }
}

// ---------- 每 tick 采样 + 告警评估 ----------
void sample(const TrackConfig& cfg, long long now) {
    (void)now;
    long total = 0, avail = 0, free = 0, st = 0, sf = 0;
    if (read_meminfo(total, avail, free, st, sf)) {
        g_total_kb = total; g_avail_kb = avail; g_free_kb = free;
        g_swap_total_kb = st; g_swap_free_kb = sf;
    }
    double avail_mb = g_avail_kb / 1024.0;
    bool over = (avail_mb < (double)cfg.mem_available_threshold_mb);
    int ch = eval_alert(st_avail, over, avail_mb, cfg.mem_alert_duration);

    // 跟踪告警期间 MemAvailable 最小值
    if (st_avail.active) {
        if (g_min_avail_mb < 0 || avail_mb < g_min_avail_mb) g_min_avail_mb = avail_mb;
    }

    if (ch & 1) {
        double total_mb = g_total_kb / 1024.0;
        double used_pct = (total_mb > 0) ? (100.0 * (total_mb - avail_mb) / total_mb) : 0.0;
        Logger::log(LogLevel::WARN,
            "mem: ALERT avail=%.1fMB thr=%dMB total=%.1fMB used=%.1f%%",
            avail_mb, cfg.mem_available_threshold_mb, total_mb, used_pct);
        take_snapshot(cfg);
    } else if (ch & 2) {
        Logger::log(LogLevel::INFO,
            "mem: RECOVERED avail=%.1fMB thr=%dMB min=%.1fMB",
            avail_mb, cfg.mem_available_threshold_mb,
            g_min_avail_mb >= 0 ? g_min_avail_mb : avail_mb);
        g_min_avail_mb = -1.0;
    }
}

// ---------- 周期概览日志（正常 mem_log_interval / 告警中 60s） ----------
void maybe_log(const TrackConfig& cfg, long long now) {
    long long interval = any_alert_active() ? 60LL * 1000 : (long long)cfg.mem_log_interval * 1000;
    if (now - g_last_log_ms < interval) return;
    g_last_log_ms = now;

    double avail_mb = g_avail_kb / 1024.0;
    double total_mb = g_total_kb / 1024.0;
    double used_pct = (total_mb > 0) ? (100.0 * (total_mb - avail_mb) / total_mb) : 0.0;
    Logger::log(LogLevel::INFO,
        "mem: avail=%.1fMB total=%.1fMB used=%.1f%% free=%.1fMB swap=%.1f/%.1fMB",
        avail_mb, total_mb, used_pct, g_free_kb / 1024.0,
        g_swap_free_kb / 1024.0, g_swap_total_kb / 1024.0);
}

} // namespace

void mem_monitor_tick(const TrackConfig& cfg) {
    if (!cfg.mem_monitor_enable) return;
    long long now = now_ms();
    if (!g_inited) {
        g_inited = true;
        g_start_ms = now;
        long ps = sysconf(_SC_PAGESIZE);
        g_page_kb = (ps > 0) ? (ps / 1024) : 4;
        // 首轮读一次并打启动行；内存为瞬时值，无需 baseline，下一轮即可告警
        long total = 0, avail = 0, free = 0, st = 0, sf = 0;
        if (read_meminfo(total, avail, free, st, sf)) {
            g_total_kb = total; g_avail_kb = avail; g_free_kb = free;
            g_swap_total_kb = st; g_swap_free_kb = sf;
        }
        g_last_log_ms = now;
        Logger::log(LogLevel::INFO,
            "mem monitor: total=%.1fMB avail=%.1fMB thr=%dMB debounce=%d",
            g_total_kb / 1024.0, g_avail_kb / 1024.0,
            cfg.mem_available_threshold_mb, cfg.mem_alert_duration);
        return;
    }
    sample(cfg, now);
    maybe_log(cfg, now);
}
