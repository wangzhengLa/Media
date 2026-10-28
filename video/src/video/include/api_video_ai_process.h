#ifndef API_VIDEO_AI_PROCESS_H_
#define API_VIDEO_AI_PROCESS_H_

#include <thread>
#include <memory>
#include <vector>
#include <queue>
#include <mutex>
#include <cstring>
#include <atomic>
#include "log.h"
#include "osp.h"
#include "libyuv.h"
#include "frame_publisher.h"
#include "queue.h"
#include "video_common_defines.h"

#ifdef ENABLE_WEBSOCKETS
#include <turbojpeg.h>
#include <libwebsockets.h>
#include "api_soft_jpeg_encoder.h"
#include "api_websocket_server.h"
#endif

#ifdef ENABLE_ICEORYX
#include "topic_data.hpp"
#include "iceoryx_posh/popo/publisher.hpp"
#endif

#define MAX_AI_FRAME_QUEUE_SIZE (30)

namespace ecovacs
{
        class VideoAI : public video_common::VideoSinkInterface
        {
        public:
                VideoAI();
                ~VideoAI() override;

                static std::unique_ptr<VideoAI> Create();

                int32_t Init();
                int32_t UnInit();
                int32_t StartAI();
                int32_t StopAI();
                bool AIStarted();

                void OnFrame(video_common::VideoFrameBuffer *frame_buffer) override;
                void AIMemcpy(uint8_t *dst, const uint8_t *src, size_t n);

#ifdef ENABLE_WEBSOCKETS
                void SetJpegParams(int quality = 85, bool enable = true)
                {
                        jpeg_quality_ = quality;
                        enable_jpeg_ = enable;
                }
                void StartWebSocketServer()
                {
                        if (ws_server_)
                                ws_server_->Start();
                }
#endif

        private:
                bool VideoAIProcess();

#if defined(ENABLE_ROS2)
                void publishToROS2(video_common::VideoFrameBuffer *frame);
#endif
#ifdef ENABLE_WEBSOCKETS
                void publishToWebSocket(video_common::VideoFrameBuffer *frame);
#endif
#ifdef ENABLE_ICEORYX
                void publishToIceoryx(video_common::VideoFrameBuffer *frame);
#endif
                void processFrame(video_common::VideoFrameBuffer *frame);

                struct FrameBuffer
                {
                        video_common::VideoFrameBuffer *frame;
                };

                video_common::FramerateController frame_rate_ctl_;
                TIntelliBufHndl frame_buf_hndl_;
                FrameBuffer frame_buf_[MAX_AI_FRAME_QUEUE_SIZE];
                std::mutex api_lock_;
                std::thread ai_thread_;
                std::atomic<bool> ai_started_{false};
                std::atomic<bool> quit_{false};

#if defined(ENABLE_ROS2)
                std::shared_ptr<FramePublisher> frame_pub_node_;
#endif

#ifdef ENABLE_WEBSOCKETS
                std::unique_ptr<JpegEncoder> jpeg_encoder_;
                std::unique_ptr<WebSocketServer> ws_server_;
                int jpeg_quality_;
                bool enable_jpeg_;
                bool jpeg_inited_;
#endif

#ifdef ENABLE_ICEORYX
                std::unique_ptr<iox::popo::Publisher<NV12Frame>> iceoryx_publisher_;
#endif
        };
}

#endif // API_VIDEO_AI_PROCESS_H_