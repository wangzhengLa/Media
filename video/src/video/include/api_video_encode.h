#ifndef API_VIDEO_ENCODE_H_
#define API_VIDEO_ENCODE_H_

#include <thread>
#include <mutex>
#include <atomic>
#include <memory>
#include "video_common_defines.h"
#include "video_encoder.h"
#include "queue.h"

#define MAX_FRAME_QUEUE_SIZE (30)

namespace ecovacs
{
    class VideoEncode : public video_common::VideoSinkInterface
    {
    public:
        VideoEncode(); // 构造函数改为 public
        ~VideoEncode() override;

        static std::unique_ptr<VideoEncode> Create(const video_common::VideoCodecConfig *codec_settings);

        int32_t Init(const video_common::VideoCodecConfig *codec_settings);
        int32_t Release();

        int32_t RegisterEncodeCompleteCallback(video_common::EncodedImageSinkInterface *callback);
        int32_t DeRegisterEncodeCompleteCallback();

        int32_t Encode(const video_common::VideoFrameBuffer *frame_buffer);
        int32_t StartEncode();
        int32_t StopEncode();
        bool EncodeStarted();
        int32_t RequestKeyFrame();
        int32_t SetRates(video_common::RateControlParameters &parameters);
        void OnFrame(video_common::VideoFrameBuffer *frame_buffer) override;
        void SetSkipFrames(int count) { skip_frames_.store(count, std::memory_order_release); }

    private:
        bool EncodeProcess();

        struct FrameBuffer
        {
            video_common::VideoFrameBuffer *frame;
        };

        std::unique_ptr<VideoEncoderModule> vem_;
        video_common::VideoCodecConfig cfg_;
        video_common::FramerateController frame_rate_ctl_;
        TIntelliBufHndl frame_buf_hndl_;
        FrameBuffer frame_buf_[MAX_FRAME_QUEUE_SIZE];
        std::mutex api_lock_;
        std::thread encodeThread_;
        std::atomic<bool> quit_{false};
        std::atomic<bool> encodeStarted_{false};
        std::atomic<int> skip_frames_{0};
    };

} // namespace ecovacs

#endif // API_VIDEO_ENCODE_H_