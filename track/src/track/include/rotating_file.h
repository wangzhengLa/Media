#ifndef TRACK_ROTATING_FILE_H
#define TRACK_ROTATING_FILE_H

#include <string>
#include <cstddef>

// 带轮转的追加写文件：单文件超过 max_size_kb 时按 keep_count 份滚动覆盖。
// 轮转规则：
//   例如 keep_count=5，文件为 <name>、<name>.1 ... <name>.4；
//   超限时删除最旧 <name>.4，依次 .3->.4 .2->.3 .1->.2，当前 <name>->.1，新建空 <name>。
// 非线程安全（调用方在单线程 tick 循环中使用）。
class RotatingFile {
public:
    RotatingFile();
    ~RotatingFile();

    // 打开 filepath（追加模式）。max_size_kb <= 0 时不轮转，只追加。
    bool open(const std::string& filepath, int max_size_kb, int keep_count);
    // 写入原始字节（写入前自动检查并执行轮转）。返回实际写入字节数。
    size_t write(const char* data, size_t len);
    void close();
    bool is_open() const { return fd_ >= 0; }

private:
    void rotate_if_needed();

    int         fd_;
    long        current_size_;
    long        max_size_;     // 字节；<=0 表示不轮转
    int         keep_count_;
    std::string filepath_;
};

#endif // TRACK_ROTATING_FILE_H
