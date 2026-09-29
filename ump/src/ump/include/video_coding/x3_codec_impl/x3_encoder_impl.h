#ifndef X3_H264_ENCODER_IMPL_H_
#define X3_H264_ENCODER_IMPL_H_

#include "video_encoder.h"
#include "video_encoder_impl.h"
#include "vio/hb_vio_interface.h"
#include "vio/hb_vps_api.h"
#include "vio/hb_vp_api.h"
#include "vio/hb_comm_venc.h"
#include "vio/hb_venc.h"
#include "x3_encoder_handle.h"

namespace ecovacs
{
    class X3EncoderImpl : public VideoEncoderImpl
    {
    public:
        X3EncoderImpl();

        ~X3EncoderImpl() override;

        int32_t InitEncode(const video_common::VideoCodecConfig *codec_settings) override;

        int32_t Release() override;

        int32_t Encode(const video_common::VideoFrameBuffer *frame_buffer) override;

        int32_t RequestKeyFrame() override;

        void SetRates(const video_common::RateControlParameters &parameters) override;

    private:
        video_common::VideoCodecConfig codec_cfg_;
        MpiEncHandle mpi_enc_handle_;
    };

} // namespace ecovacs

#endif // X3EncoderImpl