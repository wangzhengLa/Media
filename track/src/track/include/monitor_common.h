#ifndef TRACK_MONITOR_COMMON_H
#define TRACK_MONITOR_COMMON_H

// CPU / 内存 / Flash 监控共用的基础设施：
//   - AlertState + eval_alert：对称去抖（告警/恢复同一阈值次数）
//   - read_pid_stat：解析 /proc/<pid>/stat（comm/state/utime/stime/threads/rss）
// 这两个实现从 cpu_monitor.cpp 原样提取，行为保持不变；仅改为 inline 以支持多编译单元共享。

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstddef>

// 告警状态（单个 trigger）
struct AlertState {
    bool active = false;
    int strike = 0;
    int recover = 0;
    double peak = 0;   // 告警期间记录的极值（CPU/Flash 用 max；内存如需 min 请自行维护）
};

// 去抖评估：返回位 0=刚告警, 位 1=刚恢复
inline int eval_alert(AlertState& st, bool over, double val, int duration) {
    int changed = 0;
    if (over) {
        st.recover = 0;
        if (!st.active) {
            st.strike++;
            if (st.strike >= duration) {
                st.active = true;
                st.peak = val;
                changed |= 1;
            }
        } else if (val > st.peak) {
            st.peak = val;
        }
    } else {
        st.strike = 0;
        if (st.active) {
            st.recover++;
            if (st.recover >= duration) {
                st.active = false;
                changed |= 2;
            }
        }
    }
    return changed;
}

// 解析 /proc/<pid>/stat：comm（括号内）、state、utime、stime、num_threads、rss（页）。
// 不需要的字段传 nullptr。
inline bool read_pid_stat(int pid, char* comm, size_t comm_sz,
                          uint64_t& utime, uint64_t& stime,
                          int* num_threads, long* rss_pages,
                          char* state_out = nullptr) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char buf[2048];
    size_t r = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (r == 0) return false;
    buf[r] = '\0';

    // comm 在最后一对括号内
    char* lp = strchr(buf, '(');
    char* rp = strrchr(buf, ')');
    if (!lp || !rp || rp <= lp) return false;
    size_t clen = (size_t)(rp - lp - 1);
    if (clen >= comm_sz) clen = comm_sz - 1;
    memcpy(comm, lp + 1, clen);
    comm[clen] = '\0';

    // rp+1 之后是 state(字段3) 及后续
    char* p = rp + 1;
    char st[2] = {0};
    unsigned long long u = 0, s = 0;
    long nt = 0, rss = 0;
    // 取 state(3)，跳过 ppid..cmajflt 共 10 个字段(4..13) 到 utime(14)
    int matched = sscanf(p,
        "%1s "                                              // 3 state
        "%*s %*s %*s %*s %*s %*s %*s %*s %*s %*s "          // 4..13 (10 个)
        "%llu %llu "                                        // 14 utime, 15 stime
        "%*s %*s %*s %*s "                                  // 16..19 (4 个)
        "%ld "                                              // 20 num_threads
        "%*s %*s %*s "                                      // 21..23 (3 个)
        "%ld",                                              // 24 rss
        st, &u, &s, &nt, &rss);
    if (matched < 2) return false;
    utime = (uint64_t)u;
    stime = (uint64_t)s;
    if (num_threads) *num_threads = (int)nt;
    if (rss_pages) *rss_pages = rss;
    if (state_out) *state_out = st[0];
    return true;
}

#endif // TRACK_MONITOR_COMMON_H
