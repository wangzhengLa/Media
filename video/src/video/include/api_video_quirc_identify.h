#ifndef API_VIDEO_QUIRC_IDENTIFY_H_
#define API_VIDEO_QUIRC_IDENTIFY_H_

#include <ZXing/ReadBarcode.h>
#include <ZXing/TextUtfEncoding.h>

#include <thread>

#include "queue.h"
#include "string_publisher.h"
#include "video_common_defines.h"

#define MAX_QUIRC_FRAME_QUEUE_SIZE (30)

namespace ecovacs
{

    // 新增二维码识别状态结构体
    struct QRState
    {
        ZXing::DecodeHints decoder_hints =
            ZXing::DecodeHints()
                .setFormats(ZXing::BarcodeFormat::QRCode)
                .setTryRotate(true);
        bool enabled = true;
        std::string last_result; // 最后一次识别结果
    };

    class VideoQuircIdentify : public video_common::VideoSinkInterface
    {
    public:
        ~VideoQuircIdentify() override;

        static VideoQuircIdentify *Create();

        int32_t Init();

        int32_t UnInit();

        int32_t StartQuircIdentify();

        int32_t StopQuircIdentify();

        bool QuircIdentifyStarted();

        void OnFrame(video_common::VideoFrameBuffer *frame_buffer) override;

        void AIMemcpy(uint8_t *dst, const uint8_t *src, size_t n);

    private:
        VideoQuircIdentify();
        bool VideoQuircIdentifyProcess();
        ZXing::ImageView CreateZXingImageView(
            const video_common::VideoFrameBuffer *frame);
        struct FrameBuffer
        {
            video_common::VideoFrameBuffer *frame;
        };
        video_common::FramerateController frame_rate_ctl_;
        TIntelliBufHndl frame_buf_hndl_;
        FrameBuffer frame_buf_[MAX_QUIRC_FRAME_QUEUE_SIZE];
        std::mutex api_lock_;
        std::thread quirc_identify_thread_;
        bool quirc_identify_started_;
        bool quit_;
        int skip_frame_num_;
        int print_frame_cnt_;
        std::unique_ptr<QRState> qr_state_;
#if defined(ENABLE_ROS2)
        std::shared_ptr<StringPublisher> qr_result_pub_node_;
#endif
    };
} // namespace ecovacs
#endif