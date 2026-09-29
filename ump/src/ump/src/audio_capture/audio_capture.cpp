#include "audio_capture/audio_capture.h"
#include <algorithm> 
#include <cstring>
#include <unistd.h>
#include <chrono>
#include <ctime>

// ========== 日志宏（仅类内部使用，通过this调用实例日志函数） ==========
#define LOG_ERROR(msg) this->LogInternal("ERROR", __FILE__, __LINE__, msg)
#define LOG_INFO(msg)  this->LogInternal("INFO",  __FILE__, __LINE__, msg)
#define LOG_DEBUG(msg) this->LogInternal("DEBUG", __FILE__, __LINE__, msg)

// ========== 构造/析构 ==========
AlsaRecorder::AlsaRecorder() {
    ring_buffer_.clear();
}

AlsaRecorder::~AlsaRecorder() {
    Stop();
    CloseDevice();
}

// ========== 内部日志函数（实例级，线程安全） ==========
void AlsaRecorder::LogInternal(const char* level, const char* file, int line, const std::string& msg) {
    std::lock_guard<std::mutex> lock(log_mtx_);

    FILE* f = log_file_;
    if (!f) return;

    int cur_level = log_level_;
    int msg_level = (strcmp(level, "ERROR") == 0) ? 0 :
                    (strcmp(level, "INFO") == 0)  ? 1 : 2;
    if (msg_level > cur_level) return;

    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_now);

    std::string log_line = std::string("[") + time_buf + "] [" + level + "] " +
                           file + ":" + std::to_string(line) + " " + msg + "\n";

    fwrite(log_line.c_str(), 1, log_line.size(), f);
    if (strcmp(level, "ERROR") == 0) fflush(f);
}

// ========== 日志配置实现 ==========
void AlsaRecorder::SetLogFile(FILE* file) {
    std::lock_guard<std::mutex> lock(log_mtx_);
    log_file_ = file;
    if (file) setvbuf(file, nullptr, _IOLBF, 0);
}

void AlsaRecorder::SetLogLevel(int level) {
    std::lock_guard<std::mutex> lock(log_mtx_);
    log_level_ = level;
}

FILE* AlsaRecorder::GetLogFile() const { return log_file_; }
int AlsaRecorder::GetLogLevel() const { return log_level_; }

void AlsaRecorder::SetError(const std::string& msg) {
    ErrorCallback cb;
    {
        std::lock_guard<std::recursive_mutex> lock(mtx_);
        last_error_ = msg;
        state_ = RecorderState::Error;
        writer_running_ = false;  // 新增：停止写线程
        cb = error_callback_;
    }
    queue_cv_.notify_all();  // 新增：唤醒写线程，让其退出
    LOG_ERROR(msg);
    if (cb) cb(msg);
}

void AlsaRecorder::ClearBuffer() {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    if (state_ == RecorderState::Recording) {
        LOG_ERROR("Cannot clear buffer while recording.");
        return;
    }
    write_idx_ = 0;
    read_idx_ = 0;
    total_frames_ = 0;
    dropped_frames_ = 0;
    overrun_count_ = 0;
    dropped_since_log_ = 0;
}

int AlsaRecorder::GetCaptureDelayMs() const {
    if (!handle_) return -1;
    snd_pcm_sframes_t delay_frames;
    if (snd_pcm_delay(handle_, &delay_frames) < 0) return -1;
    if (delay_frames < 0) return 0;
    return (int)((double)delay_frames * 1000.0 / sample_rate_);
}

// ========== 初始化（错误状态自动重置 + 队列容量校验） ==========
bool AlsaRecorder::Init(const char* device, int sample_rate, int channels,
                        int frame_ms, size_t queue_capacity) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    if (state_ == RecorderState::Error) {
        LOG_INFO("Auto-resetting from error state for re-initialization.");
        // 先停止并回收旧线程，确保不再访问设备
        writer_running_ = false;
        queue_cv_.notify_all();
        if (writer_thread_.joinable()) writer_thread_.join();
        if (rec_thread_.joinable()) rec_thread_.join();
        // 再安全关闭设备
        if (handle_) {
            snd_pcm_close(handle_);
            handle_ = nullptr;
        }
        state_ = RecorderState::Idle;
    }

    if (state_ != RecorderState::Idle) {
        SetError("Already initialized or recording.");
        return false;
    }

    // 队列容量合法性校验，防止除零崩溃
    if (queue_capacity < 2) {
        SetError("Queue capacity must be at least 2.");
        return false;
    }

    sample_rate_ = sample_rate;
    channels_ = channels;
    frame_ms_ = frame_ms;
    queue_capacity_ = queue_capacity;

    frames_per_period_ = sample_rate_ * frame_ms_ / 1000;
    if (frames_per_period_ == 0) {
        SetError("Invalid frame_ms or sample_rate, frames_per_period is zero.");
        return false;
    }

    period_samples_ = frames_per_period_ * channels_;
    buffer_.resize(period_samples_);

    LOG_INFO(std::string("Opening ALSA capture device: ") + device);

    int err = snd_pcm_open(&handle_, device, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
    if (err < 0) {
        SetError(std::string("snd_pcm_open failed: ") + snd_strerror(err));
        return false;
    }

    snd_pcm_hw_params_t* hw_params = nullptr;
    snd_pcm_hw_params_alloca(&hw_params);
    unsigned int actual_rate = sample_rate_;
    snd_pcm_uframes_t period_size = frames_per_period_;

    err = snd_pcm_hw_params_any(handle_, hw_params);          if (err < 0) goto error;
    err = snd_pcm_hw_params_set_access(handle_, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED); if (err < 0) goto error;
    err = snd_pcm_hw_params_set_format(handle_, hw_params, SND_PCM_FORMAT_S16_LE); if (err < 0) goto error;
    err = snd_pcm_hw_params_set_channels(handle_, hw_params, channels_); if (err < 0) goto error;
    err = snd_pcm_hw_params_set_rate_near(handle_, hw_params, &actual_rate, nullptr); if (err < 0) goto error;

    if (actual_rate != (unsigned int)sample_rate_) {
        LOG_INFO("Sample rate adjusted from " + std::to_string(sample_rate_) + " to " + std::to_string(actual_rate));
        sample_rate_ = actual_rate;
        frames_per_period_ = sample_rate_ * frame_ms_ / 1000;
        period_samples_ = frames_per_period_ * channels_;
        buffer_.resize(period_samples_);
        period_size = frames_per_period_;
    }

    err = snd_pcm_hw_params_set_period_size_near(handle_, hw_params, &period_size, nullptr); if (err < 0) goto error;

    if (period_size != frames_per_period_) {
        LOG_INFO("Period size adjusted from " + std::to_string(frames_per_period_) + " to " + std::to_string(period_size));
        frames_per_period_ = period_size;
        period_samples_ = frames_per_period_ * channels_;
        buffer_.resize(period_samples_);
    }

    err = snd_pcm_hw_params(handle_, hw_params);
    if (err < 0) goto error;

    // 预分配所有帧槽内存，运行时零分配
    ring_buffer_.clear();
    ring_buffer_.resize(queue_capacity_);
    for (auto& slot : ring_buffer_) {
        slot.data.resize(period_samples_);
        slot.valid_samples = 0;
    }

    // 重置所有统计
    ClearBuffer();

    state_ = RecorderState::Initialized;
    LOG_INFO("ALSA recorder initialized. Device: " + std::string(device) +
             ", Rate: " + std::to_string(sample_rate_) +
             ", Channels: " + std::to_string(channels_) +
             ", Period frames: " + std::to_string(frames_per_period_) +
             ", Queue capacity: " + std::to_string(queue_capacity_));
    return true;

error:
    snd_pcm_close(handle_);
    handle_ = nullptr;
    SetError(std::string("ALSA setup error: ") + snd_strerror(err));
    state_ = RecorderState::Idle;  // 失败后回滚到空闲状态，允许重试
    return false;
}

// ========== 回调注册 ==========
void AlsaRecorder::RegisterCallback(AudioCallback cb) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    audio_callback_ = std::move(cb);
}

void AlsaRecorder::RegisterErrorCallback(ErrorCallback cb) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    error_callback_ = std::move(cb);
}

// ========== 环形缓冲区操作（短帧处理 + 丢帧日志节流） ==========
bool AlsaRecorder::PushData(const int16_t* data, size_t num_samples) {
    size_t w = write_idx_.load(std::memory_order_relaxed);
    size_t r = read_idx_.load(std::memory_order_acquire);
    size_t next_w = (w + 1) % queue_capacity_;

    if (next_w == r) {
        dropped_frames_++;
        // 丢帧日志节流，每100帧打印一次，避免高负载下日志风暴
        size_t cnt = ++dropped_since_log_;
        if (cnt >= 100) {
            LOG_ERROR("Ring buffer full! Cumulative dropped " + std::to_string(cnt) + " frames.");
            dropped_since_log_ = 0;
        }
        return false;
    }

    FrameSlot& slot = ring_buffer_[w];
    size_t copy_samples = std::min(num_samples, slot.data.size());
    memcpy(slot.data.data(), data, copy_samples * sizeof(int16_t));
    slot.valid_samples = copy_samples;  // 记录实际有效长度

    write_idx_.store(next_w, std::memory_order_release);
    total_frames_++;
    return true;
}

bool AlsaRecorder::PopData(FrameSlot& out) {
    size_t r = read_idx_.load(std::memory_order_relaxed);
    size_t w = write_idx_.load(std::memory_order_acquire);

    if (r == w) return false;

    FrameSlot& slot = ring_buffer_[r];
    out.valid_samples = slot.valid_samples;
    if (out.data.size() < slot.valid_samples) out.data.resize(slot.valid_samples);
    memcpy(out.data.data(), slot.data.data(), slot.valid_samples * sizeof(int16_t));

    read_idx_.store((r + 1) % queue_capacity_, std::memory_order_release);
    return true;
}

// ========== 运行统计接口实现 ==========
size_t AlsaRecorder::GetTotalFrames() const { return total_frames_.load(); }
size_t AlsaRecorder::GetDroppedFrames() const { return dropped_frames_.load(); }
size_t AlsaRecorder::GetOverrunCount() const { return overrun_count_.load(); }

double AlsaRecorder::GetDropRate() const {
    size_t total = total_frames_.load();
    return total == 0 ? 0.0 : static_cast<double>(dropped_frames_.load()) / total;
}

// ========== 线程安全的错误信息获取 ==========
std::string AlsaRecorder::GetLastError() const {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    return last_error_;
}

// ========== 启动录音（时序优化：软件就绪后再启动硬件，避免首帧丢失） ==========
bool AlsaRecorder::Start() {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    // 新增：先回收之前的线程句柄，防止覆盖崩溃
    if (writer_thread_.joinable()) writer_thread_.join();
    if (rec_thread_.joinable()) rec_thread_.join();

    if (state_ == RecorderState::Recording) {
        SetError("Already recording.");
        return false;
    }
    if (state_ != RecorderState::Initialized) {
        SetError("Not initialized or in error state.");
        return false;
    }

    // 先重置设备状态
    int err = snd_pcm_drop(handle_);
    if (err < 0) {
        LOG_ERROR(std::string("snd_pcm_drop warning: ") + snd_strerror(err));
    }

    err = snd_pcm_prepare(handle_);
    if (err < 0) {
        SetError(std::string("snd_pcm_prepare failed: ") + snd_strerror(err));
        return false;
    }

    // 中优先级修复：先重置队列、状态、创建线程，全部就绪后再启动硬件
    ClearBuffer();

    state_ = RecorderState::Recording;
    writer_running_ = true;

    // 先启动线程（软件侧准备完成）
    writer_thread_ = std::thread(&AlsaRecorder::WriterThreadFunc, this);
    rec_thread_ = std::thread(&AlsaRecorder::RecThreadFunc, this);

    // 最后启动硬件采集，确保第一帧数据不会丢失
    err = snd_pcm_start(handle_);
    if (err < 0) {
        // 启动失败：回滚所有状态
        state_ = RecorderState::Initialized;
        writer_running_ = false;
        queue_cv_.notify_all();

        if (writer_thread_.joinable()) writer_thread_.join();
        if (rec_thread_.joinable()) rec_thread_.join();

        snd_pcm_drop(handle_);

        SetError(std::string("snd_pcm_start failed: ") + snd_strerror(err));
        return false;
    }

    LOG_INFO("Recording started.");
    return true;
}

// ========== 停止录音（三阶段无死锁 + 保留错误状态） ==========
void AlsaRecorder::Stop() {
    // 1. 锁内仅设置停止标志
    {
        std::lock_guard<std::recursive_mutex> lock(mtx_);
        writer_running_ = false;
        if (state_ == RecorderState::Recording) {
            state_ = RecorderState::Initialized;
        }
    }

    // 2. 锁外通知+等待线程退出（彻底避免死锁）
    queue_cv_.notify_all();
    if (writer_thread_.joinable()) writer_thread_.join();
    if (rec_thread_.joinable()) rec_thread_.join();

    // 3. 锁内清理设备，保留Error状态不强制重置
    {
        std::lock_guard<std::recursive_mutex> lock(mtx_);
        if (handle_) {
            snd_pcm_drop(handle_);
            LOG_INFO("PCM dropped.");
        }
    }

    LOG_INFO("Recording stopped. Total frames: " + std::to_string(total_frames_.load()) +
             ", Dropped: " + std::to_string(dropped_frames_.load()) +
             ", Overruns: " + std::to_string(overrun_count_.load()));
}

// ========== 关闭设备（先停止线程再关闭，杜绝野指针） ==========
void AlsaRecorder::CloseDevice() {
    Stop();  // 先确保所有线程停止
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    if (handle_) {
        snd_pcm_close(handle_);
        handle_ = nullptr;
        LOG_INFO("ALSA device closed.");
    }
    state_ = RecorderState::Idle;
}

// ========== 录音线程（Overrun恢复优化 + 超时退出检查） ==========
void AlsaRecorder::RecThreadFunc() {
    LOG_INFO("Recording thread started.");

    while (state_ == RecorderState::Recording) {
        int wait_ret = snd_pcm_wait(handle_, 10);
        if (wait_ret < 0) {
            LOG_ERROR("snd_pcm_wait error: " + std::string(snd_strerror(wait_ret)));
            // 尝试恢复：drop + prepare + start
            snd_pcm_drop(handle_);
            int err = snd_pcm_prepare(handle_);
            if (err < 0) {
                SetError(std::string("Wait recovery prepare failed: ") + snd_strerror(err));
                break;
            }
            err = snd_pcm_start(handle_);
            if (err < 0) {
                SetError(std::string("Wait recovery start failed: ") + snd_strerror(err));
                break;
            }
            continue;
        } else if (wait_ret == 0) {
            // 超时后检查退出状态
            if (state_ != RecorderState::Recording) break;
            continue;
        }

        // 读取数据
        snd_pcm_sframes_t frames = snd_pcm_readi(handle_, buffer_.data(), frames_per_period_);
        if (frames < 0) {
            if (frames == -EPIPE) {
                overrun_count_++;
                LOG_ERROR("Overrun occurred. Recovering...");
                snd_pcm_drop(handle_);
                int err = snd_pcm_prepare(handle_);
                if (err < 0) {
                    SetError(std::string("Overrun recovery prepare failed: ") + snd_strerror(err));
                    break;
                }
                err = snd_pcm_start(handle_);
                if (err < 0) {
                    SetError(std::string("Overrun recovery start failed: ") + snd_strerror(err));
                    break;
                }
                continue;
            } else {
                LOG_ERROR("snd_pcm_readi error: " + std::string(snd_strerror(frames)));
                snd_pcm_drop(handle_);
                int err = snd_pcm_prepare(handle_);
                if (err < 0) {
                    SetError(std::string("Read recovery prepare failed: ") + snd_strerror(err));
                    break;
                }
                err = snd_pcm_start(handle_);
                if (err < 0) {
                    SetError(std::string("Read recovery start failed: ") + snd_strerror(err));
                    break;
                }
                continue;
            }
        } else if (frames > 0) {
            size_t num_samples = frames * channels_;
            PushData(buffer_.data(), num_samples);
            queue_cv_.notify_one();
        }
    }

    if (state_ == RecorderState::Recording) {
        SetError("Recording thread exited unexpectedly.");
    }
    LOG_INFO("Recording thread exited.");
}

// ========== 写回调线程（回调并发安全） ==========
void AlsaRecorder::WriterThreadFunc() {
    LOG_INFO("Writer thread started.");
    FrameSlot slot;

    while (writer_running_ || (read_idx_.load() != write_idx_.load())) {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_cv_.wait(lock, [this] {
            return !writer_running_ || (read_idx_.load() != write_idx_.load());
        });
        lock.unlock();

        while (PopData(slot)) {
            // 锁内拷贝回调对象，锁外执行，杜绝并发读写崩溃
            AudioCallback cb;
            {
                std::lock_guard<std::recursive_mutex> cb_lock(mtx_);
                cb = audio_callback_;
            }
            if (cb) {
                size_t bytes = slot.valid_samples * sizeof(int16_t);
                cb(slot.data.data(), bytes, sample_rate_, channels_);
            }
        }
    }
    LOG_INFO("Writer thread exited.");
}
