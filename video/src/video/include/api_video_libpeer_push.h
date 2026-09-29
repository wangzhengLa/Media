#ifndef API_VIDEO_LIBPEER_PUSH_H_
#define API_VIDEO_LIBPEER_PUSH_H_

#include <thread>
#include <memory>
#include <mutex>
#include <atomic>
#include <string>

#include "queue.h"
#include "video_common_defines.h"

// libpeer C API
extern "C" {
#include "peer.h"
#include "peer_connection.h"
}

// 信令客户端（项目内部实现）
#include "signaling_client.h"

#define MAX_H264_QUEUE_SIZE (30)

namespace ecovacs
{
    // libpeer 推流配置
    struct LibpeerConfig {
        std::string signaling_url = "http://10.10.15.135:8080/signal?room=room1";
        std::string turn_url = "turn:10.10.15.135:3478";
        std::string turn_username = "webrtc";
        std::string turn_password = "Intra@2026Turn";
        int video_fps = 15;          // 发送帧率上限
        int is_offerer = 1;          // 1=offerer(发 offer), 0=answerer
    };

    class VideoLibpeerPush : public video_common::EncodedImageSinkInterface
    {
    public:
        VideoLibpeerPush();
        ~VideoLibpeerPush() override;

        static std::unique_ptr<VideoLibpeerPush> Create();

        int32_t Init();
        int32_t UnInit();
        int32_t StartPush();
        int32_t StopPush();
        bool PushStarted() const { return push_started_.load(std::memory_order_acquire); }

        void SetConfig(const LibpeerConfig& config) { config_ = config; }
        const LibpeerConfig& GetConfig() const { return config_; }

        void OnFrame(
            video_common::EncodedImageBuffer *encoded_image,
            const video_common::CodecSpecificInfo *codec_specific_info) override;

    private:
        // 帧缓冲区
        struct TH264Buffer {
            video_common::EncodedImageBuffer *h264_frame = nullptr;
        };

        // 入队（OnFrame 调用，快速返回）
        void EnqueueFrame(video_common::EncodedImageBuffer *encoded_image,
                          const video_common::CodecSpecificInfo *codec_specific_info);

        // 从队列取最新帧（丢积压）
        TH264Buffer* DequeueLatestFrame();

        TIntelliBufHndl h264_buf_hndl_ = {};
        TH264Buffer h264_buf_[MAX_H264_QUEUE_SIZE];

        std::mutex api_lock_;
        std::atomic<bool> push_started_{false};
        std::atomic<bool> quit_{false};

        // libpeer 相关
        PeerConnection *pc_ = nullptr;
        std::atomic<PeerConnectionState> peer_state_{PEER_CONNECTION_NEW};
        std::thread peer_loop_thread_;
        std::thread signaling_loop_thread_;
        std::atomic<bool> peer_quit_{false};
        SignalingClient signaling_client_{};
        LibpeerConfig config_;
    };

} // namespace ecovacs

#endif // API_VIDEO_LIBPEER_PUSH_H_
