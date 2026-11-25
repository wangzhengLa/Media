// 内核日志采集：把 /proc/kmsg 的内容原样写入 log_dir/dmesg.log，带轮转。
// 设计要点：
//   - /proc/kmsg 用 O_NONBLOCK 打开，每 tick 循环 read 到 EAGAIN，拿到的就是自上次以来的增量
//     （内核全局消费游标天然保证增量，无需自己记录偏移）。
//   - 首次调用先把当前缓冲全部 dump 进去（开机以来的日志）。
//   - 非阻塞 read 可能返回半行/多行，故维护拼接缓冲，只把以 '\n' 结尾的完整行写入文件，
//     避免在轮转边界处把一行拆成两段。
//   - 轮转复用 RotatingFile，参数取 track.conf 的 log_size / log_count，与 track.log 同规则。

#include "dmesg_monitor.h"
#include "logger.h"
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

int          g_kmsg_fd   = -1;
bool         g_inited    = false;
bool         g_failed    = false;   // 初始化失败后不再重试
RotatingFile g_dmesg_file;
std::string  g_pending;            // 不完整行的拼接缓冲

// 把 g_pending 中完整的行（以 '\n' 结尾）写入 dmesg.log，保留末尾不完整部分。
void flush_complete_lines() {
    if (g_pending.empty()) return;

    size_t nl = g_pending.find_last_of('\n');
    if (nl == std::string::npos) {
        // 还没有完整行；超长则强制 flush 防止内存膨胀
        if (g_pending.size() > MAX_PENDING) {
            g_dmesg_file.write(g_pending.data(), g_pending.size());
            g_pending.clear();
        }
        return;
    }
    size_t end = nl + 1; // 包含 '\n'
    g_dmesg_file.write(g_pending.data(), end);
    g_pending.erase(0, end);
}

// 循环非阻塞读 kmsg 直到 EAGAIN，把读到的数据并入 g_pending 并 flush 整行。
void drain_kmsg() {
    char buf[READ_BUF];
    while (true) {
        ssize_t n = ::read(g_kmsg_fd, buf, sizeof(buf));
        if (n > 0) {
            g_pending.append(buf, (size_t)n);
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
    g_kmsg_fd = ::open(KMSG_PATH, O_RDONLY | O_NONBLOCK);
    if (g_kmsg_fd < 0) {
        Logger::log(LogLevel::WARN, "dmesg: cannot open %s, errno=%d", KMSG_PATH, errno);
        return false;
    }

    std::string path = cfg.log_dir + "/dmesg.log";
    if (!g_dmesg_file.open(path, cfg.log_size_kb, cfg.log_count)) {
        Logger::log(LogLevel::WARN, "dmesg: cannot open %s", path.c_str());
        ::close(g_kmsg_fd);
        g_kmsg_fd = -1;
        return false;
    }

    // 首次：dump 当前内核缓冲的全部内容
    drain_kmsg();
    Logger::log(LogLevel::INFO, "dmesg: capturing kernel log to %s", path.c_str());
    return true;
}

} // namespace

void dmesg_monitor_tick(const TrackConfig& cfg) {
    if (g_failed) return;
    if (!g_inited) {
        g_inited = true;
        if (!init_once(cfg)) {
            g_failed = true;
            return;
        }
    }
    drain_kmsg();
}

void dmesg_monitor_close() {
    // 收尾：把剩余不完整行也落盘（进程退出，不再等待换行）
    if (!g_pending.empty() && g_dmesg_file.is_open()) {
        g_dmesg_file.write(g_pending.data(), g_pending.size());
        g_pending.clear();
    }
    if (g_kmsg_fd >= 0) {
        ::close(g_kmsg_fd);
        g_kmsg_fd = -1;
    }
    g_dmesg_file.close();
    g_inited = false;
    g_failed = false;
}
