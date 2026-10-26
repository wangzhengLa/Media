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
#include <vector>
#include <alsa/asoundlib.h>   // ALSA 头文件

static std::ofstream outfile_raw;
static std::ofstream outfile_processed;
static std::ofstream outfile_8k;
static AlsaRecorder* g_recorder = nullptr;
static std::atomic<bool> g_should_exit{false};
static AudioResampler g_resampler;
static AudioProcessor* g_processor = nullptr;

// 帧缓存
static std::vector<int16_t> g_frame_buffer;
static size_t g_frame_size = 0;
static std::vector<int16_t> g_zero_render;

// 播放设备句柄（用于查询延迟）
static snd_pcm_t* g_playback_handle = nullptr;
static int g_playback_sample_rate = 0;

// ---------- 延迟获取函数 ----------
static int get_capture_delay_ms(snd_pcm_t* handle, int sample_rate) {
    if (!handle) return -1;
    snd_pcm_sframes_t delay_frames;
    int err = snd_pcm_delay(handle, &delay_frames);
    if (err < 0 || delay_frames < 0) return -1;
    return (int)((double)delay_frames * 1000.0 / sample_rate);
}

static int get_playback_delay_ms(snd_pcm_t* handle, int sample_rate) {
    if (!handle) return -1;
    snd_pcm_sframes_t delay_frames;
    int err = snd_pcm_delay(handle, &delay_frames);
    if (err < 0 || delay_frames < 0) return -1;
    return (int)((double)delay_frames * 1000.0 / sample_rate);
}

static int get_total_delay_ms() {
    if (!g_recorder) return -1;
    // 采集延迟（假设 AlsaRecorder 已提供 GetCaptureDelayMs()）
    int cap_delay = g_recorder->GetCaptureDelayMs();
    int play_delay = get_playback_delay_ms(g_playback_handle, g_playback_sample_rate);
    const int FIXED_OFFSET_MS = 20; // 根据实际硬件调整
    if (cap_delay >= 0 && play_delay >= 0) {
        return cap_delay + play_delay + FIXED_OFFSET_MS;
    } else if (cap_delay >= 0) {
        return cap_delay + FIXED_OFFSET_MS;
    } else if (play_delay >= 0) {
        return play_delay + FIXED_OFFSET_MS;
    }
    return -1; // 无法获取
}

// ---------- 音频回调 ----------
void OnAudioData(const int16_t* data, size_t bytes, int sample_rate, int channels) {
    if (channels != 1) {
        std::cerr << "Warning: multi-channel data, only first channel used." << std::endl;
        return;
    }

    size_t total_samples = bytes / sizeof(int16_t);
    if (g_frame_size == 0) {
        g_frame_size = sample_rate / 100;
        g_zero_render.assign(g_frame_size, 0);
    }

    g_frame_buffer.insert(g_frame_buffer.end(), data, data + total_samples);

    while (g_frame_buffer.size() >= g_frame_size) {
        std::vector<int16_t> rec_frame(g_frame_buffer.begin(), g_frame_buffer.begin() + g_frame_size);
        g_frame_buffer.erase(g_frame_buffer.begin(), g_frame_buffer.begin() + g_frame_size);

        // 保存原始数据 (record1.pcm)
        if (outfile_raw.is_open()) {
            outfile_raw.write(reinterpret_cast<const char*>(rec_frame.data()),
                              rec_frame.size() * sizeof(int16_t));
        }

        // 处理音频（降噪/AEC）
        std::vector<int16_t> processed(g_frame_size);
        if (g_processor) {
            int delay_ms = get_total_delay_ms();
            if (delay_ms < 0) delay_ms = 0; // 无效时使用0
            int ret = audio_processor_process(g_processor,
                                              rec_frame.data(),
                                              g_zero_render.data(),
                                              processed.data(),
                                              delay_ms);
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
    int channels        = (argc > 3) ? std::stoi(argv[3]) : 1;

    // 创建音频处理器
    g_processor = audio_processor_create(sample_rate, 1);
    if (!g_processor) {
        std::cerr << "Warning: Failed to create audio processor; continuing without processing." << std::endl;
    }

    // 初始化录音
    if (!recorder.Init(device, sample_rate, channels, 10, 64)) {
        std::cerr << "Init failed: " << recorder.GetLastError() << std::endl;
        if (g_processor) audio_processor_destroy(g_processor);
        return -1;
    }

    // ---- 打开播放设备（仅用于延迟查询） ----
    int err = snd_pcm_open(&g_playback_handle, device, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (err < 0) {
        std::cerr << "Warning: Cannot open playback device for delay query: " << snd_strerror(err) << std::endl;
        g_playback_handle = nullptr;
    } else {
        // 设置参数（与录音一致）
        snd_pcm_hw_params_t* params;
        snd_pcm_hw_params_alloca(&params);
        snd_pcm_hw_params_any(g_playback_handle, params);
        snd_pcm_hw_params_set_access(g_playback_handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
        snd_pcm_hw_params_set_format(g_playback_handle, params, SND_PCM_FORMAT_S16_LE);
        snd_pcm_hw_params_set_channels(g_playback_handle, params, channels);
        unsigned int rate = sample_rate;
        snd_pcm_hw_params_set_rate_near(g_playback_handle, params, &rate, nullptr);
        err = snd_pcm_hw_params(g_playback_handle, params);
        if (err < 0) {
            std::cerr << "Warning: Failed to set playback parameters for delay query: " << snd_strerror(err) << std::endl;
            snd_pcm_close(g_playback_handle);
            g_playback_handle = nullptr;
        } else {
            g_playback_sample_rate = rate;
        }
    }

    // 打开输出文件
    const char* path_raw      = "/oem/record1.pcm";
    const char* path_processed = "/oem/record2.pcm";
    const char* path_8k       = "/oem/record3.pcm";
    outfile_raw.open(path_raw, std::ios::binary);
    outfile_processed.open(path_processed, std::ios::binary);
    outfile_8k.open(path_8k, std::ios::binary);
    if (!outfile_raw.is_open() || !outfile_processed.is_open() || !outfile_8k.is_open()) {
        std::cerr << "Failed to open output files." << std::endl;
        if (g_processor) audio_processor_destroy(g_processor);
        if (g_playback_handle) snd_pcm_close(g_playback_handle);
        return -1;
    }

    recorder.RegisterCallback(OnAudioData);
    if (!recorder.Start()) {
        std::cerr << "Start failed: " << recorder.GetLastError() << std::endl;
        outfile_raw.close();
        outfile_processed.close();
        outfile_8k.close();
        if (g_processor) audio_processor_destroy(g_processor);
        if (g_playback_handle) snd_pcm_close(g_playback_handle);
        return -1;
    }

    std::cout << "Recording (mono, " << sample_rate << "Hz) with NS + AEC (zero reference)..." << std::endl;
    std::cout << "Saving original data to " << path_raw << std::endl;
    std::cout << "Saving processed (original rate) to " << path_processed << std::endl;
    std::cout << "Saving resampled (8kHz) to " << path_8k << std::endl;
    std::cout << "Press Ctrl+C to stop." << std::endl;

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

    if (g_processor) {
        audio_processor_destroy(g_processor);
        g_processor = nullptr;
    }
    if (g_playback_handle) {
        snd_pcm_close(g_playback_handle);
        g_playback_handle = nullptr;
    }

    std::cout << "===== Recording Statistics =====" << std::endl;
    std::cout << "Total frames:   " << recorder.GetTotalFrames() << std::endl;
    std::cout << "Dropped frames: " << recorder.GetDroppedFrames() << std::endl;
    std::cout << "Overrun count:  " << recorder.GetOverrunCount() << std::endl;
    std::cout << "Drop rate:      " << (recorder.GetDropRate() * 100) << "%" << std::endl;
    std::cout << "================================" << std::endl;

    return 0;
}
