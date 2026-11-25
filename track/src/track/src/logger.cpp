#include "logger.h"
#include "rotating_file.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {
// track.log 的轮转文件实例（函数内静态，避免跨 TU 初始化顺序问题）
RotatingFile& track_file() {
    static RotatingFile f;
    return f;
}
} // namespace

bool Logger::init(const std::string& log_dir, int file_size_kb, int keep_count) {
    // 确保目录存在
    mkdir(log_dir.c_str(), 0755);

    std::string path = log_dir + "/track.log";
    return track_file().open(path, file_size_kb, keep_count);
}

void Logger::log(LogLevel level, const char* fmt, ...) {
    // 时间戳
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    char buf[4096];
    int pos = snprintf(buf, sizeof(buf), "[%04d-%02d-%02d %02d:%02d:%02d.%03ld] [%s]  ",
                       tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                       tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000,
                       level == LogLevel::INFO ? "INFO" :
                       level == LogLevel::WARN ? "WARN" : "ERROR");

    va_list ap;
    va_start(ap, fmt);
    pos += vsnprintf(buf + pos, sizeof(buf) - pos, fmt, ap);
    va_end(ap);

    if (pos < (int)sizeof(buf) - 1) {
        buf[pos++] = '\n';
    } else {
        buf[sizeof(buf) - 2] = '\n';
        buf[sizeof(buf) - 1] = '\0';
        pos = sizeof(buf) - 1;
    }

    track_file().write(buf, (size_t)pos);
}

void Logger::close() {
    track_file().close();
}
