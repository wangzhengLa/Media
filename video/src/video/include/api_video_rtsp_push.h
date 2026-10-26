#ifndef API_VIDEO_RTSP_PUSH_H_
#define API_VIDEO_RTSP_PUSH_H_

#include <thread>
#include <memory>
#include <mutex>
#include <atomic>
#include "image_publisher.h"
#include "queue.h"
#include "video_common_defines.h"

// RTSP Server
#include "xop/RtspServer.h"
#include "xop/H264Source.h"
#include "xop/G711ASource.h"
#include "xop/MediaSession.h"

// 音频
#include "audio_capture.h"
#include "g711_encoder.h"
#include "resampler.h"
#include "audio_processor.h"

// ringbuffer.h 的 #define min 会炸 C++ stdlib，include 后 undef
#pragma push_macro("min")
#undef min
#include "ringbuffer.h"
#pragma pop_macro("min")

#define MAX_H264_QUEUE_SIZE (30)

// 音频常量
#define AUDIO_RING_BUFFER_SIZE (65536)  // 64KB，必须是 2 的幂（ringbuffer 要求）
#define AUDIO_INPUT_RATE       (16000)   // ALSA 采集采样率
#define AUDIO_OUTPUT_RATE      (8000)    // G711 输出采样率
#define AUDIO_INPUT_CHANNELS   (1)       // 采集单声道
#define AUDIO_OUTPUT_CHANNELS  (1)       // 输出单声道
#define AUDIO_FRAME_MS         (20)      // 每帧 20ms（G711 常用）

namespace ecovacs
{
    // RTSP 服务器配置
    struct RtspConfig {
        std::string ip = "0.0.0.0";
        uint16_t port = 554;
        std::string suffix = "live";
        uint32_t framerate = 30;
        bool enable_auth = false;
        std::string realm = "-_-";
        std::string username = "admin";
        std::string password = "12345";
        bool auto_start = true;      // true: StartPush/StopPush 自动启停 RTSP
    };

    class VideoRtspPush : public video_common::EncodedImageSinkInterface
    {
    public:
        VideoRtspPush();
        ~VideoRtspPush() override;

        static std::unique_ptr<VideoRtspPush> Create();

        int32_t Init();
        int32_t UnInit();
        int32_t StartPush();
        int32_t StopPush();
        bool PushStarted() const { return pushStarted_.load(std::memory_order_acquire); }

        // RTSP 独立控制（即使 auto_start = false 也能手动启动）
        int32_t StartRtspServer(const RtspConfig& config = RtspConfig());
        int32_t StopRtspServer();
        bool RtspServerStarted() const { return rtsp_started_.load(std::memory_order_acquire); }
        uint32_t GetRtspClientCount() const { return rtsp_client_count_.load(std::memory_order_acquire); }

        // 配置设置（必须在 StartPush 之前调用）
        void SetRtspConfig(const RtspConfig& config) { rtsp_config_ = config; }
        const RtspConfig& GetRtspConfig() const { return rtsp_config_; }

        // 音频降噪等级（0=关闭，1=低，2=中，3=高，4=很高）
        void SetAudioNoiseLevel(int32_t level) { audio_noise_level_ = level; }
        int32_t GetAudioNoiseLevel() const { return audio_noise_level_; }

        // 编码帧回调（来自编码器）
        void OnFrame(
            video_common::EncodedImageBuffer *encoded_image,
            const video_common::CodecSpecificInfo *codec_specific_info) override;

    private:
        bool PushProcess();
        void RtspEventLoopThread();

        // ---------- 音频相关 ----------
        bool AudioInit();
        void AudioUnInit();
        bool AudioStart();
        void AudioStop();
        void AudioCaptureCallback(const int16_t* data, size_t bytes,
                                  int sample_rate, int channels);
        bool AudioPushProcess();

        // 队列缓冲区结构体
        struct TH264Buffer {
            video_common::EncodedImageBuffer *h264_frame = nullptr;
        };

        TIntelliBufHndl h264_buf_hndl_ = {};
        TH264Buffer h264_buf_[MAX_H264_QUEUE_SIZE];
        std::mutex api_lock_;
        std::thread pushThread_;
        std::atomic<bool> pushStarted_{false};
        std::atomic<bool> quit_{false};
        int64_t video_start_us_ = 0;     // 视频时间戳起点（steady_clock 微秒）
        int64_t audio_start_us_ = 0;     // 音频时间戳起点（steady_clock 微秒，与视频统一时钟域）

        // 音频：采集、编码、重采样、降噪
        AlsaRecorder audio_recorder_;
        G711Encoder g711_encoder_;
        AudioResampler audio_resampler_;
        AudioProcessor* audio_processor_ = nullptr;   // WebRTC APM 降噪/AEC
        int32_t audio_noise_level_ = 0;                 // 降噪等级：0=关闭，1=低，2=中，3=高
        struct ringbuffer* audio_ring_buf_ = nullptr;
        std::thread audio_thread_;
        std::atomic<bool> audio_running_{false};
        std::atomic<bool> audio_quit_{false};
        std::mutex audio_mutex_;

#if defined(ENABLE_ROS2)
        std::shared_ptr<ImagePublisher> image_pub_node_;
#endif

#ifdef ENABLE_ICEORYX
        std::unique_ptr<iox::popo::Publisher<H264Frame>> iceoryx_h264_publisher_;
#endif

        // RTSP 相关
        std::shared_ptr<xop::EventLoop> rtsp_event_loop_;
        std::shared_ptr<xop::RtspServer> rtsp_server_;
        xop::MediaSessionId rtsp_session_id_ = 0;
        std::atomic<bool> rtsp_started_{false};
        std::atomic<uint32_t> rtsp_client_count_{0};
        std::mutex rtsp_mutex_;
        RtspConfig rtsp_config_;
    };

} // namespace ecovacs

#endif // API_VIDEO_RTSP_PUSH_H_
