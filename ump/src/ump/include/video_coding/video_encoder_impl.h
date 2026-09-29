#ifndef VIDEO_ENCODER_IMPL_H_
#define VIDEO_ENCODER_IMPL_H_

#include "video_encoder.h"
#include "video_common_defines.h"

#define WEBRTC_VIDEO_CODEC_OK 0
#define WEBRTC_VIDEO_CODEC_ERROR -1
#define WEBRTC_VIDEO_CODEC_MEMORY -3
#define WEBRTC_VIDEO_CODEC_ERR_PARAMETER -4
#define WEBRTC_VIDEO_CODEC_TIMEOUT -6

namespace ecovacs
{
    class VideoEncoderImpl : public VideoEncoderModule
    {
    public:
        VideoEncoderImpl();

        ~VideoEncoderImpl() override;

        // Initialize the encoder with the information from the codecSettings.
        int32_t InitEncode(const video_common::VideoCodecConfig *codec_settings) override;

        // Free encoder memory.
        int32_t Release() = 0;

        // Register an encode complete callback object.
        int32_t RegisterEncodeCompleteCallback(video_common::EncodedImageSinkInterface *callback) override;

        // DeRegister an encode complete callback object.
        int32_t DeRegisterEncodeCompleteCallback() override;

        // Encode an image (as a part of a video stream). The encoded image
        // will be returned to the user through the encode complete callback.
        int32_t Encode(const video_common::VideoFrameBuffer *frame_buffer) override;

        // require key frame.
        int32_t RequestKeyFrame() override;

        // Sets rate control parameters: bitrate, framerate, etc. These settings are
        // instantaneous (i.e. not moving averages) and should apply from now until
        // the next call to SetRates().
        void SetRates(const video_common::RateControlParameters &parameters) override;

    protected:
        int32_t CheckCodecConfig(const video_common::VideoCodecConfig *codec_settings);

    protected:
        static uint32_t instance_num;
        static pthread_mutex_t mutex;
        std::mutex api_lock_;
        video_common::VideoCodecConfig codec_cfg_;
        video_common::EncodedImageSinkInterface *encoded_image_callback_;
    };

} // namespace ecovacs

#endif // VIDEO_ENCODER_IMPL_H_
