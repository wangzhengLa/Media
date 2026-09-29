#pragma once
#include <alsa/asoundlib.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <vector>
#include <string>
#include <cstdio>
#include <cstddef>

// ---------- 状态机定义 ----------
enum class RecorderState {
    Idle,          // 未初始化/已释放
    Initialized,   // 已初始化，未启动
    Recording,     // 录音中
    Error          // 发生不可恢复错误
};

// ---------- 回调类型 ----------
using AudioCallback = std::function<void(const int16_t* data, size_t bytes,
                                         int sample_rate, int channels)>;
using ErrorCallback = std::function<void(const std::string& error)>;

class AlsaRecorder {
public:
    AlsaRecorder();
    ~AlsaRecorder();

    // 初始化：设备名、采样率、声道数、帧时长(ms)、环形队列容量(帧数)
    // 支持Error状态直接调用，自动重置设备
    bool Init(const char* device = "default",
              int sample_rate = 16000,
              int channels = 2,
              int frame_ms = 10,
              size_t queue_capacity = 64);

    void RegisterCallback(AudioCallback audio_cb);
    void RegisterErrorCallback(ErrorCallback error_cb);

    bool Start();
    void Stop();
    void CloseDevice();  // 公有接口，支持手动释放设备

    RecorderState GetState() const { return state_.load(); }
    bool IsRecording() const { return state_.load() == RecorderState::Recording; }

    // 线程安全：返回错误信息拷贝，消除并发读写风险
    std::string GetLastError() const;

    // ---------- 运行统计接口 ----------
    size_t GetTotalFrames() const;
    size_t GetDroppedFrames() const;
    size_t GetOverrunCount() const;
    double GetDropRate() const;  // 丢帧百分比

    // ---------- 日志配置（实例级，多实例互不影响） ----------
    void SetLogFile(FILE* file);
    void SetLogLevel(int level);  // 0:ERROR  1:INFO  2:DEBUG
    FILE* GetLogFile() const;
    int GetLogLevel() const;
    void ClearBuffer();
    // 获取当前采集设备的延迟（毫秒），返回 -1 表示无法获取
    int GetCaptureDelayMs() const;

private:
    // ---------- 内部帧槽结构体（类内定义，不污染全局命名空间） ----------
    struct FrameSlot {
        std::vector<int16_t> data;
        size_t valid_samples = 0;  // 实际有效样本数，处理ALSA短帧读取
    };

    // 线程函数
    void RecThreadFunc();
    void WriterThreadFunc();
    void SetError(const std::string& msg);

    // SPSC无锁环形缓冲区操作
    bool PushData(const int16_t* data, size_t num_samples);
    bool PopData(FrameSlot& out);

    // 内部日志函数（实例级）
    void LogInternal(const char* level, const char* file, int line, const std::string& msg);

    // ---------- ALSA设备句柄 ----------
    snd_pcm_t* handle_ = nullptr;

    // ---------- 线程与同步 ----------
    std::thread rec_thread_;
    std::thread writer_thread_;
    std::atomic<RecorderState> state_{RecorderState::Idle};
    std::atomic<bool> writer_running_{false};

    // 修复：改为递归互斥锁，解决锁内调用SetError重入死锁问题
    mutable std::recursive_mutex mtx_;
    std::mutex queue_mutex_;          // 保护条件变量
    std::condition_variable queue_cv_;

    // ---------- 音频参数 ----------
    int sample_rate_ = 16000;
    int channels_ = 2;
    int frame_ms_ = 10;
    snd_pcm_uframes_t frames_per_period_ = 0;
    size_t period_samples_ = 0;
    std::vector<int16_t> buffer_;

    // ---------- 回调 ----------
    AudioCallback audio_callback_;
    ErrorCallback error_callback_;
    std::string last_error_;

    // ---------- 环形缓冲区 ----------
    size_t queue_capacity_ = 64;
    std::vector<FrameSlot> ring_buffer_;
    std::atomic<size_t> write_idx_{0};
    std::atomic<size_t> read_idx_{0};

    // ---------- 运行统计 ----------
    std::atomic<size_t> total_frames_{0};
    std::atomic<size_t> dropped_frames_{0};
    std::atomic<size_t> overrun_count_{0};
    std::atomic<size_t> dropped_since_log_{0};  // 丢帧日志节流计数

    // ---------- 实例级日志（多实例独立配置） ----------
    FILE* log_file_ = stderr;
    int log_level_ = 1;
    std::mutex log_mtx_;
};
