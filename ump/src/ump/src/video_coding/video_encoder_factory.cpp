#include "video_encoder_factory.h"
#ifdef RK_PLATFORM
#include "rk_encoder_impl.h"
#elif AW_PLATFORM
#include "allwinner_encoder_impl.h"
#endif

namespace ecovacs
{
    VideoEncoderModule *VideoEncoderFactory::Create(const video_common::VideoCodecType codecType)
    {
#ifdef RK_PLATFORM
        auto implementation = new RkEncoderImpl();
        return implementation;
#elif AW_PLATFORM
        auto implementation = new AllWinnerEncoderImpl();
        return implementation;
#endif
        return nullptr;
    }

} // namespace ecovacs
