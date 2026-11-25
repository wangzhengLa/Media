// Flash 监控模块：纯 statvfs，不阻塞主循环。
// 口径：
//   - used% = (f_blocks - f_bavail) / f_blocks * 100；用 f_bavail（非 root 可用）反映真实可写空间
//   - 按 f_fsid 去重（同文件系统只告警一次），只读分区（ST_RDONLY）跳过告警
//   - 周期概览逐路径打印；告警时统计触发分区挂载目录下"一级子项"的占用，取前 N
//     （哪个分区超阈值就看哪个分区；用 du -sbx 取大小，不跨挂载点、不跟随符号链接，timeout 兜底防阻塞）
//   - 逐行写入主日志 track.log；cooldown 按 f_fsid 独立
// 观察式：仅日志，不自动干预。

#include "flash_monitor.h"
#include "logger.h"
#include "monitor_common.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace {

// ---------- 时间 ----------
long long now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// ---------- 路径解析（空格分隔） ----------
std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> v;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!cur.empty()) { v.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) v.push_back(cur);
    return v;
}

// ---------- 单文件系统信息 ----------
struct FsInfo {
    std::string path;
    std::string fsid_key;   // f_fsid 原始字节，用于去重
    bool        read_only;
    double      used_pct;   // 0~100
    unsigned long long total_mb;
    unsigned long long avail_mb;
    unsigned long long used_mb;
};

// 对一个路径 statvfs，填充 FsInfo；失败返回 false。
bool stat_one(const std::string& path, FsInfo& out) {
    struct statvfs sv;
    if (statvfs(path.c_str(), &sv) != 0) return false;
    out.path = path;
    out.fsid_key.assign((const char*)&sv.f_fsid, sizeof(sv.f_fsid));
    out.read_only = (sv.f_flag & ST_RDONLY) != 0;

    unsigned long long blocks = sv.f_blocks;
    unsigned long long bavail = sv.f_bavail;
    unsigned long long used = (blocks >= bavail) ? (blocks - bavail) : 0;
    out.used_pct = (blocks > 0) ? (100.0 * (double)used / (double)blocks) : 0.0;
    if (out.used_pct < 0) out.used_pct = 0;
    if (out.used_pct > 100.0) out.used_pct = 100.0;

    unsigned long long frsize = sv.f_frsize ? sv.f_frsize : sv.f_bsize;
    out.total_mb = blocks * frsize / (1024ULL * 1024ULL);
    out.avail_mb = bavail * frsize / (1024ULL * 1024ULL);
    out.used_mb  = used   * frsize / (1024ULL * 1024ULL);
    return true;
}

// ---------- 状态 ----------
bool g_inited = false;
long long g_last_log_ms = 0;

std::vector<std::string> g_paths;                  // 解析后的监控路径
std::map<std::string, AlertState> g_fs_alert;     // fsid_key -> 告警状态（去重后每个唯一 fs 一个）
std::map<std::string, long long> g_last_snap_ms;  // fsid_key -> 上次快照时间（每个分区独立冷却）

bool any_alert_active() {
    for (auto& kv : g_fs_alert) {
        if (kv.second.active) return true;
    }
    return false;
}

// ---------- 调用 du -sbx 取路径表观大小（字节）----------
// -s 汇总；-b 表观大小(字节)；-x 不跨挂载点；timeout 兜底防阻塞主循环。
// 失败/不可读/超时返回 0。路径来自受信任配置，单引号包裹防 shell 注入。
unsigned long long du_size(const std::string& path) {
    char cmd[1280];
    snprintf(cmd, sizeof(cmd), "timeout 3 du -sbx '%s' 2>/dev/null", path.c_str());
    FILE* fp = popen(cmd, "r");
    if (!fp) return 0;
    char line[256];
    unsigned long long sz = 0;
    if (fgets(line, sizeof(line), fp) != nullptr) {
        sz = strtoull(line, nullptr, 10);   // 输出形如 "123456\t/path"
    }
    pclose(fp);
    return sz;
}

// ---------- 拍快照：统计触发分区挂载目录下一级子项的占用，取前 N（写入主日志，不另建文件）----------
// 只看一级子项（如 /userdata 下的 etc/lib/usr/var...），每个子项用 du -sbx 取总大小，
// 按大小降序取前 N，不足 N 则有几个报几个。cooldown 按 f_fsid 独立。
void take_snapshot(const TrackConfig& cfg, const FsInfo& trigger) {
    if (!cfg.flash_snapshot_on_alert) return;
    long long now = now_ms();
    long long& last = g_last_snap_ms[trigger.fsid_key];   // 每个分区独立冷却
    if (now - last < (long long)cfg.flash_snapshot_cooldown * 1000) return;
    last = now;

    struct stat rst;
    bool have_dev = (lstat(trigger.path.c_str(), &rst) == 0);
    std::vector<std::pair<std::string, unsigned long long>> items;  // 一级子项 -> du -sbx 总大小
    if (have_dev) {
        DIR* d = opendir(trigger.path.c_str());
        if (d) {
            struct dirent* de;
            while ((de = readdir(d)) != nullptr) {
                if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
                char path[1024];
                snprintf(path, sizeof(path), "%s/%s", trigger.path.c_str(), de->d_name);
                struct stat st;
                if (lstat(path, &st) != 0) continue;
                if (S_ISDIR(st.st_mode)) {
                    if (st.st_dev != rst.st_dev) continue;   // xdev：跨挂载点的一级子项跳过
                } else if (!S_ISREG(st.st_mode)) {
                    continue;   // 符号链接/设备/FIFO 等跳过
                }
                items.push_back({path, du_size(path)});   // 目录/文件统一用 du -sbx 取大小
            }
            closedir(d);
        }
    }
    std::sort(items.begin(), items.end(),
              [](const std::pair<std::string, unsigned long long>& a,
                 const std::pair<std::string, unsigned long long>& b) {
                  return a.second > b.second;
              });
    int n = (int)items.size();
    if (n > cfg.flash_top_n) n = cfg.flash_top_n;   // 不足 N 则有几个报几个

    Logger::log(LogLevel::WARN,
        "flash: SNAPSHOT trigger=%s used=%.1f%% thr=%d%% (%lluMB/%lluMB avail=%lluMB) top=%d",
        trigger.path.c_str(), trigger.used_pct, cfg.flash_used_pct_threshold,
        trigger.used_mb, trigger.total_mb, trigger.avail_mb, n);
    // 各分区整体紧张度由周期概览 maybe_log 负责（告警中自动降到 60s），快照只聚焦 top N 子项排名
    Logger::log(LogLevel::WARN, "flash: SNAP rank sizeMB    path");
    for (int i = 0; i < n; i++) {
        Logger::log(LogLevel::WARN, "flash: SNAP %-4d %9.1f %s", i + 1,
                    (double)items[i].second / (1024.0 * 1024.0), items[i].first.c_str());
    }
}

// ---------- 每 tick：采集所有路径 + 去重告警评估 ----------
void sample(const TrackConfig& cfg, long long now) {
    (void)now;
    std::vector<FsInfo> infos;
    for (const auto& p : g_paths) {
        FsInfo fi;
        if (stat_one(p, fi)) infos.push_back(fi);
    }

    // 去重：同一 f_fsid 取第一个路径作为代表（同 fs 的 used% 一致）
    std::map<std::string, const FsInfo*> rep;
    for (const auto& fi : infos) {
        if (rep.find(fi.fsid_key) == rep.end()) rep[fi.fsid_key] = &fi;
    }

    for (const auto& kv : rep) {
        const FsInfo& fi = *kv.second;
        if (fi.read_only) continue;   // 只读分区不告警
        AlertState& st = g_fs_alert[fi.fsid_key];
        bool over = (fi.used_pct > (double)cfg.flash_used_pct_threshold);
        int ch = eval_alert(st, over, fi.used_pct, cfg.flash_alert_duration);
        if (ch & 1) {
            Logger::log(LogLevel::WARN,
                "flash: ALERT path=%s used=%.1f%% thr=%d%% (%lluMB/%lluMB avail=%lluMB)",
                fi.path.c_str(), fi.used_pct, cfg.flash_used_pct_threshold,
                fi.used_mb, fi.total_mb, fi.avail_mb);
            take_snapshot(cfg, fi);
        } else if (ch & 2) {
            Logger::log(LogLevel::INFO,
                "flash: RECOVERED path=%s used=%.1f%% thr=%d%% peak=%.1f%%",
                fi.path.c_str(), fi.used_pct, cfg.flash_used_pct_threshold, st.peak);
        }
    }
}

// ---------- 周期概览日志（正常 flash_log_interval / 告警中 60s）----------
void maybe_log(const TrackConfig& cfg, long long now) {
    long long interval = any_alert_active() ? 60LL * 1000 : (long long)cfg.flash_log_interval * 1000;
    if (now - g_last_log_ms < interval) return;
    g_last_log_ms = now;

    for (const auto& p : g_paths) {
        FsInfo fi;
        if (stat_one(p, fi)) {
            Logger::log(LogLevel::INFO,
                "flash: %s used=%.1f%% (%lluMB/%lluMB avail=%lluMB)%s",
                fi.path.c_str(), fi.used_pct,
                fi.used_mb, fi.total_mb, fi.avail_mb,
                fi.read_only ? " [read-only]" : "");
        } else {
            Logger::log(LogLevel::WARN, "flash: %s statvfs failed", p.c_str());
        }
    }
}

} // namespace

void flash_monitor_tick(const TrackConfig& cfg) {
    if (!cfg.flash_monitor_enable) return;
    long long now = now_ms();
    if (!g_inited) {
        g_inited = true;
        g_paths = split_ws(cfg.flash_paths);
        g_last_log_ms = now;
        // 启动行：列出各路径初始状态 + 只读标注
        for (const auto& p : g_paths) {
            FsInfo fi;
            if (stat_one(p, fi)) {
                Logger::log(LogLevel::INFO,
                    "flash: %s used=%.1f%% (%lluMB/%lluMB avail=%lluMB) thr=%d%% debounce=%d%s",
                    fi.path.c_str(), fi.used_pct, fi.used_mb, fi.total_mb, fi.avail_mb,
                    cfg.flash_used_pct_threshold, cfg.flash_alert_duration,
                    fi.read_only ? " [read-only, alerting disabled]" : "");
            } else {
                Logger::log(LogLevel::WARN, "flash: %s statvfs failed (skipped)", p.c_str());
            }
        }
        if (g_paths.empty()) {
            Logger::log(LogLevel::WARN, "flash: no flash_paths configured");
        }
        return;
    }
    sample(cfg, now);
    maybe_log(cfg, now);
}
