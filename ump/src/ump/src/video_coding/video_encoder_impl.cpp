#include <string.h>
#include "log.h"
#include "osp.h"
#include "video_common_defines.h"
#include "video_encoder_impl.h"

namespace ecovacs
{
    uint32_t VideoEncoderImpl::instance_num = 0;
    pthread_mutex_t VideoEncoderImpl::mutex = PTHREAD_MUTEX_INITIALIZER;

    VideoEncoderImpl::VideoEncoderImpl() : encoded_image_callback_(NULL) {}

    VideoEncoderImpl::~VideoEncoderImpl()
    {
    }

    int32_t VideoEncoderImpl::CheckCodecConfig(const video_common::VideoCodecConfig *codec_settings)
    {
        if (nullptr == codec_settings)
        {
            ump_error("codec_settings[%p] is nullptr\r\n", codec_settings);
            return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        }
        if (codec_settings->codecType != video_common::kVideoCodecH264 && codec_settings->codecType != video_common::kVideoCodecJPEG)
        {
            ump_error("codecType[%u] param error\r\n", codec_settings->codecType);
            return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        }
        if (codec_settings->maxFramerate == 0)
        {
            ump_error("maxFramerate[%u] param error\r\n", codec_settings->maxFramerate);
            return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        }
        if (codec_settings->width < 1 || codec_settings->height < 1)
        {
            ump_error("width-height[%u-%u] param error\r\n", codec_settings->width, codec_settings->height);
            return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        }
        if (codec_settings->frameType >= video_common::kI444)
        {
            ump_error("frameType[%u] param error\r\n", codec_settings->frameType);
            return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        }
        if (codec_settings->codecType == video_common::kVideoCodecH264)
        {
            if (codec_settings->codec_specific_.H264.min_qp < 0 || codec_settings->codec_specific_.H264.min_qp > 51)
            {
                ump_error("codec_specific_.H264.min_qp[%d] param error\r\n", codec_settings->codec_specific_.H264.min_qp);
                return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
            }
            if (codec_settings->codec_specific_.H264.max_qp < 0 || codec_settings->codec_specific_.H264.max_qp > 51)
            {
                ump_error("codec_specific_.H264.max_qp[%d] param error\r\n", codec_settings->codec_specific_.H264.max_qp);
                return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
            }
            if (codec_settings->codec_specific_.H264.init_qp < 0 || codec_settings->codec_specific_.H264.init_qp > 51)
            {
                ump_error("codec_specific_.H264.init_qp[%d] param error\r\n", codec_settings->codec_specific_.H264.init_qp);
                return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
            }
            if (codec_settings->codec_specific_.H264.max_qp < codec_settings->codec_specific_.H264.min_qp)
            {
                ump_error("codec_specific_.H264.max_qp-min_qp[%d-%d] param error\r\n",
                          codec_settings->codec_specific_.H264.max_qp, codec_settings->codec_specific_.H264.min_qp);
                return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
            }
            if (codec_settings->codec_specific_.H264.init_qp > codec_settings->codec_specific_.H264.max_qp)
            {
                ump_error("codec_specific_.H264.init_qp-max_qp[%d-%d] param error\r\n",
                          codec_settings->codec_specific_.H264.init_qp, codec_settings->codec_specific_.H264.max_qp);
                return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
            }
            if (codec_settings->codec_specific_.H264.init_qp < codec_settings->codec_specific_.H264.min_qp)
            {
                ump_error("codec_specific_.H264.init_qp-min_qp[%d-%d] param error\r\n",
                          codec_settings->codec_specific_.H264.init_qp, codec_settings->codec_specific_.H264.min_qp);
                return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
            }
            if (codec_settings->codec_specific_.H264.gop <= 0)
            {
                ump_error("codec_specific_.H264.gop[%d] param error\r\n", codec_settings->codec_specific_.H264.gop);
                return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
            }
        }
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t VideoEncoderImpl::InitEncode(const video_common::VideoCodecConfig *codec_settings)
    {
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t VideoEncoderImpl::Release()
    {
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t VideoEncoderImpl::Encode(const video_common::VideoFrameBuffer *frame_buffer)
    {
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t VideoEncoderImpl::RegisterEncodeCompleteCallback(video_common::EncodedImageSinkInterface *callback)
    {
        std::lock_guard<std::mutex> guard(api_lock_);
        encoded_image_callback_ = callback;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t VideoEncoderImpl::DeRegisterEncodeCompleteCallback()
    {
        std::lock_guard<std::mutex> guard(api_lock_);
        encoded_image_callback_ = NULL;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t VideoEncoderImpl::RequestKeyFrame()
    {
        return 0;
    }

    void VideoEncoderImpl::SetRates(const video_common::RateControlParameters &parameters)
    {
    }

} // namespace ecovacs
