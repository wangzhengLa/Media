// 内核日志采集：把 /proc/kmsg 的内容原样写入 log_dir/dmesg.log，带轮转。
#include "dmesg_monitor.h"
#include "logger.h"
#include "monitor_common.h"
#include "rotating_file.h"

#include <string>
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr const char* KMSG_PATH  = "/proc/kmsg";
constexpr size_t      READ_BUF    = 4096;
constexpr size_t      MAX_PENDING = 65536; // 单行超长保护：超过则强制 flush，防内存膨胀
constexpr long long   kRetryIntervalMs = 30000; // 退避间隔 30s

// ---------- 状态（模块私有，收拢所有 file-scope 变量）----------
struct State {
    int          kmsg_fd = -1;
    bool         inited = false;
    bool         failed = false;   // 初始化失败后退避重试（不永久放弃）
    long long    retry_at_ms = 0;  // 失败后下次重试的单调时间（now_ms()）
    RotatingFile dmesg_file;
    std::string  pending;          // 不完整行的拼接缓冲
};
State state;

// 把 state.pending 中完整的行（以 '\n' 结尾）写入 dmesg.log，保留末尾不完整部分。
void flush_complete_lines() {
    if (state.pending.empty()) return;

    size_t nl = state.pending.find_last_of('\n');
    if (nl == std::string::npos) {
        // 还没有完整行；超长则强制 flush 防止内存膨胀
        if (state.pending.size() > MAX_PENDING) {
            state.dmesg_file.write(state.pending.data(), state.pending.size());
            state.pending.clear();
        }
        return;
    }
    size_t end = nl + 1; // 包含 '\n'
    state.dmesg_file.write(state.pending.data(), end);
    state.pending.erase(0, end);
}

// 循环非阻塞读 kmsg 直到 EAGAIN，把读到的数据并入 state.pending 并 flush 整行。
void drain_kmsg() {
    char buf[READ_BUF];
    while (true) {
        ssize_t n = ::read(state.kmsg_fd, buf, sizeof(buf));
        if (n > 0) {
            state.pending.append(buf, (size_t)n);
            flush_complete_lines();
            continue;
        }
        if (n == 0) {
            break; // EOF（/proc/kmsg 罕见，保险退出）
        }
        // n < 0
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        Logger::log(LogLevel::WARN, "dmesg: read %s failed, errno=%d", KMSG_PATH, errno);
        break;
    }
}

bool init_once(const TrackConfig& cfg) {
    state.kmsg_fd = ::open(KMSG_PATH, O_RDONLY | O_NONBLOCK);
    if (state.kmsg_fd < 0) {
        Logger::log(LogLevel::WARN, "dmesg: cannot open %s, errno=%d", KMSG_PATH, errno);
        return false;
    }

    std::string path = cfg.log_dir + "/dmesg.log";
    if (!state.dmesg_file.open(path, cfg.log_size_kb, cfg.log_count)) {
        Logger::log(LogLevel::WARN, "dmesg: cannot open %s", path.c_str());
        ::close(state.kmsg_fd);
        state.kmsg_fd = -1;
        return false;
    }

    // 首次：dump 当前内核缓冲的全部内容
    drain_kmsg();
    Logger::log(LogLevel::INFO, "dmesg: capturing kernel log to %s", path.c_str());
    return true;
}

} // namespace

void dmesg_monitor_tick(const TrackConfig& cfg) {
    if (state.failed) {
        // 失败后退避重试：距上次失败不足 30s 则跳过；到期后允许重新初始化，
        // 避免 /proc/kmsg 被 klogd 等临时占用后永久放弃采集。
        if (now_ms() < state.retry_at_ms) return;
        state.failed = false;
    }
    if (!state.inited) {
        state.inited = true;
        if (!init_once(cfg)) {
            state.inited = false;            // 失败则允许下次重试重新初始化
            state.failed = true;
            state.retry_at_ms = now_ms() + kRetryIntervalMs;
            return;
        }
    }
    drain_kmsg();
    // 主动轮转：dmesg 写入稀疏（仅内核有新消息时写），超阈值后若内核安静，
    // 仅靠 write 入口的检查永远不会触发；这里每个 tick 兜底检查一次。
    state.dmesg_file.rotate_if_needed();
}

void dmesg_monitor_close() {
    // 收尾：把剩余不完整行也落盘（进程退出，不再等待换行）
    if (!state.pending.empty() && state.dmesg_file.is_open()) {
        state.dmesg_file.write(state.pending.data(), state.pending.size());
        state.pending.clear();
    }
    if (state.kmsg_fd >= 0) {
        ::close(state.kmsg_fd);
        state.kmsg_fd = -1;
    }
    state.dmesg_file.close();
    state.inited = false;
    state.failed = false;
    state.retry_at_ms = 0;
}
