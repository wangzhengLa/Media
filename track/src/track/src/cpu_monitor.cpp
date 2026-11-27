// CPU 监控模块：纯读 /proc 与 sysfs，不阻塞主循环。

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

// ---------- 采集：/proc/stat 整机 ----------
// 仅解析第一行 cpu（整机）；cpu0..cpuN 不再采集。
bool read_cpu_stat_full(uint64_t& out_total, uint64_t& out_idle) {
    FILE* f = fopen("/proc/stat", "r");
    if (!f) return false;
    char line[512];
    out_total = out_idle = 0;
    while (fgets(line, sizeof(line), f)) {
        char key[32];
        int n = 0;
        if (sscanf(line, "%31s%n", key, &n) != 1) continue;
        if (strcmp(key, "cpu") != 0) continue;
        char* rest = line + n;
        unsigned long long user = 0, nice = 0, sys = 0, idle = 0,
                           iowait = 0, irq = 0, softirq = 0, steal = 0;
        if (sscanf(rest, "%llu %llu %llu %llu %llu %llu %llu %llu",
                   &user, &nice, &sys, &idle, &iowait, &irq, &softirq, &steal) >= 4) {
            out_idle = idle + iowait;
            out_total = user + nice + sys + out_idle + irq + softirq + steal;
            break;
        }
    }
    fclose(f);
    return out_total > 0;
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

// ---------- 状态（模块私有，收拢所有 file-scope 变量）----------
struct State {
    bool inited = false;

    // 整机 baseline
    uint64_t last_total = 0, last_idle = 0;

    // 全进程扫描 baseline（供快照算率）。扫描/采样频率与进程存活检测一致（check_interval），
    std::unordered_map<int, uint64_t> scan_ticks; // pid -> utime+stime
    long long last_scan_wall_ms = 0;

    // 最近系统采样值（供日志使用）
    double total_pct = 0;
    double temp_cpu = -1;
    double cur_ghz = -1, max_ghz = -1;
    char   gov[32] = "";
    int    ncpu = 1;
    long   hz = 100;
    long   page_kb = 4;

    // 告警状态（仅整机 total 一个 trigger；AlertState 定义见 monitor_common.h）
    AlertState alert;
    long long last_snapshot_ms = 0;
};
State state;

// init 与告警共用的单行 CPU 摘要：仅 total thr freq gov ncpu temp 六个字段。
// 无频点/温度时显示 N/A；gov 为空时显示 N/A。
void log_cpu_info(LogLevel lvl, const TrackConfig& cfg, double total_pct) {
    char freq[48];
    if (state.cur_ghz >= 0) {
        snprintf(freq, sizeof(freq), "%.2f/%.2fGHz", state.cur_ghz, state.max_ghz);
        if (state.max_ghz > 0 && state.cur_ghz < state.max_ghz - 0.01) {
            size_t l = strlen(freq);
            snprintf(freq + l, sizeof(freq) - l, "(throttled)");
        }
    } else {
        snprintf(freq, sizeof(freq), "N/A");
    }
    const char* gov = state.gov[0] ? state.gov : "N/A";
    char temp[24];
    if (state.temp_cpu >= 0) snprintf(temp, sizeof(temp), "%.1fC", state.temp_cpu);
    else snprintf(temp, sizeof(temp), "N/A");
    Logger::log(lvl, "cpu: total=%.1f%% thr=%d%% freq=%s gov=%s ncpu=%d temp=%s",
                total_pct, cfg.cpu_total_threshold, freq, gov, state.ncpu, temp);
}

// ---------- 拍 TOP N 快照（写入主日志 track.log）----------
// 告警摘要(total/thr/freq/gov/ncpu/temp)已由 log_cpu_info 输出，这里只补 TOP N 进程表。
void take_snapshot(const TrackConfig& cfg, double trigger_val) {
    long long now = now_ms();
    if (now - state.last_snapshot_ms < (long long)cfg.snapshot_cooldown * 1000) return;
    state.last_snapshot_ms = now;

    // 用上一轮扫描的 baseline 算率：窗口 = now - state.last_scan_wall_ms ≈ check_interval，
    // 与本次 sample_system 算 total% 的窗口一致（scan_all 在 sample 之后才覆盖）。
    long long wall = (state.last_scan_wall_ms > 0) ? (now - state.last_scan_wall_ms) : 0;
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
        auto it = state.scan_ticks.find(pid);
        if (it == state.scan_ticks.end()) continue; // 无 baseline 无法算率
        uint64_t cur = u + s;
        uint64_t delta = (cur >= it->second) ? (cur - it->second) : 0;
        double denom = (double)wall / 1000.0 * (double)state.hz;
        double pct = 100.0 * (double)delta / denom;
        Top t; t.pid = pid; t.pct = pct; t.rss_kb = rss * state.page_kb;
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
        t.norm = pct / (double)state.ncpu;
        tops.push_back(t);
    }
    closedir(d);

    std::sort(tops.begin(), tops.end(), [](const Top& a, const Top& b){ return a.pct > b.pct; });
    int n = clip_top_n((int)tops.size(), cfg.snapshot_top_n);

    Logger::log(LogLevel::WARN, "cpu: SNAPSHOT top=%d val=%.1f%%",
                n, trigger_val);
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

// ---------- 5s 采样：整机 + 聚合计数，仅整机告警 ----------
void sample_system(const TrackConfig& cfg) {
    uint64_t total = 0, idle = 0;
    if (read_cpu_stat_full(total, idle) && state.last_total > 0) {
        uint64_t td = (total >= state.last_total) ? (total - state.last_total) : 0;
        uint64_t id = (idle >= state.last_idle) ? (idle - state.last_idle) : 0;
        if (td > 0) {
            state.total_pct = 100.0 * (double)(td - id) / (double)td;
            if (state.total_pct < 0) state.total_pct = 0;
            if (state.total_pct > 100.0) state.total_pct = 100.0; // 归一化口径：0~100
        }
    }
    // 更新 baseline（即使读取失败也用新值覆盖，避免卡在旧 baseline 上）
    state.last_total = total; state.last_idle = idle;

    read_zone_temp("cpu", state.temp_cpu);
    read_freq(state.cur_ghz, state.max_ghz, state.gov, sizeof(state.gov));

    // 告警评估：仅整机 total
    int ch;
    bool total_over = (state.total_pct >= cfg.cpu_total_threshold);
    ch = eval_alert(state.alert, total_over, state.total_pct);
    if (ch & 1) {
        // 刚触发告警：第1行提示 → 第2行单行摘要；快照由下方 active 块统一拍
        Logger::log(LogLevel::WARN, "cpu: CPU usage has exceeded the threshold");
        log_cpu_info(LogLevel::WARN, cfg, state.total_pct);
    } else if (ch & 2) {
        Logger::log(LogLevel::INFO, "cpu: RECOVERED total=%.1f%% thr=%d%% peak=%.1f%%",
                    state.total_pct, cfg.cpu_total_threshold, state.alert.peak);
    }

    // 告警持续期间（含刚触发）按冷却拍快照；恢复后 state.alert.active 为假即停
    if (state.alert.active) {
        take_snapshot(cfg, state.total_pct);
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
    state.scan_ticks = std::move(cur_ticks);
    state.last_scan_wall_ms = now;
}

} // namespace

void cpu_monitor_tick(const TrackConfig& cfg) {
    long long now = now_ms();
    if (!state.inited) {
        state.inited = true;
        state.hz = sysconf(_SC_CLK_TCK);
        if (state.hz <= 0) state.hz = 100;
        state.ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
        if (state.ncpu <= 0) state.ncpu = 1;
        long ps = sysconf(_SC_PAGESIZE);
        state.page_kb = (ps > 0) ? (ps / 1024) : 4;
        // 首轮建 baseline，不输出不告警
        uint64_t total = 0, idle = 0;
        read_cpu_stat_full(total, idle);
        state.last_total = total; state.last_idle = idle;

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
                        state.scan_ticks[pid] = u + s;
                    }
                }
                closedir(d);
            }
        }
        state.last_scan_wall_ms = now;

        // 启动横幅：与告警摘要同格式（total thr freq gov ncpu temp）
        read_zone_temp("cpu", state.temp_cpu);
        read_freq(state.cur_ghz, state.max_ghz, state.gov, sizeof(state.gov));
        log_cpu_info(LogLevel::INFO, cfg, state.total_pct);
        return;
    }

    // 主循环已按 check_interval 节流 tick，这里每 tick 都采样+扫描，
    // total% 与快照进程 CPU% 的窗口即 check_interval 秒。
    sample_system(cfg);
    scan_all(cfg, now);
}
