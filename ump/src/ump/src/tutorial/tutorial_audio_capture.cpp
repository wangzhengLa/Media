#include "audio_capture/audio_capture.h"
#include "audio_resample/resampler.h"
#include "audio_processor/audio_processor.h"
#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>
#include <signal.h>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <mutex>
#include <vector>
#include <alsa/asoundlib.h>   // ALSA 头文件

static std::ofstream outfile_raw;
static std::ofstream outfile_processed;
static std::ofstream outfile_8k;
static std::ofstream outfile_ref;
static AlsaRecorder* g_recorder = nullptr;
static std::atomic<bool> g_should_exit{false};
static AudioResampler g_resampler;
static AudioProcessor* g_processor = nullptr;

// 帧缓存
static std::vector<int16_t> g_frame_buffer;
static size_t g_frame_size = 0;  // 每声道每帧样本数（10ms）

// 声道选择（0-indexed）：第1声道作麦克风原声
static const int kMicChannel = 0;
// 参考声道（0-indexed），可由 argv[5] 指定（默认第6路 index=5），用于扫描哪一路才是真正参考
static int g_ref_channel = 5;

// ---------- 音频回调 ----------
void OnAudioData(const int16_t* data, size_t bytes, int sample_rate, int channels) {
    // 需要至少 g_ref_channel+1 个声道（第1声道 mic，指定声道 ref）
    if (channels <= g_ref_channel) {
        std::cerr << "Error: need at least " << (g_ref_channel + 1)
                  << " channels, got " << channels << std::endl;
        return;
    }

    size_t total_samples = bytes / sizeof(int16_t);
    if (g_frame_size == 0) {
        g_frame_size = sample_rate / 100;  // 每声道每帧样本数（10ms）
    }

    // 交错数据直接入缓冲
    g_frame_buffer.insert(g_frame_buffer.end(), data, data + total_samples);

    // 一帧的交错样本数 = 每声道帧长 × 声道数
    size_t frame_interleaved = (size_t)g_frame_size * channels;

    while (g_frame_buffer.size() >= frame_interleaved) {
        // 取出一帧交错数据
        std::vector<int16_t> frame(g_frame_buffer.begin(),
                                    g_frame_buffer.begin() + frame_interleaved);
        g_frame_buffer.erase(g_frame_buffer.begin(),
                              g_frame_buffer.begin() + frame_interleaved);

        // 拆分声道：抽取第1声道(mic)和第6声道(ref)
        std::vector<int16_t> rec_frame(g_frame_size);
        std::vector<int16_t> ref_frame(g_frame_size);
        for (size_t i = 0; i < g_frame_size; i++) {
            rec_frame[i] = frame[i * channels + kMicChannel];
            ref_frame[i] = frame[i * channels + g_ref_channel];
        }

        // 保存原始麦克风数据 (record1.pcm)
        if (outfile_raw.is_open()) {
            outfile_raw.write(reinterpret_cast<const char*>(rec_frame.data()),
                              rec_frame.size() * sizeof(int16_t));
        }
        // 保存回采参考信号 (record_ref.pcm)，用于验证第6声道是否为干净的播放参考
        if (outfile_ref.is_open()) {
            outfile_ref.write(reinterpret_cast<const char*>(ref_frame.data()),
                              ref_frame.size() * sizeof(int16_t));
        }

        // 处理音频（降噪/AEC），使用第6声道回采作为参考信号；延时由 AEC3 内部自动估算
        std::vector<int16_t> processed(g_frame_size);
        if (g_processor) {
            int ret = audio_processor_process(g_processor,
                                              rec_frame.data(),
                                              ref_frame.data(),
                                              processed.data());
            if (ret != 0) {
                std::cerr << "Audio processing error: " << ret << ", using original." << std::endl;
                processed = rec_frame;
            }
        } else {
            processed = rec_frame;
        }

        // 保存处理后的原始采样率 (record2.pcm)
        if (outfile_processed.is_open()) {
            outfile_processed.write(reinterpret_cast<const char*>(processed.data()),
                                    processed.size() * sizeof(int16_t));
        }

        // 重采样到 8kHz
        static bool resampler_ready = false;
        static std::vector<int16_t> resampled_buffer;
        if (!resampler_ready) {
            if (!g_resampler.Init(1, sample_rate, 8000, 3)) {
                std::cerr << "Failed to init resampler" << std::endl;
                return;
            }
            resampler_ready = true;
        }
        size_t out_samples = g_resampler.Process(processed.data(), processed.size(), resampled_buffer);

        // 保存重采样后的 8kHz 数据 (record3.pcm)
        if (outfile_8k.is_open()) {
            outfile_8k.write(reinterpret_cast<const char*>(resampled_buffer.data()),
                             out_samples * sizeof(int16_t));
        }
    }
}

void signal_handler(int sig) { (void)sig; g_should_exit = true; }

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    AlsaRecorder recorder;
    g_recorder = &recorder;

    const char* device  = (argc > 1) ? argv[1] : "default";
    int sample_rate     = (argc > 2) ? std::stoi(argv[2]) : 16000;
    int channels        = (argc > 3) ? std::stoi(argv[3]) : 6;
    int noise_level     = (argc > 4) ? std::stoi(argv[4]) : 2;
    if (argc > 5) g_ref_channel = std::stoi(argv[5]);

    // 创建音频处理器
    g_processor = audio_processor_create(sample_rate, noise_level);
    if (!g_processor) {
        std::cerr << "Warning: Failed to create audio processor; continuing without processing." << std::endl;
    }

    // 初始化录音
    if (!recorder.Init(device, sample_rate, channels, 10, 64)) {
        std::cerr << "Init failed: " << recorder.GetLastError() << std::endl;
        if (g_processor) audio_processor_destroy(g_processor);
        return -1;
    }

    // 打开输出文件
    const char* path_raw      = "/oem/record1.pcm";
    const char* path_processed = "/oem/record2.pcm";
    const char* path_8k       = "/oem/record3.pcm";
    const char* path_ref      = "/oem/record_ref.pcm";
    outfile_raw.open(path_raw, std::ios::binary);
    outfile_processed.open(path_processed, std::ios::binary);
    outfile_8k.open(path_8k, std::ios::binary);
    outfile_ref.open(path_ref, std::ios::binary);
    if (!outfile_raw.is_open() || !outfile_processed.is_open() || !outfile_8k.is_open()) {
        std::cerr << "Failed to open output files." << std::endl;
        if (g_processor) audio_processor_destroy(g_processor);
        return -1;
    }

    recorder.RegisterCallback(OnAudioData);
    if (!recorder.Start()) {
        std::cerr << "Start failed: " << recorder.GetLastError() << std::endl;
        outfile_raw.close();
        outfile_processed.close();
        outfile_8k.close();
        outfile_ref.close();
        if (g_processor) audio_processor_destroy(g_processor);
        return -1;
    }

    std::cout << "Recording (" << channels << "ch, " << sample_rate
              << "Hz) with ";
    if (noise_level <= 0)
        std::cout << "PURE AEC (AGC/NS off)";
    else
        std::cout << "NS(level=" << noise_level << ") + AEC";
    std::cout << " (ch1=mic, ch" << (g_ref_channel + 1) << "=loopback ref)" << std::endl;
    std::cout << "Saving original data to " << path_raw << std::endl;
    std::cout << "Saving processed (original rate) to " << path_processed << std::endl;
    std::cout << "Saving resampled (8kHz) to " << path_8k << std::endl;
    std::cout << "Saving echo reference to " << path_ref << std::endl;
    std::cout << "Press Ctrl+C to stop." << std::endl;

    // 等待录音结束（轮询错误/按 Ctrl+C 退出），不做运行时统计打印
    while (recorder.IsRecording() && !g_should_exit) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (recorder.GetState() == RecorderState::Error) {
            std::cerr << "Recorder error: " << recorder.GetLastError() << std::endl;
            break;
        }
    }

    recorder.Stop();
    outfile_raw.close();
    outfile_processed.close();
    outfile_8k.close();
    outfile_ref.close();

    if (g_processor) {
        audio_processor_destroy(g_processor);
        g_processor = nullptr;
    }

    std::cout << "===== Recording Statistics =====" << std::endl;
    std::cout << "Total frames:   " << recorder.GetTotalFrames() << std::endl;
    std::cout << "Dropped frames: " << recorder.GetDroppedFrames() << std::endl;
    std::cout << "Overrun count:  " << recorder.GetOverrunCount() << std::endl;
    std::cout << "Drop rate:      " << (recorder.GetDropRate() * 100) << "%" << std::endl;
    std::cout << "================================" << std::endl;

    return 0;
}
