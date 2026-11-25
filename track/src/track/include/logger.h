#ifndef TRACK_LOGGER_H
#define TRACK_LOGGER_H

#include <string>

// 日志级别
enum class LogLevel {
    INFO,
    WARN,
    ERROR
};

// 日志器（带轮转）。写 log_dir/track.log，轮转逻辑由 RotatingFile 提供。
class Logger {
public:
    // 初始化日志，指定单文件大小(KB)和保留份数
    static bool init(const std::string& log_dir, int file_size_kb, int keep_count);
    static void log(LogLevel level, const char* fmt, ...);
    static void close();
};

#endif // TRACK_LOGGER_H
