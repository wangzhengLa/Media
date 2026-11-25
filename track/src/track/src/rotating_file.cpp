#include "rotating_file.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

RotatingFile::RotatingFile()
    : fd_(-1), current_size_(0), max_size_(0), keep_count_(1) {}

RotatingFile::~RotatingFile() {
    close();
}

bool RotatingFile::open(const std::string& filepath, int max_size_kb, int keep_count) {
    filepath_ = filepath;
    max_size_ = (max_size_kb > 0) ? (long)max_size_kb * 1024 : 0;
    keep_count_ = keep_count;

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

void RotatingFile::rotate_if_needed() {
    if (max_size_ <= 0 || current_size_ < max_size_) return;
    if (keep_count_ < 1) return;

    ::close(fd_);
    fd_ = -1;

    // 删除最旧的 <name>.<keep_count-1>
    char oldest[512];
    snprintf(oldest, sizeof(oldest), "%s.%d", filepath_.c_str(), keep_count_ - 1);
    unlink(oldest);

    // 轮转: .N-2 -> .N-1, ..., .1 -> .2
    for (int i = keep_count_ - 2; i >= 1; --i) {
        char src[512], dst[512];
        snprintf(src, sizeof(src), "%s.%d", filepath_.c_str(), i);
        snprintf(dst, sizeof(dst), "%s.%d", filepath_.c_str(), i + 1);
        rename(src, dst);
    }
    // 当前 -> .1
    char dst[512];
    snprintf(dst, sizeof(dst), "%s.1", filepath_.c_str());
    rename(filepath_.c_str(), dst);

    // 新建当前文件
    fd_ = ::open(filepath_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    current_size_ = 0;
}

size_t RotatingFile::write(const char* data, size_t len) {
    if (fd_ < 0 || data == nullptr || len == 0) return 0;
    rotate_if_needed();
    ssize_t n = ::write(fd_, data, len);
    if (n > 0) current_size_ += n;
    return (n > 0) ? (size_t)n : 0;
}

void RotatingFile::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    current_size_ = 0;
}
