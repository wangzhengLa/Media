
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

// ---------- 状态（模块私有，收拢所有 file-scope 变量）----------
struct State {
    bool inited = false;
    std::vector<std::string> paths;                  // 解析后的监控路径
    std::map<std::string, AlertState> fs_alert;     // fsid_key -> 告警状态（去重后每个唯一 fs 一个）
    std::map<std::string, long long> last_snap_ms;  // fsid_key -> 上次快照时间（每个分区独立冷却）
};
State state;

// ---------- 递归统计路径表观大小（字节），等价于 du -sbx ----------
// 为什么不用 popen("du ...")：设备 rootfs 的 busybox 未启用 CONFIG_TIMEOUT，
constexpr unsigned long long kMaxDuEntries = 20000;

unsigned long long dir_apparent_size(const std::string& path) {
    struct stat root_st;
    if (lstat(path.c_str(), &root_st) != 0) return 0;
    const dev_t root_dev = root_st.st_dev;

    unsigned long long total = root_st.st_size;
    unsigned long long count = 1;

    std::vector<std::string> stack;
    stack.push_back(path);

    while (!stack.empty() && count < kMaxDuEntries) {
        std::string cur = std::move(stack.back());
        stack.pop_back();

        DIR* d = opendir(cur.c_str());
        if (!d) continue;
        struct dirent* de;
        while ((de = readdir(d)) != nullptr && count < kMaxDuEntries) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            std::string child = cur + "/" + de->d_name;
            struct stat cst;
            if (lstat(child.c_str(), &cst) != 0) continue;
            total += cst.st_size;
            count++;
            // 同设备的目录才递归（-x：不跨挂载点）；符号链接到此为止（链接自身已计入）
            if (S_ISDIR(cst.st_mode) && cst.st_dev == root_dev) {
                stack.push_back(std::move(child));
            }
        }
        closedir(d);
    }
    return total;
}

// ---------- 拍快照：统计触发分区挂载目录下一级子项的占用，取前 N（写入主日志，不另建文件）----------
// 只看一级子项（如 /userdata 下的 etc/lib/usr/var...），每个子项递归遍历取表观总大小，
// 按大小降序取前 N，不足 N 则有几个报几个。cooldown 按 f_fsid 独立。
void take_snapshot(const TrackConfig& cfg, const FsInfo& trigger) {
    long long now = now_ms();
    long long& last = state.last_snap_ms[trigger.fsid_key];   // 每个分区独立冷却
    if (now - last < (long long)cfg.snapshot_cooldown * 1000) return;
    last = now;

    struct stat rst;
    bool have_dev = (lstat(trigger.path.c_str(), &rst) == 0);
    std::vector<std::pair<std::string, unsigned long long>> items;  // 一级子项 -> 表观总大小(字节)
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
                items.push_back({path, dir_apparent_size(path)});   // 目录/文件统一取表观大小
            }
            closedir(d);
        }
    }
    std::sort(items.begin(), items.end(),
              [](const std::pair<std::string, unsigned long long>& a,
                 const std::pair<std::string, unsigned long long>& b) {
                  return a.second > b.second;
              });
    int n = clip_top_n((int)items.size(), cfg.snapshot_top_n);   // 不足 N 则有几个报几个

    Logger::log(LogLevel::WARN,
        "flash: SNAPSHOT trigger=%s used=%.1f%% thr=%d%% (%lluMB/%lluMB avail=%lluMB) top=%d",
        trigger.path.c_str(), trigger.used_pct, cfg.flash_used_pct_threshold,
        trigger.used_mb, trigger.total_mb, trigger.avail_mb, n);
    // 快照只聚焦 top N 子项排名（整体紧张度见告警第2行明细）
    Logger::log(LogLevel::WARN, "flash: SNAP rank sizeMB    path");
    for (int i = 0; i < n; i++) {
        Logger::log(LogLevel::WARN, "flash: SNAP %-4d %9.1f %s", i + 1,
                    (double)items[i].second / (1024.0 * 1024.0), items[i].first.c_str());
    }
}

// ---------- 每 tick：采集所有路径 + 去重告警评估 ----------
void sample(const TrackConfig& cfg) {
    std::vector<FsInfo> infos;
    for (const auto& p : state.paths) {
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
        AlertState& st = state.fs_alert[fi.fsid_key];
        bool over = (fi.used_pct > (double)cfg.flash_used_pct_threshold);
        int ch = eval_alert(st, over, fi.used_pct);
        if (ch & 1) {
            // 刚触发告警：第1行提示 → 第2行该分区明细；快照由下方 active 块统一拍
            Logger::log(LogLevel::WARN, "flash: flash usage has exceeded the threshold");
            Logger::log(LogLevel::WARN,
                "flash: path=%s used=%.1f%% thr=%d%% (%lluMB/%lluMB avail=%lluMB)",
                fi.path.c_str(), fi.used_pct, cfg.flash_used_pct_threshold,
                fi.used_mb, fi.total_mb, fi.avail_mb);
        } else if (ch & 2) {
            Logger::log(LogLevel::INFO,
                "flash: RECOVERED path=%s used=%.1f%% thr=%d%% peak=%.1f%%",
                fi.path.c_str(), fi.used_pct, cfg.flash_used_pct_threshold, st.peak);
        }
        // 告警持续期间（含刚触发）按冷却拍快照（cooldown 按 f_fsid 独立）；恢复后 active 为假即停
        if (st.active) {
            take_snapshot(cfg, fi);
        }
    }
}

} // namespace

void flash_monitor_tick(const TrackConfig& cfg) {
    if (!state.inited) {
        state.inited = true;
        state.paths = split_ws(cfg.flash_paths);
        // 启动行：列出各路径初始状态 + 只读标注
        for (const auto& p : state.paths) {
            FsInfo fi;
            if (stat_one(p, fi)) {
                Logger::log(LogLevel::INFO,
                    "flash: %s used=%.1f%% (%lluMB/%lluMB avail=%lluMB) thr=%d%%%s",
                    fi.path.c_str(), fi.used_pct, fi.used_mb, fi.total_mb, fi.avail_mb,
                    cfg.flash_used_pct_threshold,
                    fi.read_only ? " [read-only, alerting disabled]" : "");
            } else {
                Logger::log(LogLevel::WARN, "flash: %s statvfs failed (skipped)", p.c_str());
            }
        }
        if (state.paths.empty()) {
            Logger::log(LogLevel::WARN, "flash: no flash_paths configured");
        }
        return;
    }
    sample(cfg);
}
