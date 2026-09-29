#include <string.h>
#include <com/amazonaws/kinesis/video/webrtcclient/log.h>
#include "osp.h"
#include "video_common_defines.h"
#include "x3_encoder_handle.h"

namespace ecovacs
{
    MpiEncHandle::MpiEncHandle() : width(0),
                                   height(0),
                                   hor_stride(0),
                                   ver_stride(0),
                                   mmz_paddr(0),
                                   channel(0),
                                   mmz_vaddr(nullptr) {}

    MpiEncHandle::~MpiEncHandle()
    {
    }

    int32_t MpiEncHandle::mpp_read_image(void *mpp_buffer, const video_common::VideoFrameBuffer *frame_buffer)
    {
        int32_t ret = 0;
        PIXEL_FORMAT_E format = HB_PIXEL_FORMAT_NONE;

        switch (frame_buffer->type())
        {
        case video_common::kI420:
            format = HB_PIXEL_FORMAT_YUV420P;
            break;
        case video_common::kNV12:
            format = HB_PIXEL_FORMAT_NV12;
            break;
        case video_common::kNV21:
            format = HB_PIXEL_FORMAT_NV21;
            break;
        case video_common::kI444:
            format = HB_PIXEL_FORMAT_YUV444P;
            break;
        default:
            format = HB_PIXEL_FORMAT_YUV420P;
            break;
        }
        if (format != this->pixFmt)
        {
            error("input format[%u] !=  encode this->format[%u]", format, this->pixFmt);
            return -1;
        }

        uint8_t *buf_y = (uint8_t *)mpp_buffer;
        uint8_t *buf_u = nullptr;
        uint8_t *buf_uv = nullptr;
        uint8_t *buf_v = nullptr;

        switch (frame_buffer->type())
        {
        case video_common::kI420:
        {
            buf_u = buf_y + hor_stride * ver_stride;
            buf_v = buf_u + hor_stride * ver_stride / 4;
            /* Y分量拷贝 */
            for (int32_t row = 0; row < height; row++)
            {
                memcpy(buf_y + row * hor_stride, frame_buffer->DataY() + row * frame_buffer->StrideY(), width);
            }
            /* U分量拷贝 */
            for (int32_t row = 0; row < height / 2; row++)
            {
                memcpy(buf_u + row * hor_stride / 2, frame_buffer->DataU() + row * frame_buffer->StrideU() / 2, width / 2);
            }
            /* V分量拷贝 */
            for (int32_t row = 0; row < height / 2; row++)
            {
                memcpy(buf_v + row * hor_stride / 2, frame_buffer->DataV() + row * frame_buffer->StrideV() / 2, width / 2);
            }
        }
        break;
        case video_common::kNV12:
        case video_common::kNV21:
        {
            buf_uv = buf_y + hor_stride * ver_stride;
            /* Y分量拷贝 */
            for (int32_t row = 0; row < height; row++)
            {
                memcpy(buf_y + row * hor_stride, frame_buffer->DataY() + row * frame_buffer->StrideY(), width);
            }
            /* UV分量拷贝 */
            for (int32_t row = 0; row < height / 2; row++)
            {
                memcpy(buf_uv + row * hor_stride, frame_buffer->DataUV() + row * frame_buffer->StrideUV(), width);
            }
        }
        break;
        default:
        {
            error("read image do not support fmt %d\n", frame_buffer->type());
            ret = -1;
        }
        break;
        }

        return ret;
    }

    int32_t MpiEncHandle::iot_venc_init(const video_common::VideoCodecConfig *codec_settings)
    {
        int32_t ret = 0;
        PIXEL_FORMAT_E pixFmt = HB_PIXEL_FORMAT_NV12;
        VENC_RC_ATTR_S *pstRcParam;

        memset(&vencChnAttr, 0, sizeof(VENC_CHN_ATTR_S));
        if (ecovacs::video_common::kVideoCodecH264 == codec_settings->codecType)
            vencChnAttr.stVencAttr.enType = PT_H264;
        else if (ecovacs::video_common::kVideoCodecJPEG == codec_settings->codecType)
            vencChnAttr.stVencAttr.enType = PT_JPEG;
        else
            vencChnAttr.stVencAttr.enType = PT_H264;

        vencChnAttr.stVencAttr.u32PicWidth = codec_settings->width;
        vencChnAttr.stVencAttr.u32PicHeight = codec_settings->height;

        vencChnAttr.stVencAttr.enMirrorFlip = DIRECTION_NONE;
        vencChnAttr.stVencAttr.enRotation = CODEC_ROTATION_0;
        vencChnAttr.stVencAttr.stCropCfg.bEnable = HB_FALSE;

        if (PT_JPEG == vencChnAttr.stVencAttr.enType)
        {
            vencChnAttr.stVencAttr.enPixelFormat = pixFmt;
            vencChnAttr.stVencAttr.u32BitStreamBufferCount = 1;
            vencChnAttr.stVencAttr.u32FrameBufferCount = 2;
            vencChnAttr.stVencAttr.bExternalFreamBuffer = HB_TRUE;
            vencChnAttr.stVencAttr.stAttrJpeg.dcf_enable = HB_FALSE;
            vencChnAttr.stVencAttr.stAttrJpeg.quality_factor = 0;
            vencChnAttr.stVencAttr.stAttrJpeg.restart_interval = 0;
        }
        else
        {
            vencChnAttr.stVencAttr.enPixelFormat = pixFmt;
            vencChnAttr.stVencAttr.u32BitStreamBufferCount = 5;
            vencChnAttr.stVencAttr.u32FrameBufferCount = 5;
            vencChnAttr.stVencAttr.bExternalFreamBuffer = HB_TRUE;
        }

        if (PT_H264 == vencChnAttr.stVencAttr.enType)
        {
            vencChnAttr.stRcAttr.enRcMode = VENC_RC_MODE_H264VBR;
            vencChnAttr.stRcAttr.stH264Vbr.bQpMapEnable = HB_TRUE;
            vencChnAttr.stRcAttr.stH264Vbr.u32IntraQp = codec_settings->codec_specific_.H264.init_qp;
            vencChnAttr.stRcAttr.stH264Vbr.u32IntraPeriod = 15;
            vencChnAttr.stRcAttr.stH264Vbr.u32FrameRate = codec_settings->maxFramerate;
            if (ecovacs::video_common::VideoCodecProfile::kProfileBase == codec_settings->codec_specific_.H264.profile)
                vencChnAttr.stVencAttr.stAttrH264.h264_profile = HB_H264_PROFILE_BP;
            else if (ecovacs::video_common::VideoCodecProfile::kProfileMain == codec_settings->codec_specific_.H264.profile)
                vencChnAttr.stVencAttr.stAttrH264.h264_profile = HB_H264_PROFILE_MP;
            else if (ecovacs::video_common::VideoCodecProfile::kProfileHigh == codec_settings->codec_specific_.H264.profile)
                vencChnAttr.stVencAttr.stAttrH264.h264_profile = HB_H264_PROFILE_HP;
            else
                vencChnAttr.stVencAttr.stAttrH264.h264_profile = HB_H264_PROFILE_UNSPECIFIED;
            vencChnAttr.stVencAttr.stAttrH264.h264_level = HB_H264_LEVEL_UNSPECIFIED;
        }
        vencChnAttr.stGopAttr.u32GopPresetIdx = 6;
        vencChnAttr.stGopAttr.s32DecodingRefreshType = 2;

        info("channel:%d\n", channel);

        ret = HB_VENC_CreateChn(channel, &vencChnAttr);
        if (ret != 0)
        {
            error("HB_VENC_CreateChn %d failed, %d.\n", 0, ret);
            return -1;
        }
        // stModParam.u32OneStreamBuffer = 0;
        // ret = HB_VENC_SetModParam(channel, &stModParam);
        // if (ret != 0)
        // {
        //     error("HB_VENC_SetModParam %d failed, %d.\n", channel, ret);
        //     return -1;
        // }

        // int stroi_map_len = ((width + 15) >> 4) * ((height + 15) >> 4);
        // memset(&pstRoiAttr, 0, sizeof(VENC_ROI_ATTR_S));
        // pstRoiAttr.roi_enable = HB_TRUE;
        // pstRoiAttr.roi_map_array_count = stroi_map_len;
        // pstRoiAttr.roi_map_array = (unsigned char *)malloc(stroi_map_len * sizeof(unsigned char));
        // for (int i = 0; i < stroi_map_len; i++)
        // {
        //     pstRoiAttr.roi_map_array[i] = 51;
        // }
        // ret = HB_VENC_SetRoiAttr(channel, &pstRoiAttr);
        // if (ret != 0)
        // {
        //     error("HB_VENC_SetRoiAttr %d failed, %dx%d\n", channel, width, height);
        //     return -1;
        // }

        ret = HB_VENC_SetChnAttr(channel, &vencChnAttr); // config
        if (ret != 0)
        {
            error("HB_VENC_SetChnAttr failed ret:%d\n", ret);
            return -1;
        }

        return ret;
    }

    int32_t MpiEncHandle::iot_venc_start()
    {
        int32_t ret = 0;
        pstRecvParam.s32RecvPicNum = 0; // unchangable

        ret = HB_VENC_StartRecvFrame(channel, &pstRecvParam);
        if (ret != 0)
        {
            error("HB_VENC_StartRecvFrame failed\n");
            return -1;
        }
        return ret;
    }

    int32_t MpiEncHandle::iot_venc_input(VIDEO_FRAME_S *pstFrame)
    {
        int32_t ret = 0;
        ret = HB_VENC_SendFrame(channel, pstFrame, 3000);
        if (ret < 0)
        {
            error("HB_VENC_SendFrame error!!!\n");
        }
        return ret;
    }

    int32_t MpiEncHandle::iot_venc_get_output(VIDEO_STREAM_S *pstStream)
    {
        int32_t ret = 0;
        ret = HB_VENC_GetStream(channel, pstStream, 3000);
        if (ret < 0)
        {
            error("HB_VENC_GetStream error!!!\n");
        }
        return ret;
    }

    int32_t MpiEncHandle::iot_venc_output_release(VIDEO_STREAM_S *pstStream)
    {
        int32_t ret = 0;
        ret = HB_VENC_ReleaseStream(channel, pstStream);
        if (ret != 0)
        {
            error("HB_VENC_ReleaseStream failed\n");
            return ret;
        }
        return ret;
    }

    int32_t MpiEncHandle::iot_venc_stop()
    {
        int32_t ret = 0;
        ret = HB_VENC_StopRecvFrame(channel);
        if (ret != 0)
        {
            error("HB_VENC_StopRecvFrame failed ret:%d channel:%d\n", ret, channel);
            return ret;
        }
        return ret;
    }

    int32_t MpiEncHandle::iot_venc_deinit()
    {
        int32_t ret = 0;
        ret = HB_VENC_DestroyChn(channel);
        if (ret != 0)
        {
            error("HB_VENC_DestroyChn failed\n");
            return -1;
        }
        return ret;
    }

    int32_t MpiEncHandle::iot_venc_request_idr()
    {
        int32_t ret = 0;
        ret = HB_VENC_RequestIDR(channel);
        if (ret != 0)
        {
            error("HB_VENC_RequestIDR failed\n");
            return -1;
        }
        return ret;
    }

    int32_t MpiEncHandle::iot_vp_init()
    {
        int32_t ret = 0;
        VP_CONFIG_S struVpConf;
        memset(&struVpConf, 0x00, sizeof(VP_CONFIG_S));
        struVpConf.u32MaxPoolCnt = 32;
        ret = HB_VP_SetConfig(&struVpConf);
        if (ret != 0)
        {
            error("HB_VP_SetConfig failed ret:%d\r\n",ret);
            return ret;
        }
        ret = HB_VP_Init();
        if (ret != 0)
        {
            error("HB_VP_Init failed\r\n");
        }
        return ret;
    }

    int32_t MpiEncHandle::iot_vp_deinit()
    {
        int32_t ret = 0;
        ret = HB_VP_Exit();
        if (ret == 0)
        {
            info("HB_VP_Exit success\n");
        }
        else
        {
            error("HB_VP_Exit failed\r\n");
        }
        return ret;
    }

    int32_t MpiEncHandle::iot_sys_alloc(uint64_t *pu64PhyAddr, void **ppVirAddr, uint32_t u32Len)
    {
        int32_t ret = 0;
        ret = HB_SYS_Alloc(pu64PhyAddr, ppVirAddr, u32Len);
        if (ret != 0)
        {
            error("HB_SYS_Alloc failed\r\n");
        }
        return ret;
    }

    int32_t MpiEncHandle::iot_sys_free(uint64_t u64PhyAddr, void *pVirAddr)
    {
        int32_t ret = 0;
        ret = HB_SYS_Free(u64PhyAddr, pVirAddr);
        if (ret == 0)
        {
            info("mmzFree paddr = 0x%x, vaddr = 0x%x\n", mmz_paddr, mmz_vaddr);
        }
        else
        {
            error("HB_SYS_Free failed\r\n");
        }
        return ret;
    }

} // namespace ecovacs