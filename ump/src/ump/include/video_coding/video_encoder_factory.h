#ifndef VIDEO_ENCODER_FACTORY_H_
#define VIDEO_ENCODER_FACTORY_H_

#include "video_encoder.h"
#include "video_common_defines.h"

namespace ecovacs
{
    class VideoEncoderFactory
    {
    public:
        static VideoEncoderModule *Create(const video_common::VideoCodecType codecType);

    private:
        ~VideoEncoderFactory();
    };

} // namespace ecovacs

#endif // VIDEO_ENCODER_FACTORY_H_
