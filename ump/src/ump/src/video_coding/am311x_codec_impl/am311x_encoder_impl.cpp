/**
 * @file    am311x_encoder_impl.cpp
 * @brief   AM311x H.264 编码器实现
 */

#include <cstring>
#include <chrono>
#include <unistd.h>

#include "log.h"
#include "video_common_defines.h"
#include "am311x_encoder_impl.h"

#include <linux/videodev2.h>

namespace ecovacs
{

    // ============================================================================
    // 常量
    // ============================================================================
    static constexpr uint32_t OUTPUT_MAX = 8U * 1024U * 1024U;   // 8MB
    static constexpr uint32_t OUTPUT_MIN = 1024U * 1024U;        // 1MB
    static constexpr int DEFAULT_TIMEOUT_MS = 5000;

    // ============================================================================
    // 默认码率估算
    // ============================================================================
    static uint32_t default_bitrate_kbps(uint32_t width, uint32_t height, uint32_t fps)
    {
        uint64_t pixels = (uint64_t)width * height;
        uint32_t bpp_x1000;
        if (pixels <= 640U * 480U)      bpp_x1000 = 300;  // 0.30 bpp
        else if (pixels <= 1280U * 720U) bpp_x1000 = 150;  // 0.15 bpp
        else                            bpp_x1000 = 90;   // 0.09 bpp

        uint64_t bitrate = pixels * fps * bpp_x1000 / 1000U;
        uint32_t clamped = 0;
        if (bitrate < 512000U)      clamped = 512;
        else if (bitrate > 20000000U) clamped = 20000;
        else                        clamped = (uint32_t)(bitrate / 1000);
        return clamped * 1000;   // 返回 bps
    }

    // ============================================================================
    // 构造 / 析构
    // ============================================================================
    Am311xEncoderImpl::Am311xEncoderImpl() = default;

    Am311xEncoderImpl::~Am311xEncoderImpl()
    {
        Release();
    }

    // ============================================================================
    // SetEncoderParameters —— 设置编码器参数
    //   1. PATH_CONFIG (codec=H264, max_dim, bitrate, isp_id=-1)
    //   2. IN (分辨率, 像素格式, dir=NONE, frc=1:1)
    //   3. OUT_ENC_PARAM (codec=H264, gop, profile=HIGH)
    //   4. OUT_RATE_CONTROL (VBR, bitrate, fps, init/max/min I-P/QP)
    // ============================================================================
    int Am311xEncoderImpl::SetEncoderParameters()
    {
        MH_VIDEO_CODEC mh_codec = (codec_type_ == video_common::kVideoCodecJPEG)
                                     ? MH_CODEC_JPEG : MH_CODEC_H264;

        // ---- 1. PATH_CONFIG ----
        MH_VIDEOENC_PATH_CONFIG path_cfg = {};
        path_cfg.max_mem.codec_type = mh_codec;
        path_cfg.max_mem.max_dim.w = width_;
        path_cfg.max_mem.max_dim.h = height_;
        path_cfg.max_mem.bitrate = bitrate_;
        path_cfg.isp_id = 0xffffffffU;  // 忽略 ISP
        auto ret = mh_videoenc_set(path_, MH_VIDEOENC_PARAM_PATH_CONFIG, &path_cfg);
        if (ret != MH_OK) {
            ump_error("[AM311X_VENC] set PATH_CONFIG failed: %d\n", ret);
            return -1;
        }

        // ---- 2. IN ----
        MH_VIDEOENC_IN input = {};
        input.dim.w = width_;
        input.dim.h = height_;
        input.pxl_fmt = encoder_format_;
        input.dir = MH_VIDEO_DIR_NONE;
        input.frc = MH_VIDEO_FRC_RATIO(1, 1);
        ret = mh_videoenc_set(path_, MH_VIDEOENC_PARAM_IN, &input);
        if (ret != MH_OK) {
            ump_error("[AM311X_VENC] set IN failed: %d\n", ret);
            return -1;
        }

        // ---- 3. OUT_ENC_PARAM ----
        MH_VIDEOENC_OUT output = {};
        output.codec_type = mh_codec;
        if (codec_type_ == video_common::kVideoCodecJPEG) {
            output.jpeg.image_quality = jpeg_quality_;
        } else {
            output.h26x.gop_num = gop_;
            output.h26x.profile = MH_H264E_HIGH_PROFILE;
            output.h26x.level_idc = (MH_VIDEOENC_LEVEL)0;  // auto
        }
        ret = mh_videoenc_set(path_, MH_VIDEOENC_PARAM_OUT_ENC_PARAM, &output);
        if (ret != MH_OK) {
            ump_error("[AM311X_VENC] set OUT_ENC_PARAM failed: %d\n", ret);
            return -1;
        }

        // 验证 GOP 是否被编码器接受（get 回来确认）
        if (codec_type_ != video_common::kVideoCodecJPEG) {
            MH_VIDEOENC_OUT verify = {};
            auto r2 = mh_videoenc_get(path_, MH_VIDEOENC_PARAM_OUT_ENC_PARAM, &verify);
            ump_info("[AM311X_VENC] GOP verify: set=%u, get_back=%u (ret=%d)\n",
                     gop_, (r2 == MH_OK) ? verify.h26x.gop_num : 0xFFFFFFFF, r2);
        }

        // ---- 4. OUT_RATE_CONTROL（仅 H.264 需要） ----
        if (codec_type_ == video_common::kVideoCodecH264) {
            MH_H26XENC_RATE_CONTROL rc = {};
            rc.rc_mode = MH_RC_MODE_VBR;
            // 注意：MHAL 内部无论 CBR/VBR 都从 cbr 字段读取参数（union 共享内存）
            rc.cbr.bitrate = bitrate_;
            rc.cbr.frame_rate_base = fps_;
            rc.cbr.frame_rate_incr = 1;
            rc.cbr.init_i_qp = init_qp_;
            rc.cbr.max_i_qp = max_qp_;
            rc.cbr.min_i_qp = min_qp_;
            rc.cbr.init_p_qp = init_qp_;
            rc.cbr.max_p_qp = max_qp_;
            rc.cbr.min_p_qp = min_qp_;
            ret = mh_videoenc_set(path_, MH_VIDEOENC_PARAM_OUT_RATE_CONTROL, &rc);
            if (ret != MH_OK) {
                ump_error("[AM311X_VENC] set OUT_RATE_CONTROL failed: %d\n", ret);
                return -1;
            }
        }

        ump_info("[AM311X_VENC] parameters set: %ux%u %s %s %ufps\n",
                 width_, height_,
                 fourcc_ == V4L2_PIX_FMT_NV12 ? "NV12" :
                 fourcc_ == V4L2_PIX_FMT_YUYV ? "YUYV" :
                 fourcc_ == V4L2_PIX_FMT_YUV420 ? "I420" : "?",
                 codec_type_ == video_common::kVideoCodecJPEG ? "JPEG" : "H264",
                 fps_);
        return 0;
    }

    // ============================================================================
    // CopyFrameToDma —— 把 VideoFrameBuffer 拷到 dma buffer virt_addr
    // ============================================================================
    int Am311xEncoderImpl::CopyFrameToDma(const video_common::VideoFrameBuffer *src, void *dst_virt)
    {
        if (!src || !dst_virt)
            return -1;

        uint8_t *dst = (uint8_t *)dst_virt;
        uint32_t w = width_;
        uint32_t h = height_;
        uint32_t stride = bytesperline_;

        switch (fourcc_) {
        case V4L2_PIX_FMT_NV12: {
            uint8_t *dst_y = dst;
            uint8_t *dst_uv = dst + stride * h;
            uint32_t uv_h = h / 2;

            // Y plane
            int src_stride_y = src->StrideY();
            if (src_stride_y == (int)w && (uint32_t)src_stride_y == stride) {
                memcpy(dst_y, src->DataY(), w * h);
            } else {
                for (uint32_t row = 0; row < h; row++)
                    memcpy(dst_y + row * stride, src->DataY() + row * src_stride_y, w);
            }

            // UV plane
            int src_stride_uv = src->StrideUV();
            if (src_stride_uv == (int)w && (uint32_t)src_stride_uv == stride) {
                memcpy(dst_uv, src->DataUV(), w * uv_h);
            } else {
                for (uint32_t row = 0; row < uv_h; row++)
                    memcpy(dst_uv + row * stride, src->DataUV() + row * src_stride_uv, w);
            }
            break;
        }
        case V4L2_PIX_FMT_YUYV: {
            // packed YUYV, 每像素 2 字节
            uint32_t src_stride = (uint32_t)src->StrideY();
            uint32_t row_bytes = w * 2;
            const uint8_t *src_ptr = src->DataY();
            if (src_stride == stride) {
                memcpy(dst, src_ptr, stride * h);
            } else {
                for (uint32_t row = 0; row < h; row++)
                    memcpy(dst + row * stride, src_ptr + row * src_stride, row_bytes);
            }
            break;
        }
        case V4L2_PIX_FMT_YUV420: {
            uint8_t *dst_y = dst;
            uint8_t *dst_u = dst + stride * h;
            uint8_t *dst_v = dst_u + (stride / 2) * (h / 2);
            uint32_t half_w = w / 2;
            uint32_t half_h = h / 2;

            int src_stride_y = src->StrideY();
            if (src_stride_y == (int)w && (uint32_t)src_stride_y == stride)
                memcpy(dst_y, src->DataY(), w * h);
            else
                for (uint32_t row = 0; row < h; row++)
                    memcpy(dst_y + row * stride, src->DataY() + row * src_stride_y, w);

            int src_stride_u = src->StrideU();
            uint32_t dst_uv_stride = stride / 2;
            if (src_stride_u == (int)half_w && (uint32_t)src_stride_u == dst_uv_stride)
                memcpy(dst_u, src->DataU(), half_w * half_h);
            else
                for (uint32_t row = 0; row < half_h; row++)
                    memcpy(dst_u + row * dst_uv_stride, src->DataU() + row * src_stride_u, half_w);

            int src_stride_v = src->StrideV();
            if (src_stride_v == (int)half_w && (uint32_t)src_stride_v == dst_uv_stride)
                memcpy(dst_v, src->DataV(), half_w * half_h);
            else
                for (uint32_t row = 0; row < half_h; row++)
                    memcpy(dst_v + row * dst_uv_stride, src->DataV() + row * src_stride_v, half_w);
            break;
        }
        default:
            ump_error("[AM311X_VENC] unsupported fourcc: 0x%08x\n", fourcc_);
            return -1;
        }
        return 0;
    }

    // ============================================================================
    // InitEncode —— 初始化编码器
    // ============================================================================
    int32_t Am311xEncoderImpl::InitEncode(const video_common::VideoCodecConfig *codec_settings)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!codec_settings) {
            ump_error("[AM311X_VENC] InitEncode: null config\n");
            return -1;
        }
        if (initialized_) {
            ump_warn("[AM311X_VENC] InitEncode: already initialized, release first\n");
            return -1;
        }
        if (codec_settings->codecType != video_common::kVideoCodecH264 &&
            codec_settings->codecType != video_common::kVideoCodecJPEG) {
            ump_error("[AM311X_VENC] only H.264/JPEG supported, requested codecType=%d\n", codec_settings->codecType);
            return -1;
        }
        if (codec_settings->codecType == video_common::kVideoCodecH264 &&
            (codec_settings->width % 4 || codec_settings->height % 2)) {
            ump_error("[AM311X_VENC] H.264 requires width%%4==0 height%%2==0, got %dx%d\n",
                      codec_settings->width, codec_settings->height);
            return -1;
        }

        // ---- 保存编码类型 ----
        codec_type_ = codec_settings->codecType;

        // ---- 重置统计和时间戳基准 ----
        frame_sequence_ = 0;
        first_timestamp_ms_ = 0;
        encoded_frames_ = 0;
        encoded_bytes_ = 0;

        // ---- 像素格式映射 ----
        width_ = codec_settings->width;
        height_ = codec_settings->height;
        switch (codec_settings->frameType) {
        case video_common::kNV12:
            fourcc_ = V4L2_PIX_FMT_NV12;
            encoder_format_ = MH_VIDEOENC_PXLFMT_YUV420_NV12;
            break;
        case video_common::kI420:
            fourcc_ = V4L2_PIX_FMT_YUV420;
            encoder_format_ = MH_VIDEOENC_PXLFMT_YUV420_I420;
            break;
        case video_common::kYUY2:
            fourcc_ = V4L2_PIX_FMT_YUYV;
            encoder_format_ = MH_VIDEOENC_PXLFMT_YUV422_YUYV;
            break;
        default:
            ump_error("[AM311X_VENC] unsupported frameType=%d\n", codec_settings->frameType);
            return -1;
        }

        // ---- 计算 stride + sizeimage ----
        uint32_t aligned_w = (width_ + 15U) & ~15U;
        switch (fourcc_) {
        case V4L2_PIX_FMT_NV12:
            bytesperline_ = aligned_w;
            sizeimage_ = bytesperline_ * height_ * 3U / 2U;
            break;
        case V4L2_PIX_FMT_YUYV:
            bytesperline_ = aligned_w * 2U;
            sizeimage_ = bytesperline_ * height_;
            break;
        case V4L2_PIX_FMT_YUV420:
            bytesperline_ = aligned_w;
            sizeimage_ = bytesperline_ * height_ * 3U / 2U;
            break;
        }

        // ---- 编码参数 ----
        fps_ = codec_settings->maxFramerate > 0 ? (uint32_t)codec_settings->maxFramerate : 30;
        if (codec_type_ == video_common::kVideoCodecJPEG) {
            // JPEG：不需要 gop/bitrate，使用质量参数
            jpeg_quality_ = 85;  // 默认质量
            bitrate_ = 0;
            gop_ = 0;
        } else {
            gop_ = codec_settings->codec_specific_.H264.gop > 0
                       ? (uint32_t)codec_settings->codec_specific_.H264.gop : 30;
            ump_info("[AM311X_VENC] GOP config: raw_gop=%d, final_gop=%u\n",
                     codec_settings->codec_specific_.H264.gop, gop_);
            uint32_t br = codec_settings->initBitrate > 0
                              ? (uint32_t)codec_settings->initBitrate
                              : default_bitrate_kbps(width_, height_, fps_);
            bitrate_ = br > 0 ? br : default_bitrate_kbps(width_, height_, fps_);

            // QP：优先使用配置文件，否则用默认值
            init_qp_ = codec_settings->codec_specific_.H264.init_qp > 0
                           ? (uint32_t)codec_settings->codec_specific_.H264.init_qp : 26;
            min_qp_ = codec_settings->codec_specific_.H264.min_qp > 0
                          ? (uint32_t)codec_settings->codec_specific_.H264.min_qp : 20;
            max_qp_ = codec_settings->codec_specific_.H264.max_qp > 0
                          ? (uint32_t)codec_settings->codec_specific_.H264.max_qp : 45;
            ump_info("[AM311X_VENC] QP config: init=%u min=%u max=%u\n",
                     init_qp_, min_qp_, max_qp_);
        }
        timeout_ms_ = DEFAULT_TIMEOUT_MS;

        // ---- Init MHAL ----
        auto ret = mh_videoenc_init();
        if (ret != MH_OK) {
            ump_error("[AM311X_VENC] mh_videoenc_init failed: %d\n", ret);
            return -1;
        }
        initialized_ = true;

        ret = mh_videoenc_open(MH_VIDEOENC_0_IN_0, MH_VIDEOENC_0_OUT_0, &path_);
        if (ret != MH_OK) {
            ump_error("[AM311X_VENC] mh_videoenc_open failed: %d\n", ret);
            Release();
            return -1;
        }
        opened_ = true;

        if (SetEncoderParameters() != 0) {
            Release();
            return -1;
        }

        ret = mh_videoenc_start(path_);
        if (ret != MH_OK) {
            ump_error("[AM311X_VENC] mh_videoenc_start failed: %d\n", ret);
            Release();
            return -1;
        }
        started_ = true;

        // ---- H.264 获取 SPS/PPS（JPEG 不需要） ----
        if (codec_type_ == video_common::kVideoCodecH264) {
            MH_VIDEOENC_HEADER header = {};
            ret = mh_videoenc_get(path_, MH_VIDEOENC_PARAM_OUT_HEADER, &header);
            if (ret != MH_OK || !header.data || !header.size) {
                ump_error("[AM311X_VENC] get SPS/PPS failed: ret=%d\n", ret);
                Release();
                return -1;
            }
            header_data_.assign((uint8_t *)header.data, (uint8_t *)header.data + header.size);
            // 打印 SPS/PPS 前 16 字节，确认是否带 Annex-B start code
            ump_info("[AM311X_VENC] SPS/PPS size=%zu, first 16 bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                     header_data_.size(),
                     header_data_[0], header_data_[1], header_data_[2], header_data_[3],
                     header_data_[4], header_data_[5], header_data_[6], header_data_[7],
                     header_data_[8], header_data_[9], header_data_[10], header_data_[11],
                     header_data_[12], header_data_[13], header_data_[14], header_data_[15]);
        }

        // ---- 分配 output buffer ----
        uint64_t ob_size = (uint64_t)sizeimage_ * 4U;
        if (ob_size < OUTPUT_MIN) ob_size = OUTPUT_MIN;
        if (ob_size > OUTPUT_MAX) ob_size = OUTPUT_MAX;
        output_buffer_size_ = (uint32_t)ob_size;
        output_buffer_ = (unsigned char *)malloc(output_buffer_size_);
        if (!output_buffer_) {
            ump_error("[AM311X_VENC] malloc output_buffer failed\n");
            Release();
            return -1;
        }

        // ---- dma_alloc（MHAL phy_addr 路径输入缓冲）----
        ret = mh_videoenc_dma_alloc(sizeimage_, &input_dma_);
        if (ret != MH_OK) {
            ump_error("[AM311X_VENC] mh_videoenc_dma_alloc(%u) failed: %d\n",
                      sizeimage_, ret);
            Release();
            return -1;
        }
        input_dma_ready_ = true;

        ump_info("[AM311X_VENC] InitEncode OK: %ux%u %s %ufps %ubps gop=%u "
                 "stride=%u sizeimage=%u SPSPPS=%zuB\n",
                 width_, height_,
                 fourcc_ == V4L2_PIX_FMT_NV12 ? "NV12" :
                 fourcc_ == V4L2_PIX_FMT_YUYV ? "YUYV" : "I420",
                 fps_, bitrate_, gop_,
                 bytesperline_, sizeimage_, header_data_.size());
        return 0;
    }

    // ============================================================================
    // Release —— 释放编码器资源
    // ============================================================================
    int32_t Am311xEncoderImpl::Release()
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // ---- stop ----
        if (started_) {
            auto ret = mh_videoenc_stop(path_);
            if (ret != MH_OK)
                ump_warn("[AM311X_VENC] mh_videoenc_stop failed: %d\n", ret);
            started_ = false;
        }

        // ---- close ----
        if (opened_) {
            auto ret = mh_videoenc_close(path_);
            if (ret != MH_OK)
                ump_warn("[AM311X_VENC] mh_videoenc_close failed: %d\n", ret);
            opened_ = false;
        }

        // ---- free dma buffer（必须在 uninit 之前，否则 DMA 池已销毁导致泄漏） ----
        if (input_dma_ready_) {
            auto ret = mh_videoenc_dma_free(&input_dma_);
            if (ret != MH_OK)
                ump_warn("[AM311X_VENC] mh_videoenc_dma_free failed: %d\n", ret);
            else
                ump_info("[AM311X_VENC] DMA buffer freed (size=%u)\n", input_dma_.size);
            input_dma_ready_ = false;
            memset(&input_dma_, 0, sizeof(input_dma_));
        }

        // ---- uninit ----
        if (initialized_) {
            auto ret = mh_videoenc_uninit();
            if (ret != MH_OK)
                ump_warn("[AM311X_VENC] mh_videoenc_uninit failed: %d\n", ret);
            initialized_ = false;
        }

        // ---- free output buffer ----
        if (output_buffer_) {
            free(output_buffer_);
            output_buffer_ = nullptr;
        }
        output_buffer_size_ = 0;
        header_data_.clear();

        ump_info("[AM311X_VENC] Release OK (encoded=%u frames, %llu bytes)\n",
                 encoded_frames_, (unsigned long long)encoded_bytes_);
        return 0;
    }

    // ============================================================================
    // Encode —— 编码一帧
    // ============================================================================
    int32_t Am311xEncoderImpl::Encode(const video_common::VideoFrameBuffer *frame_buffer)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!started_ || !input_dma_ready_) {
            ump_warn("[AM311X_VENC] Encode: encoder not ready\n");
            return -2;  // WEBRTC_VIDEO_CODEC_UNINITIALIZED
        }
        if (!frame_buffer) {
            ump_error("[AM311X_VENC] Encode: null frame_buffer\n");
            return -4;  // WEBRTC_VIDEO_CODEC_ERR_PARAMETER
        }

        // ---- 1. memcpy 到 dma buffer ----
        if (CopyFrameToDma(frame_buffer, input_dma_.virt_addr) != 0)
            return -1;

        // ---- 2. 构造 MH_VIDEO_FRAME（phy_addr 路径）----
        MH_VIDEO_FRAME frame = {};
        // MH_VIDEOENC_PXLFMT 和 MH_VIDEO_PXLFMT 枚举值不同，需要转换
        switch (encoder_format_) {
        case MH_VIDEOENC_PXLFMT_YUV420_NV12: frame.pxlfmt = MH_VIDEO_PXLFMT_YUV420_NV12; break;
        case MH_VIDEOENC_PXLFMT_YUV420_I420: frame.pxlfmt = MH_VIDEO_PXLFMT_YUV420_I420; break;
        case MH_VIDEOENC_PXLFMT_YUV422_YUYV: frame.pxlfmt = MH_VIDEO_PXLFMT_YUV422_YUYV; break;
        default: frame.pxlfmt = MH_VIDEO_PXLFMT_YUV420_NV12; break;
        }
        frame.dim.w = width_;
        frame.dim.h = height_;
        frame.count = ++frame_sequence_;
        // 时间戳：V4L2 绝对毫秒 → 相对微秒（首帧为基准）
        uint64_t ts_ms = (uint64_t)frame_buffer->GetTimestamp();
        if (first_timestamp_ms_ == 0)
            first_timestamp_ms_ = ts_ms;
        frame.timestamp = (ts_ms - first_timestamp_ms_) * 1000;
        frame.loff[0] = bytesperline_;
        frame.phy_addr[0] = input_dma_.phy_addr;
        if (encoder_format_ == MH_VIDEOENC_PXLFMT_YUV420_NV12 ||
            encoder_format_ == MH_VIDEOENC_PXLFMT_YUV420_I420) {
            // NV12/I420: Y plane + UV plane（plane_num=2）
            // NV12 的 UV 在 bytesperline_ * height_ 偏移处
            frame.phy_addr[1] = input_dma_.phy_addr + (uint32_t)bytesperline_ * height_;
            frame.loff[1] = bytesperline_;
            frame.pw[1] = width_;
            frame.ph[1] = height_ / 2;
        } else {
            frame.pw[0] = width_;
            frame.ph[0] = height_;
        }
        frame.pw[0] = width_;
        frame.ph[0] = height_;

        // ---- 3. push 输入帧 + pull 输出码流 ----
        MH_VIDEOENC_BS bitstream = {};
        bitstream.p_user_buf = (CHAR *)output_buffer_;
        bitstream.user_buf_size = output_buffer_size_;

        // 3a. push_in_buf：把输入帧送入编码器 in_q
        auto ret = mh_videoenc_push_in_buf(path_, &frame, &bitstream, timeout_ms_);
        if (ret != MH_OK) {
            ump_error("[AM311X_VENC] push_in_buf frame %llu failed: %d\n",
                      (unsigned long long)frame.count, ret);
            return ret == MH_TIMEOUT ? -6 /*TIMEOUT*/ : -1;
        }

        // 3b. pull_out_buf：从 out_q 取编码后的码流（必须，否则 out_q 满 push 会阻塞）
        ret = mh_videoenc_pull_out_buf(path_, &bitstream, timeout_ms_);
        if (ret != MH_OK) {
            ump_error("[AM311X_VENC] pull_out_buf frame %llu failed: %d\n",
                      (unsigned long long)frame.count, ret);
            return ret == MH_TIMEOUT ? -6 /*TIMEOUT*/ : -1;
        }

        // ---- 4. 检查输出 ----
        uint32_t encoded_size = 0;
        if (bitstream.frame_type == MH_FRAME_TYPE_NONE &&
            bitstream.pack_num == 1 &&
            bitstream.video_pack[0].size == 0) {
            // 帧被码控丢弃
            auto rr = mh_videoenc_release_out_buf(path_, &bitstream);
            if (rr != MH_OK)
                ump_warn("[AM311X_VENC] release dropped frame failed: %d\n", rr);
            ump_debug("[AM311X_VENC] frame %llu not coded (rate control drop)\n",
                      (unsigned long long)frame.count);
            return 0;  // 不算错误
        }
        if (bitstream.pack_num != 1 ||
            !bitstream.video_pack[0].size ||
            bitstream.video_pack[0].size > output_buffer_size_) {
            ump_error("[AM311X_VENC] invalid output: pack_num=%u size=%u cap=%u\n",
                      bitstream.pack_num, bitstream.video_pack[0].size,
                      output_buffer_size_);
            // 尝试 release
            mh_videoenc_release_out_buf(path_, &bitstream);
            return -1;
        }
        encoded_size = bitstream.video_pack[0].size;

        // ---- 帧类型调试日志（仅打印 IDR 帧） ----
        {
            static uint64_t s_frame_cnt = 0;
            static uint64_t s_last_idr_idx = 0;
            s_frame_cnt++;
            bool is_idr = (bitstream.frame_type == MH_FRAME_TYPE_IDR);
            if (is_idr) {
                uint64_t gop = (s_last_idr_idx > 0) ? (s_frame_cnt - s_last_idr_idx) : 0;
                ump_info("[AM311X_VENC] frame #%llu: IDR size=%u gop=%llu\n",
                         (unsigned long long)s_frame_cnt, encoded_size,
                         (unsigned long long)gop);
                s_last_idr_idx = s_frame_cnt;
            }
        }

        // ---- 5. 回调 ----
        if (encoded_image_callback_ && encoded_size > 0) {
            // 创建编码帧 buffer（拷贝到新分配的 buffer，生命周期独立于 output_buffer_）
            auto *enc_buf = new video_common::EncodedImageBuffer(
                output_buffer_, encoded_size);

            video_common::CodecSpecificInfo codec_info;
            codec_info.codecType = codec_type_;

            // sps_pps 必须声明在外层，确保 OnFrame 调用期间有效（不能在内层 if 块内声明，
            // 否则 if 块结束时 vector 析构，sps_pps.data() 变成悬空指针）
            std::vector<uint8_t> sps_pps;
            if (codec_type_ == video_common::kVideoCodecH264) {
                bool is_idr = (bitstream.frame_type == MH_FRAME_TYPE_IDR);
                if (is_idr && !header_data_.empty()) {
                    codec_info.codecSpecific.H264.idr_frame = true;
                    sps_pps = header_data_;   // 复制一份
                    codec_info.codecSpecific.H264.sps_pps = sps_pps.data();
                    codec_info.codecSpecific.H264.sps_pps_len = sps_pps.size();
                }
            }
            // JPEG：codec_info 不需要额外字段，直接回调

            enc_buf->AddRef();
            encoded_image_callback_->OnFrame(enc_buf, &codec_info);
            enc_buf->RelRef();
        }

        // ---- 6. release output buffer ----
        auto rr = mh_videoenc_release_out_buf(path_, &bitstream);
        if (rr != MH_OK)
            ump_warn("[AM311X_VENC] release_out_buf failed: %d\n", rr);

        encoded_frames_++;
        encoded_bytes_ += encoded_size;
        return 0;
    }

    // ============================================================================
    // RequestKeyFrame —— 请求 IDR 帧
    // ============================================================================
    int32_t Am311xEncoderImpl::RequestKeyFrame()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) {
            ump_warn("[AM311X_VENC] RequestKeyFrame: encoder not started\n");
            return -2;
        }

        MH_H26XENC_REQUEST_IFRAME req = {};
        req.enable = TRUE;
        auto ret = mh_videoenc_set(path_, MH_VIDEOENC_PARAM_OUT_REQUEST_IFRAME, &req);
        if (ret != MH_OK) {
            ump_error("[AM311X_VENC] request IDR failed: %d\n", ret);
            return -1;
        }
        ump_debug("[AM311X_VENC] IDR requested\n");
        return 0;
    }

    // ============================================================================
    // SetRates —— MHAL 的 OUT_RATE_CONTROL 可以运行时 get+set 更新 bitrate
    // ============================================================================
    void Am311xEncoderImpl::SetRates(const video_common::RateControlParameters &parameters)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_)
            return;

        uint32_t new_br = parameters.target_bitrate;
        if (new_br == 0)
            return;

        // 更新 RATE_CONTROL，只改 bitrate
        MH_H26XENC_RATE_CONTROL rc = {};
        rc.rc_mode = MH_RC_MODE_VBR;
        // 注意：MHAL 内部无论 CBR/VBR 都从 cbr 字段读取参数（union 共享内存）
        rc.cbr.bitrate = new_br;
        rc.cbr.frame_rate_base = fps_;
        rc.cbr.frame_rate_incr = 1;
        rc.cbr.init_i_qp = init_qp_;
        rc.cbr.max_i_qp = max_qp_;
        rc.cbr.min_i_qp = min_qp_;
        rc.cbr.init_p_qp = init_qp_;
        rc.cbr.max_p_qp = max_qp_;
        rc.cbr.min_p_qp = min_qp_;

        auto ret = mh_videoenc_set(path_, MH_VIDEOENC_PARAM_OUT_RATE_CONTROL, &rc);
        if (ret != MH_OK) {
            ump_warn("[AM311X_VENC] SetRates failed: %d\n", ret);
            return;
        }
        bitrate_ = new_br;
        ump_info("[AM311X_VENC] bitrate updated to %ubps\n", new_br);
    }

} // namespace ecovacs
