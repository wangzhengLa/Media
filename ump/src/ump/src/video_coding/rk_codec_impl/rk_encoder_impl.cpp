#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <mutex>
#include "rk_encoder_impl.h"

namespace ecovacs
{
    // ============================================================================
    // RkEncHandle 实现
    // ============================================================================
    RkEncoderImpl::RkEncHandle::RkEncHandle()
        : width(0), height(0), hor_stride(0), ver_stride(0),
          pixel_format(0), codec_type(RK_VIDEO_ID_Unused),
          frame_count(0), stream_size(0),
          venc_chn(-1), mb_pool(0),
          gop(DEFAULT_GOP), fps(DEFAULT_FPS), qp_init(DEFAULT_QP),
          q_factor(DEFAULT_Q_FACTOR),
          bitrate(0), bitrate_max(0), bitrate_min(0)
    {
        memset(&chn_attr, 0, sizeof(VENC_CHN_ATTR_S));
        memset(&frame_info, 0, sizeof(VIDEO_FRAME_INFO_S));
        memset(&stream_info, 0, sizeof(VENC_STREAM_S));
    }

    RkEncoderImpl::RkEncHandle::~RkEncHandle()
    {
        if (stream_info.pstPack)
        {
            free(stream_info.pstPack);
            stream_info.pstPack = nullptr;
        }
    }

    void RkEncoderImpl::RkEncHandle::Reset()
    {
        width = 0;
        height = 0;
        hor_stride = 0;
        ver_stride = 0;
        pixel_format = 0;
        codec_type = RK_VIDEO_ID_Unused;
        frame_count = 0;
        stream_size = 0;
        venc_chn = -1;
        mb_pool = 0;
        gop = DEFAULT_GOP;
        fps = DEFAULT_FPS;
        qp_init = DEFAULT_QP;
        q_factor = DEFAULT_Q_FACTOR;
        bitrate = 0;
        bitrate_max = 0;
        bitrate_min = 0;
        sps.clear();
        pps.clear();
        memset(&chn_attr, 0, sizeof(VENC_CHN_ATTR_S));
        memset(&frame_info, 0, sizeof(VIDEO_FRAME_INFO_S));
        if (stream_info.pstPack)
        {
            memset(stream_info.pstPack, 0, sizeof(VENC_PACK_S));
        }
    }

    // ============================================================================
    // 辅助函数：从 H.264 码流中提取 SPS/PPS
    // ============================================================================
    static bool ExtractSPSPPS(const uint8_t *data, size_t len,
                              std::vector<uint8_t> &sps, std::vector<uint8_t> &pps)
    {
        sps.clear();
        pps.clear();

        for (size_t i = 0; i + 4 < len; ++i)
        {
            if (data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01)
            {
                size_t start = i + 3;
                if (i + 2 > 0 && data[i + 2] == 0x00 && data[i + 3] == 0x01)
                    start = i + 4;

                if (start >= len)
                    break;

                int nalu_type = data[start] & 0x1F;

                size_t end = len;
                for (size_t j = start + 1; j + 4 < len; ++j)
                {
                    if ((data[j] == 0x00 && data[j + 1] == 0x00 && data[j + 2] == 0x01) ||
                        (data[j] == 0x00 && data[j + 1] == 0x00 && data[j + 2] == 0x00 && data[j + 3] == 0x01))
                    {
                        end = j;
                        break;
                    }
                }

                if (nalu_type == 7)
                {
                    sps.assign(data + start, data + end);
                    LOG_DEBUG("Extracted SPS, size=%zu", sps.size());
                }
                else if (nalu_type == 8)
                {
                    pps.assign(data + start, data + end);
                    LOG_DEBUG("Extracted PPS, size=%zu", pps.size());
                }
            }
        }
        return !sps.empty() && !pps.empty();
    }

    // ============================================================================
    // JPEG 数据验证
    // ============================================================================
    bool RkEncoderImpl::ValidateJPEGData(const uint8_t *data, size_t len)
    {
        if (len < 2)
            return false;

        // 检查 SOI 标记
        if (data[0] != 0xFF || data[1] != 0xD8)
        {
            LOG_ERROR("Invalid JPEG: missing SOI marker (0xFF 0xD8)");
            return false;
        }

        // 检查 EOI 标记（可选，不是所有JPEG都有完整的EOI）
        if (len >= 2 && (data[len - 2] == 0xFF && data[len - 1] == 0xD9))
        {
            LOG_DEBUG("JPEG frame with valid SOI/EOI, size=%zu", len);
        }
        else
        {
            LOG_DEBUG("JPEG frame may be incomplete (missing EOI), size=%zu", len);
        }

        return true;
    }

    // ============================================================================
    // RkEncoderImpl 实现
    // ============================================================================
    RkEncoderImpl::RkEncoderImpl()
        : initialized_(false)
    {
    }

    RkEncoderImpl::~RkEncoderImpl()
    {
        Release();
    }

    // 参数校验和调整函数
    int32_t RkEncoderImpl::ValidateAndAdjustParams(const video_common::VideoCodecConfig *codec_settings)
    {
        // ========== 1. 分辨率校验 ==========
        if (codec_settings->width < MIN_WIDTH || codec_settings->width > MAX_WIDTH ||
            codec_settings->height < MIN_HEIGHT || codec_settings->height > MAX_HEIGHT)
        {
            LOG_ERROR("Resolution %dx%d out of range [%dx%d - %dx%d]",
                      codec_settings->width, codec_settings->height,
                      MIN_WIDTH, MIN_HEIGHT, MAX_WIDTH, MAX_HEIGHT);
            return static_cast<int32_t>(RkEncResult::ResolutionTooLarge);
        }

        // 16对齐检查
        if ((codec_settings->width % ALIGNMENT != 0) || (codec_settings->height % ALIGNMENT != 0))
        {
            LOG_WARN("Resolution %dx%d is not %d-byte aligned, will align to %dx%d",
                     codec_settings->width, codec_settings->height, ALIGNMENT,
                     ALIGN(codec_settings->width, ALIGNMENT),
                     ALIGN(codec_settings->height, ALIGNMENT));
        }

        // ========== 2. 帧率校验 ==========
        if (codec_settings->maxFramerate <= 0 || codec_settings->maxFramerate > 120)
        {
            LOG_ERROR("Invalid framerate: %d, must be 1-120", codec_settings->maxFramerate);
            return static_cast<int32_t>(RkEncResult::InvalidParam);
        }

        // ========== 3. 码率校验和转换（bps -> kbps） ==========
        uint32_t target_kbps = codec_settings->initBitrate / 1000;
        uint32_t max_kbps = codec_settings->maxBitrate / 1000;
        uint32_t min_kbps = codec_settings->minBitrate / 1000;

        if (target_kbps == 0)
        {
            LOG_ERROR("Target bitrate cannot be 0");
            return static_cast<int32_t>(RkEncResult::InvalidParam);
        }

        // 码率范围调整
        if (target_kbps < 100)
        {
            LOG_WARN("Target bitrate %d kbps too low, adjusting to 100 kbps", target_kbps);
            target_kbps = 100;
        }

        if (max_kbps > 0 && max_kbps < target_kbps)
        {
            LOG_WARN("Max bitrate %d kbps < target bitrate %d kbps, adjusting max to target",
                     max_kbps, target_kbps);
            max_kbps = target_kbps;
        }

        if (min_kbps > 0 && min_kbps > target_kbps)
        {
            LOG_WARN("Min bitrate %d kbps > target bitrate %d kbps, adjusting min to target",
                     min_kbps, target_kbps);
            min_kbps = target_kbps;
        }

        // 根据分辨率估算合理码率范围
        uint32_t pixels = codec_settings->width * codec_settings->height;
        uint32_t estimated_min_kbps = (pixels * 8) / 1000;
        uint32_t estimated_max_kbps = (pixels * 80) / 1000;

        if (target_kbps < estimated_min_kbps)
        {
            LOG_WARN("Target bitrate %d kbps may be too low for %dx%d, recommended minimum: %d kbps",
                     target_kbps, codec_settings->width, codec_settings->height, estimated_min_kbps);
        }

        if (target_kbps > estimated_max_kbps)
        {
            LOG_WARN("Target bitrate %d kbps may be too high for %dx%d, recommended maximum: %d kbps",
                     target_kbps, codec_settings->width, codec_settings->height, estimated_max_kbps);
        }

        // 保存调整后的码率
        enc_handle_.bitrate = target_kbps;
        enc_handle_.bitrate_max = max_kbps;
        enc_handle_.bitrate_min = min_kbps;

        // ========== 4. 像素格式校验 ==========
        if (codec_settings->frameType != video_common::kNV12 &&
            codec_settings->frameType != video_common::kNV21 &&
            codec_settings->frameType != video_common::kI420)
        {
            LOG_ERROR("Unsupported pixel format: %d", codec_settings->frameType);
            return static_cast<int32_t>(RkEncResult::UnsupportedPixelFormat);
        }

        // ========== 5. 编码类型校验 ==========
        if (codec_settings->codecType != video_common::kVideoCodecH264 &&
            codec_settings->codecType != video_common::kVideoCodecJPEG)
        {
            LOG_ERROR("Unsupported codec type: %d", codec_settings->codecType);
            return static_cast<int32_t>(RkEncResult::UnsupportedCodec);
        }

        // ========== 6. H.264 特定参数校验 ==========
        if (codec_settings->codecType == video_common::kVideoCodecH264)
        {
            if (codec_settings->codec_specific_.H264.gop <= 0)
            {
                LOG_ERROR("Invalid GOP: %d, must be > 0", codec_settings->codec_specific_.H264.gop);
                return static_cast<int32_t>(RkEncResult::InvalidParam);
            }
            enc_handle_.gop = codec_settings->codec_specific_.H264.gop;

            if (codec_settings->codec_specific_.H264.init_qp < 0 ||
                codec_settings->codec_specific_.H264.init_qp > 51)
            {
                LOG_ERROR("Invalid init_qp: %d, must be 0-51",
                          codec_settings->codec_specific_.H264.init_qp);
                return static_cast<int32_t>(RkEncResult::InvalidParam);
            }
            enc_handle_.qp_init = codec_settings->codec_specific_.H264.init_qp;
        }

        // ========== 7. JPEG 特定参数校验 ==========
        if (codec_settings->codecType == video_common::kVideoCodecJPEG)
        {
            uint32_t jpeg_quality = DEFAULT_Q_FACTOR;
#ifdef JPEG_HAS_QUALITY_FIELD
            jpeg_quality = codec_settings->codec_specific_.JPEG.quality;
#endif
            if (jpeg_quality < 1 || jpeg_quality > 100)
            {
                LOG_WARN("Invalid JPEG quality: %d, using default %d", jpeg_quality, DEFAULT_Q_FACTOR);
                jpeg_quality = DEFAULT_Q_FACTOR;
            }
            enc_handle_.q_factor = jpeg_quality;
        }

        // ========== 8. 打印校验通过的参数 ==========
        LOG_INFO("Parameter validation passed:");
        LOG_INFO("  Resolution: %dx%d (stride: %dx%d)",
                 codec_settings->width, codec_settings->height,
                 ALIGN(codec_settings->width, ALIGNMENT),
                 ALIGN(codec_settings->height, ALIGNMENT));
        LOG_INFO("  Framerate: %d fps", codec_settings->maxFramerate);
        LOG_INFO("  Bitrate: target=%d kbps, max=%d kbps, min=%d kbps",
                 enc_handle_.bitrate, enc_handle_.bitrate_max, enc_handle_.bitrate_min);
        LOG_INFO("  Codec: %s", codec_settings->codecType == video_common::kVideoCodecH264 ? "H.264" : "JPEG");

        return static_cast<int32_t>(RkEncResult::Success);
    }

    int32_t RkEncoderImpl::ConfigureEncoder(const video_common::VideoCodecConfig *codec_settings)
    {
        // 设置分辨率
        enc_handle_.width = codec_settings->width;
        enc_handle_.height = codec_settings->height;
        enc_handle_.hor_stride = ALIGN(enc_handle_.width, ALIGNMENT);
        enc_handle_.ver_stride = ALIGN(enc_handle_.height, ALIGNMENT);
        enc_handle_.fps = codec_settings->maxFramerate;

        // 设置像素格式
        switch (codec_settings->frameType)
        {
        case video_common::kNV12:
            enc_handle_.pixel_format = RK_FMT_YUV420SP;
            break;
        case video_common::kNV21:
            enc_handle_.pixel_format = RK_FMT_YUV420SP_VU;
            break;
        case video_common::kI420:
            enc_handle_.pixel_format = RK_FMT_YUV420P;
            break;
        default:
            return static_cast<int32_t>(RkEncResult::UnsupportedPixelFormat);
        }

        // 配置编码通道属性
        memset(&enc_handle_.chn_attr, 0, sizeof(VENC_CHN_ATTR_S));
        enc_handle_.chn_attr.stVencAttr.enType = enc_handle_.codec_type;
        enc_handle_.chn_attr.stVencAttr.enPixelFormat = (PIXEL_FORMAT_E)enc_handle_.pixel_format;
        enc_handle_.chn_attr.stVencAttr.u32PicWidth = enc_handle_.width;
        enc_handle_.chn_attr.stVencAttr.u32PicHeight = enc_handle_.height;
        enc_handle_.chn_attr.stVencAttr.u32VirWidth = enc_handle_.hor_stride;
        enc_handle_.chn_attr.stVencAttr.u32VirHeight = enc_handle_.ver_stride;
        enc_handle_.chn_attr.stVencAttr.u32StreamBufCnt = 8;
        enc_handle_.chn_attr.stVencAttr.u32BufSize = enc_handle_.hor_stride * enc_handle_.ver_stride * 3 / 2;

        // 设置 RC 参数
        if (enc_handle_.codec_type == RK_VIDEO_ID_AVC)
        {
            enc_handle_.chn_attr.stVencAttr.u32Profile = H264E_PROFILE_HIGH;
            enc_handle_.chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H264VBR;
            enc_handle_.chn_attr.stRcAttr.stH264Vbr.u32BitRate = enc_handle_.bitrate;
            enc_handle_.chn_attr.stRcAttr.stH264Vbr.u32MaxBitRate = enc_handle_.bitrate_max;
            enc_handle_.chn_attr.stRcAttr.stH264Vbr.u32MinBitRate = enc_handle_.bitrate_min;
            enc_handle_.chn_attr.stRcAttr.stH264Vbr.u32Gop = enc_handle_.gop;
            enc_handle_.chn_attr.stRcAttr.stH264Vbr.u32SrcFrameRateNum = enc_handle_.fps;
            enc_handle_.chn_attr.stRcAttr.stH264Vbr.u32SrcFrameRateDen = 1;
            enc_handle_.chn_attr.stRcAttr.stH264Vbr.fr32DstFrameRateNum = enc_handle_.fps;
            enc_handle_.chn_attr.stRcAttr.stH264Vbr.fr32DstFrameRateDen = 1;
            enc_handle_.chn_attr.stRcAttr.stH264Vbr.u32StatTime = 3;

            LOG_INFO("H.264 VBR: bitrate=%d kbps, gop=%d, qp=%d",
                     enc_handle_.bitrate, enc_handle_.gop, enc_handle_.qp_init);
        }
        else if (enc_handle_.codec_type == RK_VIDEO_ID_JPEG)
        {
            enc_handle_.chn_attr.stVencAttr.u32Profile = 0;
            enc_handle_.chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_MJPEGCBR;
            enc_handle_.chn_attr.stRcAttr.stMjpegCbr.u32BitRate = enc_handle_.bitrate * 1000;
            enc_handle_.chn_attr.stRcAttr.stMjpegCbr.u32SrcFrameRateNum = enc_handle_.fps;
            enc_handle_.chn_attr.stRcAttr.stMjpegCbr.u32SrcFrameRateDen = 1;
            enc_handle_.chn_attr.stRcAttr.stMjpegCbr.fr32DstFrameRateNum = enc_handle_.fps;
            enc_handle_.chn_attr.stRcAttr.stMjpegCbr.fr32DstFrameRateDen = 1;

            LOG_INFO("JPEG CBR: bitrate=%d bps, quality hint=%d",
                     enc_handle_.bitrate * 1000, enc_handle_.q_factor);
        }

        return static_cast<int32_t>(RkEncResult::Success);
    }

    int32_t RkEncoderImpl::CreateBufferPool()
    {
        MB_POOL_CONFIG_S mb_pool_cfg;
        memset(&mb_pool_cfg, 0, sizeof(MB_POOL_CONFIG_S));

        size_t y_plane_size = enc_handle_.hor_stride * enc_handle_.ver_stride;
        size_t uv_plane_size = y_plane_size / 2;
        mb_pool_cfg.u64MBSize = y_plane_size + uv_plane_size;
        mb_pool_cfg.u32MBCnt = 16;
        mb_pool_cfg.enAllocType = MB_ALLOC_TYPE_DMA;
        mb_pool_cfg.bPreAlloc = RK_TRUE;

        LOG_INFO("Buffer pool: %zux%zu = %zu bytes, %d buffers",
                 y_plane_size, uv_plane_size, mb_pool_cfg.u64MBSize, mb_pool_cfg.u32MBCnt);

        enc_handle_.mb_pool = RK_MPI_MB_CreatePool(&mb_pool_cfg);
        if (enc_handle_.mb_pool == 0)
        {
            LOG_ERROR("Failed to create buffer pool");
            return static_cast<int32_t>(RkEncResult::BufferPoolFailed);
        }

        return static_cast<int32_t>(RkEncResult::Success);
    }

    int32_t RkEncoderImpl::ReleaseInternal()
    {
        if (enc_handle_.stream_info.pstPack)
        {
            free(enc_handle_.stream_info.pstPack);
            enc_handle_.stream_info.pstPack = nullptr;
        }

        if (enc_handle_.mb_pool)
        {
            RK_MPI_MB_DestroyPool(enc_handle_.mb_pool);
            enc_handle_.mb_pool = 0;
        }

        if (enc_handle_.venc_chn >= 0)
        {
            RK_MPI_VENC_StopRecvFrame(enc_handle_.venc_chn);
            RK_MPI_VENC_DestroyChn(enc_handle_.venc_chn);
            VencChannelManager::GetInstance().Release(enc_handle_.venc_chn);
            enc_handle_.venc_chn = -1;
        }

        enc_handle_.Reset();
        return static_cast<int32_t>(RkEncResult::Success);
    }

    int32_t RkEncoderImpl::InitEncode(const video_common::VideoCodecConfig *codec_settings)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!codec_settings)
        {
            LOG_ERROR("codec_settings is null");
            return static_cast<int32_t>(RkEncResult::InvalidParam);
        }

        // 参数校验
        int32_t ret = ValidateAndAdjustParams(codec_settings);
        if (ret != static_cast<int32_t>(RkEncResult::Success))
        {
            return ret;
        }

        ReleaseInternal();

        // 初始化 MPI 系统
        RkMpiSystem::GetInstance().AddRef();

        // 分配编码通道
        enc_handle_.venc_chn = VencChannelManager::GetInstance().Allocate();
        if (enc_handle_.venc_chn < 0)
        {
            LOG_ERROR("No free VENC channel");
            RkMpiSystem::GetInstance().Release();
            return static_cast<int32_t>(RkEncResult::NoFreeChannel);
        }
        LOG_INFO("Allocated VENC channel %d", enc_handle_.venc_chn);

        // 设置编码类型
        if (codec_settings->codecType == video_common::kVideoCodecH264)
        {
            enc_handle_.codec_type = RK_VIDEO_ID_AVC;
        }
        else if (codec_settings->codecType == video_common::kVideoCodecJPEG)
        {
            enc_handle_.codec_type = RK_VIDEO_ID_JPEG;
        }
        else
        {
            LOG_ERROR("Unsupported codec type %d", codec_settings->codecType);
            VencChannelManager::GetInstance().Release(enc_handle_.venc_chn);
            RkMpiSystem::GetInstance().Release();
            return static_cast<int32_t>(RkEncResult::UnsupportedCodec);
        }

        // 配置编码器
        ret = ConfigureEncoder(codec_settings);
        if (ret != static_cast<int32_t>(RkEncResult::Success))
        {
            ReleaseInternal();
            RkMpiSystem::GetInstance().Release();
            return ret;
        }

        // 创建编码通道
        RK_S32 s32Ret = RK_MPI_VENC_CreateChn(enc_handle_.venc_chn, &enc_handle_.chn_attr);
        if (s32Ret != RK_SUCCESS)
        {
            LOG_ERROR("RK_MPI_VENC_CreateChn failed %d", s32Ret);
            ReleaseInternal();
            RkMpiSystem::GetInstance().Release();
            return static_cast<int32_t>(RkEncResult::InitFailed);
        }
        LOG_INFO("Encoder channel %d created", enc_handle_.venc_chn);

        // 开始接收帧
        VENC_RECV_PIC_PARAM_S recv_param;
        memset(&recv_param, 0, sizeof(VENC_RECV_PIC_PARAM_S));
        recv_param.s32RecvPicNum = -1;
        s32Ret = RK_MPI_VENC_StartRecvFrame(enc_handle_.venc_chn, &recv_param);
        if (s32Ret != RK_SUCCESS)
        {
            LOG_ERROR("RK_MPI_VENC_StartRecvFrame failed %d", s32Ret);
            ReleaseInternal();
            RkMpiSystem::GetInstance().Release();
            return static_cast<int32_t>(RkEncResult::InitFailed);
        }

        // 创建 buffer pool
        ret = CreateBufferPool();
        if (ret != static_cast<int32_t>(RkEncResult::Success))
        {
            ReleaseInternal();
            RkMpiSystem::GetInstance().Release();
            return ret;
        }

        // 分配输出流结构
        if (!enc_handle_.stream_info.pstPack)
        {
            enc_handle_.stream_info.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));
            if (!enc_handle_.stream_info.pstPack)
            {
                LOG_ERROR("Failed to allocate stream info");
                ReleaseInternal();
                RkMpiSystem::GetInstance().Release();
                return static_cast<int32_t>(RkEncResult::StreamFailed);
            }
        }

        enc_handle_.sps.clear();
        enc_handle_.pps.clear();
        initialized_ = true;

        LOG_INFO("InitEncode success on channel %d", enc_handle_.venc_chn);
        return static_cast<int32_t>(RkEncResult::Success);
    }

    int32_t RkEncoderImpl::Release()
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!initialized_)
            return static_cast<int32_t>(RkEncResult::Success);

        ReleaseInternal();
        RkMpiSystem::GetInstance().Release();

        initialized_ = false;
        LOG_INFO("Release success");
        return static_cast<int32_t>(RkEncResult::Success);
    }

    // ============================================================================
    // FillYUVFrame - 优化版，正确处理 stride
    // ============================================================================
    int32_t RkEncoderImpl::FillYUVFrame(const video_common::VideoFrameBuffer *frame_buffer, void *dst)
    {
        uint8_t *y_dst = (uint8_t *)dst;
        uint32_t w = enc_handle_.width;
        uint32_t h = enc_handle_.height;
        uint32_t stride_y = enc_handle_.hor_stride;
        uint32_t stride_uv = enc_handle_.hor_stride;

        // 检查是否可以直接整块拷贝（性能优化）
        bool can_copy_y_direct = (frame_buffer->StrideY() == w && w == stride_y);
        bool can_copy_uv_direct = (frame_buffer->StrideUV() == w && w == stride_uv);

        if (enc_handle_.pixel_format == RK_FMT_YUV420SP) // NV12/NV21
        {
            uint8_t *uv_dst = y_dst + stride_y * h;
            uint32_t uv_h = h / 2;

            // Y 平面
            if (can_copy_y_direct)
            {
                memcpy(y_dst, frame_buffer->DataY(), w * h);
            }
            else
            {
                for (uint32_t row = 0; row < h; ++row)
                {
                    memcpy(y_dst + row * stride_y,
                           frame_buffer->DataY() + row * frame_buffer->StrideY(),
                           w);
                }
            }

            // UV 平面
            if (can_copy_uv_direct)
            {
                memcpy(uv_dst, frame_buffer->DataUV(), w * uv_h);
            }
            else
            {
                for (uint32_t row = 0; row < uv_h; ++row)
                {
                    memcpy(uv_dst + row * stride_uv,
                           frame_buffer->DataUV() + row * frame_buffer->StrideUV(),
                           w);
                }
            }
        }
        else if (enc_handle_.pixel_format == RK_FMT_YUV420P) // I420
        {
            uint8_t *u_dst = y_dst + stride_y * h;
            uint8_t *v_dst = u_dst + (stride_y / 2) * (h / 2);
            uint32_t uv_h = h / 2;
            uint32_t uv_w = w / 2;

            // Y 平面
            if (can_copy_y_direct)
            {
                memcpy(y_dst, frame_buffer->DataY(), w * h);
            }
            else
            {
                for (uint32_t row = 0; row < h; ++row)
                {
                    memcpy(y_dst + row * stride_y,
                           frame_buffer->DataY() + row * frame_buffer->StrideY(),
                           w);
                }
            }

            // U 平面
            bool can_copy_u_direct = (frame_buffer->StrideU() == uv_w && uv_w == stride_y / 2);
            if (can_copy_u_direct)
            {
                memcpy(u_dst, frame_buffer->DataU(), uv_w * uv_h);
            }
            else
            {
                for (uint32_t row = 0; row < uv_h; ++row)
                {
                    memcpy(u_dst + row * (stride_y / 2),
                           frame_buffer->DataU() + row * frame_buffer->StrideU(),
                           uv_w);
                }
            }

            // V 平面
            bool can_copy_v_direct = (frame_buffer->StrideV() == uv_w && uv_w == stride_y / 2);
            if (can_copy_v_direct)
            {
                memcpy(v_dst, frame_buffer->DataV(), uv_w * uv_h);
            }
            else
            {
                for (uint32_t row = 0; row < uv_h; ++row)
                {
                    memcpy(v_dst + row * (stride_y / 2),
                           frame_buffer->DataV() + row * frame_buffer->StrideV(),
                           uv_w);
                }
            }
        }
        else
        {
            LOG_ERROR("Unsupported pixel format: %d", enc_handle_.pixel_format);
            return static_cast<int32_t>(RkEncResult::InvalidParam);
        }

        return static_cast<int32_t>(RkEncResult::Success);
    }

    int32_t RkEncoderImpl::Encode(const video_common::VideoFrameBuffer *frame_buffer)
    {
        auto start_time = std::chrono::steady_clock::now();

        if (!initialized_)
        {
            LOG_ERROR("Encoder not initialized");
            return static_cast<int32_t>(RkEncResult::NotInitialized);
        }

        if (!frame_buffer)
        {
            LOG_ERROR("Frame buffer is null");
            return static_cast<int32_t>(RkEncResult::InvalidParam);
        }

        std::lock_guard<std::mutex> lock(mutex_);

        // 计算所需内存大小
        size_t y_plane_size = enc_handle_.hor_stride * enc_handle_.ver_stride;
        size_t uv_plane_size = y_plane_size / 2;
        size_t frame_size = y_plane_size + uv_plane_size;

        // 获取缓冲区
        MB_BLK blk = RK_MPI_MB_GetMB(enc_handle_.mb_pool, frame_size, RK_TRUE);
        if (!blk)
        {
            LOG_ERROR("Failed to get buffer, size=%zu", frame_size);
            return static_cast<int32_t>(RkEncResult::EncodeFailed);
        }

        // 填充 YUV 数据
        void *pVirAddr = RK_MPI_MB_Handle2VirAddr(blk);
        int32_t ret = FillYUVFrame(frame_buffer, pVirAddr);
        if (ret != static_cast<int32_t>(RkEncResult::Success))
        {
            RK_MPI_MB_ReleaseMB(blk);
            return ret;
        }

        // 刷新缓存
        RK_MPI_SYS_MmzFlushCache(blk, RK_FALSE);

        // 准备帧信息
        memset(&enc_handle_.frame_info, 0, sizeof(VIDEO_FRAME_INFO_S));
        enc_handle_.frame_info.stVFrame.pMbBlk = blk;
        enc_handle_.frame_info.stVFrame.u32Width = enc_handle_.width;
        enc_handle_.frame_info.stVFrame.u32Height = enc_handle_.height;
        enc_handle_.frame_info.stVFrame.u32VirWidth = enc_handle_.hor_stride;
        enc_handle_.frame_info.stVFrame.u32VirHeight = enc_handle_.ver_stride;
        enc_handle_.frame_info.stVFrame.enPixelFormat = (PIXEL_FORMAT_E)enc_handle_.pixel_format;

        // 发送帧到编码器
        RK_S32 s32Ret = RK_MPI_VENC_SendFrame(enc_handle_.venc_chn, &enc_handle_.frame_info, 100);
        RK_MPI_MB_ReleaseMB(blk); // 立即释放，编码器会复制数据

        if (s32Ret != RK_SUCCESS)
        {
            LOG_ERROR("SendFrame failed %d", s32Ret);
            return static_cast<int32_t>(RkEncResult::EncodeFailed);
        }

        // 获取编码后的流
        VENC_STREAM_S stream;
        memset(&stream, 0, sizeof(VENC_STREAM_S));
        stream.pstPack = enc_handle_.stream_info.pstPack;

        s32Ret = RK_MPI_VENC_GetStream(enc_handle_.venc_chn, &stream, -1);
        if (s32Ret < 0 || !stream.pstPack->pMbBlk)
        {
            LOG_ERROR("GetStream failed %d", s32Ret);
            return static_cast<int32_t>(RkEncResult::EncodeFailed);
        }

        void *pEncData = RK_MPI_MB_Handle2VirAddr(stream.pstPack->pMbBlk);
        size_t len = stream.pstPack->u32Len;

        // 回调处理编码数据
        if (len > 0 && encoded_image_callback_)
        {
            // H.264 SPS/PPS 提取
            if (enc_handle_.codec_type == RK_VIDEO_ID_AVC &&
                enc_handle_.sps.empty() && enc_handle_.pps.empty())
            {
                ExtractSPSPPS((uint8_t *)pEncData, len, enc_handle_.sps, enc_handle_.pps);
            }

            // JPEG 数据验证
            if (enc_handle_.codec_type == RK_VIDEO_ID_JPEG)
            {
                ValidateJPEGData((uint8_t *)pEncData, len);
            }

            // 创建编码图像缓冲区
            video_common::EncodedImageBuffer *encoded_buffer =
                new video_common::EncodedImageBuffer((uint8_t *)pEncData, len);

            // 填充编码信息
            video_common::CodecSpecificInfo codec_info;
            if (enc_handle_.codec_type == RK_VIDEO_ID_AVC)
            {
                codec_info.codecType = video_common::kVideoCodecH264;

                // 检测是否为 IDR 帧
                bool is_idr = false;
                uint8_t *data = (uint8_t *)pEncData;
                for (size_t i = 0; i + 4 < len; ++i)
                {
                    if (data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01)
                    {
                        int nalu_type = data[i + 3] & 0x1F;
                        if (nalu_type == 5)
                        {
                            is_idr = true;
                            break;
                        }
                    }
                }
                codec_info.codecSpecific.H264.idr_frame = is_idr;

                // 添加 SPS/PPS
                if (is_idr && !enc_handle_.sps.empty() && !enc_handle_.pps.empty())
                {
                    std::vector<uint8_t> sps_pps;
                    sps_pps.insert(sps_pps.end(), enc_handle_.sps.begin(), enc_handle_.sps.end());
                    sps_pps.insert(sps_pps.end(), enc_handle_.pps.begin(), enc_handle_.pps.end());
                    codec_info.codecSpecific.H264.sps_pps = sps_pps.data();
                    codec_info.codecSpecific.H264.sps_pps_len = sps_pps.size();
                }
            }
            else if (enc_handle_.codec_type == RK_VIDEO_ID_JPEG)
            {
                codec_info.codecType = video_common::kVideoCodecJPEG;
            }

            // 调用回调
            encoded_buffer->AddRef();
            encoded_image_callback_->OnFrame(encoded_buffer, &codec_info);
            encoded_buffer->RelRef();
        }

        // 更新统计信息
        auto end_time = std::chrono::steady_clock::now();
        uint32_t elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
        perf_stats_.Update(elapsed_us, len);

        enc_handle_.frame_count++;
        enc_handle_.stream_size += len;

        // 释放流
        RK_MPI_VENC_ReleaseStream(enc_handle_.venc_chn, &stream);

        return static_cast<int32_t>(RkEncResult::Success);
    }

    int32_t RkEncoderImpl::RequestKeyFrame()
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!initialized_)
        {
            LOG_ERROR("Encoder not initialized");
            return static_cast<int32_t>(RkEncResult::NotInitialized);
        }

        if (enc_handle_.codec_type != RK_VIDEO_ID_AVC)
        {
            LOG_DEBUG("RequestKeyFrame only supported for H.264");
            return static_cast<int32_t>(RkEncResult::Success);
        }

        RK_S32 s32Ret = RK_MPI_VENC_RequestIDR(enc_handle_.venc_chn, RK_FALSE);
        if (s32Ret != RK_SUCCESS)
        {
            LOG_ERROR("RequestIDR failed %d", s32Ret);
            return static_cast<int32_t>(RkEncResult::EncodeFailed);
        }

        LOG_DEBUG("Key frame requested");
        return static_cast<int32_t>(RkEncResult::Success);
    }

    void RkEncoderImpl::SetRates(const video_common::RateControlParameters &parameters)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!initialized_)
        {
            LOG_ERROR("Encoder not initialized");
            return;
        }

        if (enc_handle_.codec_type != RK_VIDEO_ID_AVC)
        {
            LOG_DEBUG("SetRates only supported for H.264");
            return;
        }

        uint32_t new_bitrate_kbps = parameters.target_bitrate;
        if (new_bitrate_kbps < 100)
        {
            LOG_WARN("Bitrate %d kbps too low, adjusting to 100 kbps", new_bitrate_kbps);
            new_bitrate_kbps = 100;
        }

        VENC_CHN_ATTR_S chn_attr;
        RK_S32 s32Ret = RK_MPI_VENC_GetChnAttr(enc_handle_.venc_chn, &chn_attr);
        if (s32Ret != RK_SUCCESS)
        {
            LOG_ERROR("GetChnAttr failed %d", s32Ret);
            return;
        }

        if (chn_attr.stRcAttr.enRcMode == VENC_RC_MODE_H264VBR)
        {
            chn_attr.stRcAttr.stH264Vbr.u32BitRate = new_bitrate_kbps;
        }
        else if (chn_attr.stRcAttr.enRcMode == VENC_RC_MODE_H264CBR)
        {
            chn_attr.stRcAttr.stH264Cbr.u32BitRate = new_bitrate_kbps;
        }
        else
        {
            LOG_ERROR("Unsupported RC mode for dynamic rate change");
            return;
        }

        s32Ret = RK_MPI_VENC_SetChnAttr(enc_handle_.venc_chn, &chn_attr);
        if (s32Ret != RK_SUCCESS)
        {
            LOG_ERROR("SetChnAttr failed %d", s32Ret);
        }
        else
        {
            enc_handle_.bitrate = new_bitrate_kbps;
            LOG_INFO("Bitrate updated to %d kbps", new_bitrate_kbps);
        }
    }

    void RkEncoderImpl::GetPerformanceStats(EncoderPerformanceStats &stats) const
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // 手动拷贝每个成员
        stats.total_encode_time_us = perf_stats_.total_encode_time_us.load();
        stats.max_encode_time_us = perf_stats_.max_encode_time_us.load();
        stats.min_encode_time_us = perf_stats_.min_encode_time_us.load();
        stats.encode_count = perf_stats_.encode_count.load();
        stats.total_encoded_bytes = perf_stats_.total_encoded_bytes.load();
    }

} // namespace ecovacs