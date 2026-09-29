#ifndef API_VIDEO_PUSH_H_
#define API_VIDEO_PUSH_H_

#include <thread>
#include <memory>
#include <mutex>
#include <atomic>

#include "image_publisher.h"
#include "queue.h"
#include "video_common_defines.h"

// iceoryx 支持
#ifdef ENABLE_ICEORYX
#include "topic_data.hpp"
#include "iceoryx_posh/popo/publisher.hpp"
#endif

#define MAX_H264_QUEUE_SIZE (30)

namespace ecovacs
{
    class VideoPush : public video_common::EncodedImageSinkInterface
    {
    public:
        VideoPush();  // 构造函数改为 public
        ~VideoPush() override;

        static std::unique_ptr<VideoPush> Create();

        int32_t Init();
        int32_t UnInit();
        int32_t StartPush();
        int32_t StopPush();
        bool PushStarted();

        void OnFrame(
            video_common::EncodedImageBuffer *encoded_image,
            const video_common::CodecSpecificInfo *codec_specific_info) override;

    private:
        bool PushProcess();

        // 队列缓冲区结构体
        struct TH264Buffer
        {
            video_common::EncodedImageBuffer *h264_frame;
        };

        TIntelliBufHndl h264_buf_hndl_;
        TH264Buffer h264_buf_[MAX_H264_QUEUE_SIZE];
        std::mutex api_lock_;
        std::thread pushThread_;
        std::atomic<bool> pushStarted_{false};
        std::atomic<bool> quit_{false};

#if defined(ENABLE_ROS2)
        std::shared_ptr<ImagePublisher> image_pub_node_;
#endif

#ifdef ENABLE_ICEORYX
        std::unique_ptr<iox::popo::Publisher<H264Frame>> iceoryx_h264_publisher_;
#endif
    };

} // namespace ecovacs

#endif // API_VIDEO_PUSH_H_