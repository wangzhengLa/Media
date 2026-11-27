#ifndef TRACK_ROTATING_FILE_H
#define TRACK_ROTATING_FILE_H

#include <string>
#include <cstddef>

// 带轮转的追加写文件：单文件超过 max_size_kb 时按 keep_count 份滚动覆盖，历史备份自动 gzip 压缩。
// 轮转规则（备份为 .gz，当前文件始终明文以便 tail -f）：
//   例如 keep_count=5，文件为 <name>、<name>.1.gz ... <name>.4.gz；
//   超限时删除最旧 <name>.4.gz，依次 .3.gz->.4.gz ... .1.gz->.2.gz，当前 <name>->.1，
//   新建空 <name> 继续写，再把 .1 压缩为 .1.gz。gzip 失败时保留明文 .1，不丢数据。
// 非线程安全（调用方在单线程 tick 循环中使用）。
class RotatingFile {
public:
    RotatingFile();
    ~RotatingFile();

    // 打开 filepath（追加模式）。max_size_kb <= 0 时不轮转，只追加。
    // 幂等：若已打开会先关闭旧 fd。
    bool open(const std::string& filepath, int max_size_kb, int keep_count);
    // 写入原始字节（写入前自动检查并执行轮转）。返回实际写入字节数。
    size_t write(const char* data, size_t len);
    void close();
    bool is_open() const { return fd_ >= 0; }

    // 主动检查并执行轮转（无需发生写入）。低于阈值时立即返回，零开销。
    // 供稀疏写入方（如 dmesg）每个 tick 调用，保证超阈值文件即使无新写入也能在一个节拍内轮转。
    void rotate_if_needed();

private:
    // 重新以追加模式打开 filepath_，并用 fstat 刷新 current_size_。
    bool reopen();
    // 确保 fd_ 仍指向磁盘上当前路径的 inode（对方进程轮转后自愈），并刷新 current_size_。
    bool ensure_fd_current();

    int         fd_;
    long        current_size_;
    long        max_size_;     // 字节；<=0 表示不轮转
    int         keep_count_;
    std::string filepath_;
};

#endif // TRACK_ROTATING_FILE_H
