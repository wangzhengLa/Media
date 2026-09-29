#include <string.h>
#include <com/amazonaws/kinesis/video/webrtcclient/log.h>
#include "osp.h"
#include "video_common_defines.h"
#include "x3_encoder_impl.h"
#include "video_encoder_impl.h"

namespace ecovacs
{
    X3EncoderImpl::X3EncoderImpl()
    {
        pthread_mutex_lock(&mutex);
        if (0 == instance_num)
            mpi_enc_handle_.channel = 0;
        else
            mpi_enc_handle_.channel = instance_num;
        instance_num++;
        pthread_mutex_unlock(&mutex);
    }

    X3EncoderImpl::~X3EncoderImpl()
    {
    }

    int32_t X3EncoderImpl::InitEncode(const video_common::VideoCodecConfig *codec_settings)
    {
        int32_t s32Ret;
        s32Ret = CheckCodecConfig(codec_settings);
        if (s32Ret != WEBRTC_VIDEO_CODEC_OK)
        {
            error("video_encoder_check_config error s32Ret:%d\n", s32Ret);
            return WEBRTC_VIDEO_CODEC_ERROR;
        }

        info("video_encoder_check_config s32Ret:%d\n", s32Ret);

        codec_cfg_ = *codec_settings;

        switch (codec_settings->frameType)
        {
        case video_common::kI420:
            mpi_enc_handle_.pixFmt = HB_PIXEL_FORMAT_YUV420P;
            break;
        case video_common::kNV12:
            mpi_enc_handle_.pixFmt = HB_PIXEL_FORMAT_NV12;
            break;
        case video_common::kNV21:
            mpi_enc_handle_.pixFmt = HB_PIXEL_FORMAT_NV21;
            break;
        default:
            break;
        }

        info("pixFmt = %d\n", mpi_enc_handle_.pixFmt);
        mpi_enc_handle_.width = codec_settings->width;
        mpi_enc_handle_.height = codec_settings->height;
        mpi_enc_handle_.hor_stride = ALIGN(mpi_enc_handle_.width, 16);
        mpi_enc_handle_.ver_stride = ALIGN(mpi_enc_handle_.height, 16);
        mpi_enc_handle_.frame_size = ALIGN(mpi_enc_handle_.hor_stride, 64) * ALIGN(mpi_enc_handle_.ver_stride, 64) * 3 / 2;
        info("width:%u, height:%u hor_stride:%u ver_stride:%u frame_size:%u\n", mpi_enc_handle_.width, mpi_enc_handle_.height,
             mpi_enc_handle_.hor_stride, mpi_enc_handle_.ver_stride, mpi_enc_handle_.frame_size);

        if (1 == instance_num)
        {
            s32Ret = MpiEncHandle::iot_vp_init();
            if (s32Ret != 0)
            {
                return WEBRTC_VIDEO_CODEC_ERROR;
            }
            s32Ret = HB_VENC_Module_Init();
            info("HB_VENC_Module_Init: %d\n", s32Ret);
            if (s32Ret)
            {
                info("HB_VENC_Module_Init: %d\n", s32Ret);
            }
        }

        s32Ret = mpi_enc_handle_.iot_sys_alloc(&mpi_enc_handle_.mmz_paddr, (void **)&mpi_enc_handle_.mmz_vaddr, mpi_enc_handle_.frame_size);
        if (s32Ret == 0)
        {
            info("mmzAlloc paddr = 0x%x, vaddr = 0x%x\n", mpi_enc_handle_.mmz_paddr, mpi_enc_handle_.mmz_vaddr);
        }
        else
        {
            error("HB_SYS_Alloc failed\r\n");
            return WEBRTC_VIDEO_CODEC_ERROR;
        }

        s32Ret = mpi_enc_handle_.iot_venc_init(&codec_cfg_);
        if (s32Ret != 0)
        {
            return WEBRTC_VIDEO_CODEC_ERROR;
        }

        info("iot_venc_init finshed channel:%d\n", mpi_enc_handle_.channel);

        s32Ret = mpi_enc_handle_.iot_venc_start();
        if (s32Ret != 0)
        {
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        info("iot_venc_start finshed");

        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t X3EncoderImpl::Release()
    {
        info("encode release\n");
        int32_t s32Ret;

        pthread_mutex_lock(&mutex);
        instance_num--;
        pthread_mutex_unlock(&mutex);

        mpi_enc_handle_.iot_venc_stop();
        mpi_enc_handle_.iot_venc_deinit();
        mpi_enc_handle_.iot_sys_free(mpi_enc_handle_.mmz_paddr, mpi_enc_handle_.mmz_vaddr);

        if (0 == instance_num)
        {
            MpiEncHandle::iot_vp_deinit();

            s32Ret = HB_VENC_Module_Uninit();
            if (s32Ret)
            {
                error("HB_VENC_Module_Init: %d\n", s32Ret);
            }
        }

        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t X3EncoderImpl::Encode(const video_common::VideoFrameBuffer *frame_buffer)
    {
        std::lock_guard<std::mutex> guard(api_lock_);
        int32_t s32Ret;
        VIDEO_FRAME_S pstFrame;
        VIDEO_STREAM_S pstStream;

        memset(&pstFrame, 0, sizeof(VIDEO_FRAME_S));
        memset(&pstStream, 0, sizeof(VIDEO_STREAM_S));

        pstFrame.stVFrame.width = mpi_enc_handle_.width;
        pstFrame.stVFrame.height = mpi_enc_handle_.height;
        pstFrame.stVFrame.stride = mpi_enc_handle_.hor_stride;
        pstFrame.stVFrame.vstride = mpi_enc_handle_.ver_stride;
        pstFrame.stVFrame.size = mpi_enc_handle_.frame_size;

        memset(mpi_enc_handle_.mmz_vaddr, 0, mpi_enc_handle_.frame_size);
        // info("mmzAlloc paddr = 0x%x, vaddr = 0x%x frame_size:%d\n", mpi_enc_handle_.mmz_paddr, mpi_enc_handle_.mmz_vaddr, mpi_enc_handle_.frame_size);
        if (mpi_enc_handle_.mpp_read_image(mpi_enc_handle_.mmz_vaddr, frame_buffer) < 0)
        {
            error("found last frame\r\n");
            return WEBRTC_VIDEO_CODEC_ERROR;
        }

        int32_t offset = mpi_enc_handle_.hor_stride * mpi_enc_handle_.ver_stride;
        pstFrame.stVFrame.phy_ptr[0] = mpi_enc_handle_.mmz_paddr;
        pstFrame.stVFrame.phy_ptr[1] = mpi_enc_handle_.mmz_paddr + offset;
        pstFrame.stVFrame.vir_ptr[0] = mpi_enc_handle_.mmz_vaddr;
        pstFrame.stVFrame.vir_ptr[1] = mpi_enc_handle_.mmz_vaddr + offset;
        pstFrame.stVFrame.pix_format = HB_PIXEL_FORMAT_NV12;

        s32Ret = mpi_enc_handle_.iot_venc_input(&pstFrame);
        if (s32Ret != 0)
        {
            error("iot_venc_input fail s32Ret = %d !\n", s32Ret);
            return WEBRTC_VIDEO_CODEC_ERROR;
        }

        s32Ret = mpi_enc_handle_.iot_venc_get_output(&pstStream);
        if (s32Ret != 0)
        {
            error("iot_venc_get_output fail s32Ret = %d !\n", s32Ret);
            return WEBRTC_VIDEO_CODEC_ERROR;
        }

        void *ptr = pstStream.pstPack.vir_ptr;
        size_t len = pstStream.pstPack.size;
        static int print_num = 0;
        if (print_num++ % 75 == 0)
            info("ptr = %p len:%u!\n", ptr, len);
#if 0
        static FILE *pFile = NULL;
        if (NULL == pFile)
        {
            pFile = fopen("/data/x3_test.h264", "wb+");
            if (NULL == pFile)
            {
                error("fopen error\n");
                return WEBRTC_VIDEO_CODEC_ERROR;
            }
        }
        fwrite(ptr, len, 1, pFile);
#endif

#if 0
        if (ecovacs::video_common::kVideoCodecJPEG == codec_cfg_.codecType)
        {
            info("jpeg encode ptr = %p len:%u!\n", ptr, len);
            static FILE *pFile = NULL;

            pFile = fopen("/data/x3_test.jpeg", "wb+");
            if (NULL == pFile)
            {
                error("fopen error\n");
                return WEBRTC_VIDEO_CODEC_ERROR;
            }
            fwrite(ptr, len, 1, pFile);
        }
#endif
        uint8_t *encoded_image_data = static_cast<uint8_t *>(ptr);
        video_common::EncodedImageBuffer *encoded_image_buffer = new video_common::EncodedImageBuffer(encoded_image_data, len);
        video_common::CodecSpecificInfo codec_specific_info;
        codec_specific_info.codecType = video_common::kVideoCodecH264;

        if (encoded_image_callback_)
        {
            encoded_image_buffer->AddRef();
            encoded_image_callback_->OnFrame(encoded_image_buffer, &codec_specific_info);
            encoded_image_buffer->RelRef();
        }

        s32Ret = mpi_enc_handle_.iot_venc_output_release(&pstStream);
        if (s32Ret != 0)
        {
            error("iot_venc_output_release fail s32Ret = %d !\n", s32Ret);
            return WEBRTC_VIDEO_CODEC_ERROR;
        }

        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t X3EncoderImpl::RequestKeyFrame()
    {
        if (0 != mpi_enc_handle_.iot_venc_request_idr())
        {
            error("HB_VENC_RequestIDR failed\n");
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
        return WEBRTC_VIDEO_CODEC_OK;
    }

    void X3EncoderImpl::SetRates(const video_common::RateControlParameters &parameters)
    {
        return;
    }

} // namespace ecovacs