#ifndef API_JPEG_PUBLISH_H_
#define API_JPEG_PUBLISH_H_

#include <fstream>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <mutex>
#include <atomic>

#include "image_publisher.h"
#include "queue.h"
#include "video_common_defines.h"

#define MAX_JPEG_PUB_QUEUE_SIZE (15)

namespace ecovacs
{
    class JpegPublish : public video_common::EncodedImageSinkInterface
    {
    public:
        JpegPublish(); // 构造函数改为 public
        ~JpegPublish() override;

        static std::unique_ptr<JpegPublish> Create();

        int32_t Init();
        int32_t UnInit();
        int32_t StartPublish();
        int32_t StopPublish();
        bool PublishStarted();

        void OnFrame(
            video_common::EncodedImageBuffer *encoded_image,
            const video_common::CodecSpecificInfo *codec_specific_info) override;

    private:
        bool PublishProcess();

        struct TJpegBuffer
        {
            video_common::EncodedImageBuffer *jpeg_frame;
        };
        TIntelliBufHndl jpeg_buf_hndl_;
        TJpegBuffer jpeg_buf_[MAX_JPEG_PUB_QUEUE_SIZE];
        std::mutex api_lock_;
        std::thread publishThread_;
        std::atomic<bool> publishStarted_{false};
        std::atomic<bool> quit_{false};
#if defined(ENABLE_ROS2)
        std::shared_ptr<ImagePublisher> image_pub_node_;
#endif
    };

} // namespace ecovacs

#endif // API_JPEG_PUBLISH_H_