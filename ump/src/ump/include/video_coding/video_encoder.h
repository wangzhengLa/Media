#ifndef VIDEO_ENCODER_H_
#define VIDEO_ENCODER_H_

#include "video_common_defines.h"

#define WEBRTC_VIDEO_CODEC_OK 0
#define WEBRTC_VIDEO_CODEC_ERROR -1
#define WEBRTC_VIDEO_CODEC_UNINITIALIZED -2
#define WEBRTC_VIDEO_CODEC_MEMORY -3
#define WEBRTC_VIDEO_CODEC_ERR_PARAMETER -4
#define WEBRTC_VIDEO_CODEC_TIMEOUT -6

namespace ecovacs
{
    class VideoEncoderModule
    {
    public:
        // Initialize the encoder with the information from the codecSettings.
        virtual int32_t InitEncode(const video_common::VideoCodecConfig *codec_settings) = 0;

        // Free encoder memory.
        virtual int32_t Release() = 0;

        // Register an encode complete callback object.
        virtual int32_t RegisterEncodeCompleteCallback(video_common::EncodedImageSinkInterface *callback) = 0;

        // DeRegister an encode complete callback object.
        virtual int32_t DeRegisterEncodeCompleteCallback() = 0;

        // Encode an image (as a part of a video stream). The encoded image
        // will be returned to the user through the encode complete callback.
        virtual int32_t Encode(const video_common::VideoFrameBuffer *frame_buffer) = 0;

        // require key frame.
        virtual int32_t RequestKeyFrame() = 0;

        // Sets rate control parameters: bitrate, framerate, etc. These settings are
        // instantaneous (i.e. not moving averages) and should apply from now until
        // the next call to SetRates().
        virtual void SetRates(const video_common::RateControlParameters &parameters) = 0;

        virtual ~VideoEncoderModule() {}
    };

} // namespace ecovacs

#endif // VIDEO_ENCODER_H_
