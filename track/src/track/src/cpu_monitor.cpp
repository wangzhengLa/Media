// CPU 监控模块：纯读 /proc 与 sysfs，不阻塞主循环。
// 口径：
//   - 整机 total%：归一化 0~100（/proc/stat 差分，与 sys_monitor collect.c 对齐）
//   - 每核 core%：每个核的 busy%，0~100
//   - 进程 per-core%：占单核百分比，可 >100（top 风格）
//   - 进程 normalized% = per-core% / ncpu
// 告警：仅整机 total% 超 cpu_total_threshold 时告警（去抖 cpu_alert_duration 次）。
//      温度/频率/负载等仍作为信息打印。
// 告警时一次性打印全量 CPU 信息（整机/每核/ctxt/running-blocked/频率/温度/负载）
// 并拍全系统 TOP N 进程快照。

#include "cpu_monitor.h"
#include "logger.h"
#include "monitor_common.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
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

// ---------- 采集：/proc/stat 整机 + 每核 + 聚合计数 ----------
// 解析第一行 cpu（整机）、cpu0..cpuN（每核）、ctxt、procs_running、procs_blocked。
// 每核按出现顺序（cpu0..cpuN-1）存入 core_total/core_idle，与核编号对齐。
bool read_cpu_stat_full(uint64_t& out_total, uint64_t& out_idle,
                        std::vector<uint64_t>& core_total,
                        std::vector<uint64_t>& core_idle,
                        uint64_t& out_ctxt,
                        uint64_t& out_running, uint64_t& out_blocked) {
    FILE* f = fopen("/proc/stat", "r");
    if (!f) return false;
    char line[512];
    core_total.clear();
    core_idle.clear();
    out_total = out_idle = 0;
    out_ctxt = 0;
    out_running = 0;
    out_blocked = 0;
    while (fgets(line, sizeof(line), f)) {
        char key[32];
        int n = 0;
        if (sscanf(line, "%31s%n", key, &n) != 1) continue;
        char* rest = line + n;
        if (strcmp(key, "cpu") == 0) {
            unsigned long long user = 0, nice = 0, sys = 0, idle = 0,
                               iowait = 0, irq = 0, softirq = 0, steal = 0;
            if (sscanf(rest, "%llu %llu %llu %llu %llu %llu %llu %llu",
                       &user, &nice, &sys, &idle, &iowait, &irq, &softirq, &steal) >= 4) {
                out_idle = idle + iowait;
                out_total = user + nice + sys + out_idle + irq + softirq + steal;
            }
        } else if (key[0] == 'c' && key[1] == 'p' && key[2] == 'u' &&
                   key[3] >= '0' && key[3] <= '9') {
            unsigned long long user = 0, nice = 0, sys = 0, idle = 0,
                               iowait = 0, irq = 0, softirq = 0, steal = 0;
            if (sscanf(rest, "%llu %llu %llu %llu %llu %llu %llu %llu",
                       &user, &nice, &sys, &idle, &iowait, &irq, &softirq, &steal) >= 4) {
                uint64_t idl = idle + iowait;
                uint64_t tot = user + nice + sys + idl + irq + softirq + steal;
                core_total.push_back(tot);
                core_idle.push_back(idl);
            }
        } else if (strcmp(key, "ctxt") == 0) {
            unsigned long long c = 0;
            if (sscanf(rest, "%llu", &c) == 1) out_ctxt = c;
        } else if (strcmp(key, "procs_running") == 0) {
            unsigned long long c = 0;
            if (sscanf(rest, "%llu", &c) == 1) out_running = c;
        } else if (strcmp(key, "procs_blocked") == 0) {
            unsigned long long c = 0;
            if (sscanf(rest, "%llu", &c) == 1) out_blocked = c;
        }
    }
    fclose(f);
    return out_total > 0;
}

// ---------- 采集：loadavg ----------
bool read_loadavg(double load[3]) {
    FILE* f = fopen("/proc/loadavg", "r");
    if (!f) return false;
    int n = fscanf(f, "%lf %lf %lf", &load[0], &load[1], &load[2]);
    fclose(f);
    return n == 3;
}

// ---------- 采集：温度 ----------
// 在 thermal_zone 中找 type 含 substr 的 zone；找不到返回 false。
bool read_zone_temp(const char* substr, double& out_c) {
    char path[128];
    for (int i = 0; i < 16; i++) {
        char type[64] = {0};
        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/type", i);
        FILE* f = fopen(path, "r");
        if (!f) break; // zone 编号连续，遇到第一个不存在就停
        if (fgets(type, sizeof(type), f)) {
            fclose(f);
            if (strstr(type, substr)) {
                snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", i);
                f = fopen(path, "r");
                if (!f) return false;
                int milli = 0;
                int ok = fscanf(f, "%d", &milli);
                fclose(f);
                if (ok != 1) return false;
                out_c = milli / 1000.0;
                return true;
            }
        } else {
            fclose(f);
        }
    }
    return false;
}

// ---------- 采集：调频 ----------
bool read_freq(double& cur_ghz, double& max_ghz, char* gov, size_t gov_sz) {
    long cur = 0, mx = 0;
    FILE* f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
    if (!f) return false;
    if (fscanf(f, "%ld", &cur) != 1) { fclose(f); return false; }
    fclose(f);
    f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", "r");
    if (!f) return false;
    if (fscanf(f, "%ld", &mx) != 1) { fclose(f); return false; }
    fclose(f);
    gov[0] = '\0';
    f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "r");
    if (f) {
        if (fgets(gov, (int)gov_sz, f)) {
            // 去掉换行
            size_t l = strlen(gov);
            while (l > 0 && (gov[l-1] == '\n' || gov[l-1] == '\r')) gov[--l] = '\0';
        }
        fclose(f);
    }
    cur_ghz = cur / 1000000.0;
    max_ghz = mx / 1000000.0;
    return true;
}

// ---------- 状态 ----------
bool g_inited = false;
long long g_last_log_ms = 0;   // 周期概览日志节流（300s/告警中60s）
long long g_start_ms = 0;      // uptime 基准

uint64_t g_last_total = 0, g_last_idle = 0;

// 每核 baseline（与 cpu0..cpuN-1 对齐）
struct CoreStat { uint64_t total; uint64_t idle; };
std::vector<CoreStat> g_core_base;
std::vector<double> g_core_pct;   // 最近一次每核 busy%（0~100）

// 全进程扫描 baseline。
// 扫描/采样频率与进程存活检测一致（check_interval），故 take_snapshot 的进程
// CPU% 窗口恒等于整机采样窗口（≈check_interval 秒），与告警时的 total% 同期、可对齐诊断。
// scan_all 不再调用 take_snapshot，baseline 在覆盖前已被 sample_system 消费，
// 无需再额外保存快照副本。
std::unordered_map<int, uint64_t> g_scan_ticks; // pid -> utime+stime
long long g_last_scan_wall_ms = 0;

// 上下文切换 / 运行阻塞进程（最近值）
uint64_t g_last_ctxt = 0;
long long g_last_ctxt_ms = 0;
double g_ctxt_rate = 0;        // ctxt/s
uint64_t g_procs_running = 0;
uint64_t g_procs_blocked = 0;

// 最近系统采样值（供日志使用）
double g_total_pct = 0;
double g_load[3] = {0, 0, 0};
double g_temp_cpu = -1;
double g_temp_npu = -1;
double g_temp_ddr = -1;
double g_cur_ghz = -1, g_max_ghz = -1;
char g_gov[32] = "";
int g_ncpu = 1;
long g_hz = 100;
long g_page_kb = 4;

// 告警状态（仅整机 total 一个 trigger；AlertState 定义见 monitor_common.h）
AlertState st_total;
long long g_last_snapshot_ms = 0;

// eval_alert 定义见 monitor_common.h

bool any_alert_active() {
    return st_total.active;
}

// 拼接频率段（含 throttled 标记），返回写入字符数
int fmt_freq(char* buf, size_t sz) {
    int off = 0;
    if (g_cur_ghz >= 0) {
        off += snprintf(buf + off, sz - off, "freq=%.2f/%.2fGHz", g_cur_ghz, g_max_ghz);
        if (g_max_ghz > 0 && g_cur_ghz < g_max_ghz - 0.01)
            off += snprintf(buf + off, sz - off, "(throttled)");
        if (g_gov[0]) off += snprintf(buf + off, sz - off, " gov=%s", g_gov);
    }
    return off;
}

// ---------- 告警时打印全量 CPU 信息到主日志 ----------
void log_cpu_details(const TrackConfig& cfg, long long now, double trigger_val) {
    char freq[96]; fmt_freq(freq, sizeof(freq));
    // 每核
    char cores[256]; int coff = 0;
    coff += snprintf(cores + coff, sizeof(cores) - coff, "cores:");
    for (size_t i = 0; i < g_core_pct.size() && coff < (int)sizeof(cores) - 16; i++) {
        coff += snprintf(cores + coff, sizeof(cores) - coff, " cpu%zu=%.1f", i, g_core_pct[i]);
    }
    Logger::log(LogLevel::WARN,
        "cpu: DETAIL total=%.1f%% thr=%d%% %s load=%.2f/%.2f/%.2f "
        "ctxt=%.0f/s running=%llu blocked=%llu ncpu=%d uptime=%llds",
        trigger_val, cfg.cpu_total_threshold, freq,
        g_load[0], g_load[1], g_load[2],
        g_ctxt_rate, (unsigned long long)g_procs_running, (unsigned long long)g_procs_blocked,
        g_ncpu, (now - g_start_ms) / 1000);
    Logger::log(LogLevel::WARN, "cpu: DETAIL %s", cores);
    Logger::log(LogLevel::WARN,
        "cpu: DETAIL temp_cpu=%.1fC temp_npu=%.1fC temp_ddr=%.1fC hz=%ld page=%ldKB",
        g_temp_cpu, g_temp_npu, g_temp_ddr, g_hz, g_page_kb);
}

// ---------- 拍 TOP N 快照（写入主日志 track.log，不另建文件）----------
// 概览(total/load/cores/temp)已由 log_cpu_details 输出到主日志，这里只补 TOP N 进程表。
void take_snapshot(const TrackConfig& cfg, double trigger_val) {
    if (!cfg.cpu_snapshot_on_alert) return;
    long long now = now_ms();
    if (now - g_last_snapshot_ms < (long long)cfg.cpu_snapshot_cooldown * 1000) return;
    g_last_snapshot_ms = now;

    // 用上一轮扫描的 baseline 算率：窗口 = now - g_last_scan_wall_ms ≈ check_interval，
    // 与本次 sample_system 算 total% 的窗口一致（scan_all 在 sample 之后才覆盖）。
    long long wall = (g_last_scan_wall_ms > 0) ? (now - g_last_scan_wall_ms) : 0;
    if (wall <= 0) {
        // 没 baseline 就无法算率，跳过
        return;
    }

    struct Top {
        int pid; char comm[64]; char state;
        double pct, user, sys, norm;
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
        auto it = g_scan_ticks.find(pid);
        if (it == g_scan_ticks.end()) continue; // 无 baseline 无法算率
        uint64_t cur = u + s;
        uint64_t delta = (cur >= it->second) ? (cur - it->second) : 0;
        double denom = (double)wall / 1000.0 * (double)g_hz;
        double pct = 100.0 * (double)delta / denom;
        Top t; t.pid = pid; t.pct = pct; t.rss_kb = rss * g_page_kb;
        t.threads = nt; t.state = st;
        strncpy(t.comm, comm, sizeof(t.comm) - 1); t.comm[sizeof(t.comm)-1] = '\0';
        // baseline 只存了 utime+stime 总和，无法精确拆窗口内 user/sys 增量；
        // 这里按当前累计 u/s 占比近似拆分（量级参考）。
        if (cur > 0) {
            t.user = pct * (double)u / (double)cur;
            t.sys  = pct * (double)s / (double)cur;
        } else {
            t.user = 0; t.sys = 0;
        }
        t.norm = pct / (double)g_ncpu;
        tops.push_back(t);
    }
    closedir(d);

    std::sort(tops.begin(), tops.end(), [](const Top& a, const Top& b){ return a.pct > b.pct; });
    int n = (int)tops.size();
    if (n > cfg.cpu_top_n) n = cfg.cpu_top_n;

    Logger::log(LogLevel::WARN, "cpu: SNAPSHOT top=%d val=%.1f%% wall=%lldms",
                n, trigger_val, wall);
    Logger::log(LogLevel::WARN,
        "cpu: SNAP rank pid    comm             st cpu%%      user%%    sys%%     norm%%    threads rssKB");
    for (int i = 0; i < n; i++) {
        const Top& t = tops[i];
        Logger::log(LogLevel::WARN,
            "cpu: SNAP %-4d %-6d %-16s %-2c %8.1f %8.1f %8.1f %8.1f %7d %8ld",
            i + 1, t.pid, t.comm, t.state ? t.state : '-',
            t.pct, t.user, t.sys, t.norm, t.threads, t.rss_kb);
    }
}

// ---------- 5s 采样：整机 + 每核 + 聚合计数，仅整机告警 ----------
void sample_system(const TrackConfig& cfg, long long now) {
    uint64_t total = 0, idle = 0;
    std::vector<uint64_t> core_total, core_idle;
    uint64_t ctxt = 0, running = 0, blocked = 0;
    if (read_cpu_stat_full(total, idle, core_total, core_idle, ctxt, running, blocked) && g_last_total > 0) {
        uint64_t td = (total >= g_last_total) ? (total - g_last_total) : 0;
        uint64_t id = (idle >= g_last_idle) ? (idle - g_last_idle) : 0;
        if (td > 0) {
            g_total_pct = 100.0 * (double)(td - id) / (double)td;
            if (g_total_pct < 0) g_total_pct = 0;
            if (g_total_pct > 100.0) g_total_pct = 100.0; // 归一化口径：0~100
        }
        // 每核
        if (g_core_base.size() == core_total.size() && !core_total.empty()) {
            if (g_core_pct.size() != core_total.size()) g_core_pct.resize(core_total.size());
            for (size_t i = 0; i < core_total.size(); i++) {
                uint64_t ctd = (core_total[i] >= g_core_base[i].total) ? (core_total[i] - g_core_base[i].total) : 0;
                uint64_t cid = (core_idle[i] >= g_core_base[i].idle) ? (core_idle[i] - g_core_base[i].idle) : 0;
                g_core_pct[i] = (ctd > 0) ? (100.0 * (double)(ctd - cid) / (double)ctd) : 0.0;
                if (g_core_pct[i] < 0) g_core_pct[i] = 0;
                if (g_core_pct[i] > 100.0) g_core_pct[i] = 100.0;
            }
        }
    }
    // 更新 baseline（即使读取失败也用新值覆盖，避免卡在旧 baseline 上）
    g_last_total = total; g_last_idle = idle;
    if (g_core_base.size() != core_total.size()) g_core_base.resize(core_total.size());
    for (size_t i = 0; i < core_total.size(); i++) {
        g_core_base[i].total = core_total[i];
        g_core_base[i].idle = core_idle[i];
    }

    // ctxt 速率
    if (g_last_ctxt_ms > 0) {
        long long dt = now - g_last_ctxt_ms;
        if (dt > 0 && ctxt >= g_last_ctxt) {
            g_ctxt_rate = 1000.0 * (double)(ctxt - g_last_ctxt) / (double)dt;
        }
    }
    g_last_ctxt = ctxt; g_last_ctxt_ms = now;
    g_procs_running = running;
    g_procs_blocked = blocked;

    read_loadavg(g_load);
    read_zone_temp("cpu", g_temp_cpu);
    read_zone_temp("npu", g_temp_npu);
    read_zone_temp("ddr", g_temp_ddr);
    read_freq(g_cur_ghz, g_max_ghz, g_gov, sizeof(g_gov));

    // 告警评估：仅整机 total
    int ch;
    bool total_over = (g_total_pct >= cfg.cpu_total_threshold);
    ch = eval_alert(st_total, total_over, g_total_pct, cfg.cpu_alert_duration);
    if (ch & 1) {
        char freq[96]; fmt_freq(freq, sizeof(freq));
        Logger::log(LogLevel::WARN,
            "cpu: ALERT total=%.1f%% total_thr=%d%% load=%.2f/%.2f/%.2f "
            "%s ncpu=%d",
            g_total_pct, cfg.cpu_total_threshold, g_load[0], g_load[1], g_load[2],
            freq, g_ncpu);
        log_cpu_details(cfg, now, g_total_pct);
        take_snapshot(cfg, g_total_pct);
    } else if (ch & 2) {
        Logger::log(LogLevel::INFO, "cpu: RECOVERED total=%.1f%% thr=%d%% peak=%.1f%%",
                    g_total_pct, cfg.cpu_total_threshold, st_total.peak);
    }
}

// ---------- 全进程扫描：仅维护 baseline（供快照算率），不再告警 ----------
// 触发频率与进程存活检测一致（check_interval，见 cpu_monitor_tick），保证快照窗口 = 采样窗口。
void scan_all(const TrackConfig& cfg, long long now) {
    (void)cfg;
    std::unordered_map<int, uint64_t> cur_ticks;
    DIR* d = opendir("/proc");
    if (!d) return;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        int pid = atoi(de->d_name);
        if (pid <= 0) continue;
        char comm[64]; uint64_t u = 0, s = 0;
        if (!read_pid_stat(pid, comm, sizeof(comm), u, s, nullptr, nullptr)) continue;
        cur_ticks[pid] = u + s;
    }
    closedir(d);

    // 覆盖 baseline：take_snapshot（在 sample_system 内、本次扫描之前）已消费了旧值，
    // 此处安全覆盖为当前值，供下一轮采样算率。
    g_scan_ticks = std::move(cur_ticks);
    g_last_scan_wall_ms = now;
}

// ---------- 周期概览日志（正常 5min / 告警中 60s 更新） ----------
void maybe_log(const TrackConfig& cfg, long long now) {
    long long interval = any_alert_active() ? 60LL * 1000 : (long long)cfg.cpu_log_interval * 1000;
    if (now - g_last_log_ms < interval) return;
    g_last_log_ms = now;

    char sys[512];
    int off = 0;
    off += snprintf(sys + off, sizeof(sys) - off, "cpu: total=%.1f%%(整机)", g_total_pct);
    // 每核
    for (size_t i = 0; i < g_core_pct.size() && off < (int)sizeof(sys) - 16; i++) {
        off += snprintf(sys + off, sizeof(sys) - off, " cpu%zu=%.1f", i, g_core_pct[i]);
    }
    if (g_temp_cpu >= 0) off += snprintf(sys + off, sizeof(sys) - off, " temp=%.1fC", g_temp_cpu);
    if (g_cur_ghz >= 0) {
        off += snprintf(sys + off, sizeof(sys) - off, " freq=%.2f/%.2fGHz", g_cur_ghz, g_max_ghz);
        if (g_max_ghz > 0 && g_cur_ghz < g_max_ghz - 0.01) off += snprintf(sys + off, sizeof(sys) - off, "(throttled)");
    }
    if (g_gov[0]) off += snprintf(sys + off, sizeof(sys) - off, " gov=%s", g_gov);
    off += snprintf(sys + off, sizeof(sys) - off, " load=%.2f/%.2f/%.2f ctxt=%.0f/s run=%llu/blk=%llu ncpu=%d",
                    g_load[0], g_load[1], g_load[2], g_ctxt_rate,
                    (unsigned long long)g_procs_running, (unsigned long long)g_procs_blocked, g_ncpu);
    Logger::log(LogLevel::INFO, "%s", sys);
}

} // namespace

void cpu_monitor_tick(const TrackConfig& cfg) {
    if (!cfg.cpu_monitor_enable) return;
    long long now = now_ms();
    if (!g_inited) {
        g_inited = true;
        g_start_ms = now;
        g_hz = sysconf(_SC_CLK_TCK);
        if (g_hz <= 0) g_hz = 100;
        g_ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
        if (g_ncpu <= 0) g_ncpu = 1;
        long ps = sysconf(_SC_PAGESIZE);
        g_page_kb = (ps > 0) ? (ps / 1024) : 4;
        g_core_pct.assign(g_ncpu, 0.0);
        // 首轮建 baseline，不输出不告警
        uint64_t total = 0, idle = 0;
        std::vector<uint64_t> core_total, core_idle;
        uint64_t ctxt = 0, running = 0, blocked = 0;
        read_cpu_stat_full(total, idle, core_total, core_idle, ctxt, running, blocked);
        g_last_total = total; g_last_idle = idle;
        g_core_base.resize(core_total.size());
        for (size_t i = 0; i < core_total.size(); i++) {
            g_core_base[i].total = core_total[i];
            g_core_base[i].idle = core_idle[i];
        }
        g_last_ctxt = ctxt; g_last_ctxt_ms = now;
        g_procs_running = running; g_procs_blocked = blocked;

        g_last_log_ms = now;

        // 全进程扫描 baseline（快照用），首轮即建
        {
            DIR* d = opendir("/proc");
            if (d) {
                struct dirent* de;
                while ((de = readdir(d)) != nullptr) {
                    int pid = atoi(de->d_name);
                    if (pid <= 0) continue;
                    char comm[64]; uint64_t u = 0, s = 0;
                    if (read_pid_stat(pid, comm, sizeof(comm), u, s, nullptr, nullptr)) {
                        g_scan_ticks[pid] = u + s;
                    }
                }
                closedir(d);
            }
        }
        g_last_scan_wall_ms = now;

        Logger::log(LogLevel::INFO, "cpu monitor: ncpu=%d hz=%ld page=%ldKB sample=%ds snap_window=%ds total_thr=%d%%",
                    g_ncpu, g_hz, g_page_kb, cfg.check_interval, cfg.check_interval, cfg.cpu_total_threshold);
        return;
    }

    // 主循环已按 check_interval 节流 tick，这里每 tick 都采样+扫描，
    // total% 与快照进程 CPU% 的窗口即 check_interval 秒。
    sample_system(cfg, now);
    scan_all(cfg, now);
    maybe_log(cfg, now);
}
