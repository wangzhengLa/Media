#include "rotating_file.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>

// 压缩单个文件：gzip -f <path> → <path>.gz，成功后源文件由 gzip 自动删除。
// 两级回退：先找 gzip；找不到则 busybox gzip（busybox 内置 CONFIG_GZIP，必然可用）。
// 都失败返回 false，调用方保留明文备份，不影响日志写入。
static bool gzip_in_place(const std::string& path) {
    pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
        int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, 0);
            ::dup2(devnull, 1);
            ::dup2(devnull, 2);
            if (devnull > 2) ::close(devnull);
        }
        ::execlp("gzip", "gzip", "-f", path.c_str(), static_cast<char*>(nullptr));
        ::execlp("busybox", "busybox", "gzip", "-f", path.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

RotatingFile::RotatingFile()
    : fd_(-1), current_size_(0), max_size_(0), keep_count_(1) {}

RotatingFile::~RotatingFile() {
    close();
}

bool RotatingFile::open(const std::string& filepath, int max_size_kb, int keep_count) {
    // 幂等：fork 后子进程会再次 init，需先关掉继承来的旧 fd
    close();

    filepath_ = filepath;
    max_size_ = (max_size_kb > 0) ? (long)max_size_kb * 1024 : 0;
    keep_count_ = keep_count;

    if (!reopen()) return false;
    // 打开时若已有文件已超阈值（如上次运行遗留、或配置调小），当场轮转，不依赖后续写入触发
    rotate_if_needed();
    return true;
}

bool RotatingFile::reopen() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    fd_ = ::open(filepath_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd_ < 0) return false;

    struct stat st;
    if (fstat(fd_, &st) == 0) {
        current_size_ = st.st_size;
    } else {
        current_size_ = 0;
    }
    return true;
}

bool RotatingFile::ensure_fd_current() {
    // 磁盘路径当前的 inode
    struct stat st_path;
    int path_ok = (::stat(filepath_.c_str(), &st_path) == 0);

    if (fd_ < 0) return reopen();

    struct stat st_fd;
    if (::fstat(fd_, &st_fd) != 0) return reopen();

    // fd 与路径 inode 不一致（或路径已不存在）说明对方进程已轮转，重开自愈
    if (!path_ok || st_fd.st_ino != st_path.st_ino || st_fd.st_dev != st_path.st_dev) {
        return reopen();
    }

    // 一致：用 fd 真实大小刷新 current_size_，兼容对方进程的写入
    current_size_ = st_fd.st_size;
    return true;
}

void RotatingFile::rotate_if_needed() {
    if (fd_ < 0) return;
    if (max_size_ <= 0 || current_size_ < max_size_) return;
    if (keep_count_ < 1) return;

    ::close(fd_);
    fd_ = -1;

    // 删除最旧备份：兼容 .gz（压缩后）与无后缀（升级前残留的明文）
    char oldest_gz[512], oldest_raw[512];
    snprintf(oldest_gz, sizeof(oldest_gz), "%s.%d.gz", filepath_.c_str(), keep_count_ - 1);
    snprintf(oldest_raw, sizeof(oldest_raw), "%s.%d", filepath_.c_str(), keep_count_ - 1);
    unlink(oldest_gz);
    unlink(oldest_raw);

    // 移位: .i.gz -> .i+1.gz；若 .i.gz 不存在则尝试残留明文 .i -> .i+1（一次性迁移）
    for (int i = keep_count_ - 2; i >= 1; --i) {
        char src_gz[512], dst_gz[512], src_raw[512], dst_raw[512];
        snprintf(src_gz, sizeof(src_gz), "%s.%d.gz", filepath_.c_str(), i);
        snprintf(dst_gz, sizeof(dst_gz), "%s.%d.gz", filepath_.c_str(), i + 1);
        snprintf(src_raw, sizeof(src_raw), "%s.%d", filepath_.c_str(), i);
        snprintf(dst_raw, sizeof(dst_raw), "%s.%d", filepath_.c_str(), i + 1);
        if (rename(src_gz, dst_gz) != 0) {
            rename(src_raw, dst_raw);
        }
    }
    // 当前 -> .1
    char dst[512];
    snprintf(dst, sizeof(dst), "%s.1", filepath_.c_str());
    rename(filepath_.c_str(), dst);

    // 新建当前文件（O_APPEND 保证即使 current_size_ 漂移也写在末尾）
    fd_ = ::open(filepath_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    current_size_ = 0;

    // 压缩刚轮转出来的 .1 -> .1.gz；失败则保留明文 .1（不丢数据）
    gzip_in_place(dst);
}

size_t RotatingFile::write(const char* data, size_t len) {
    if (data == nullptr || len == 0) return 0;
    // 写前自愈：对方进程轮转后本进程 fd 会指向旧 inode，此处重开并刷新大小
    if (!ensure_fd_current()) return 0;
    rotate_if_needed();
    if (fd_ < 0) return 0;
    ssize_t n = ::write(fd_, data, len);
    if (n > 0) {
        current_size_ += n;
    } else {
        // 写入失败时按 fd 真实大小回算，避免 current_size_ 漂移
        struct stat st;
        if (::fstat(fd_, &st) == 0) current_size_ = st.st_size;
    }
    return (n > 0) ? (size_t)n : 0;
}

void RotatingFile::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    current_size_ = 0;
}
