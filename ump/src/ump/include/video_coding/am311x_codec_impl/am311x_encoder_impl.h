/**
 * @file    am311x_encoder_impl.h
 * @brief   AM311x H.264 编码器
 */

#ifndef AM311X_ENCODER_IMPL_H_
#define AM311X_ENCODER_IMPL_H_

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "video_encoder_impl.h"
#include "mh_videoenc.h"

namespace ecovacs
{
    /**
     * @brief AM311x 硬件 H.264 编码器
     *
     * 对应 camera_v2_venc_private 的成员完整映射：
     *   path_           ← MH_PATH_ID (MH_VIDEOENC_0_IN_0 / MH_VIDEOENC_0_OUT_0)
     *   width_/height_  ← 编码器输入分辨率
     *   fourcc_         ← V4L2_PIX_FMT_NV12 或 YUYV（决定 encoder_format_）
     *   bytesperline_   ← stride（16 对齐后的值）
     *   sizeimage_      ← 单帧 dma buffer 大小
     *   encoder_format_ ← MH_VIDEOENC_PXLFMT_YUV420_NV12 / YUV422_YUYV / YUV420_I420
     *   fps_/bitrate_/gop_ ← 编码参数
     *   timeout_ms_     ← push_in_buf / pull_out_buf 超时（默认 2000ms）
     *   output_buffer_  ← MHAL bitstream 输出缓冲（malloc）
     *   header_data_    ← SPS/PPS（MH_VIDEOENC_PARAM_OUT_HEADER 获取）
     */
    class Am311xEncoderImpl : public VideoEncoderImpl
    {
    public:
        Am311xEncoderImpl();
        ~Am311xEncoderImpl() override;

        // === VideoEncoderImpl 虚函数 ===
        int32_t InitEncode(const video_common::VideoCodecConfig *codec_settings) override;
        int32_t Release() override;
        int32_t Encode(const video_common::VideoFrameBuffer *frame_buffer) override;
        int32_t RequestKeyFrame() override;
        void SetRates(const video_common::RateControlParameters &parameters) override;

        bool IsInitialized() const { return initialized_; }

    private:
        // ===== 内部方法（照搬 camera_v2_venc 静态函数） =====
        // 照搬 camera_v2_venc_set_parameters —— 设置 PATH_CONFIG / IN / OUT_ENC_PARAM / OUT_RATE_CONTROL
        int SetEncoderParameters();

        // 把 VideoFrameBuffer 的数据按 stride 拷到 dma buffer 的 virt_addr
        int CopyFrameToDma(const video_common::VideoFrameBuffer *src, void *dst_virt);

        // ===== MHAL 状态（对应 camera_v2_venc_private） =====
        MH_PATH_ID path_ = 0;               // 编码路径句柄
        bool initialized_ = false;           // mh_videoenc_init 成功
        bool opened_ = false;                // mh_videoenc_open 成功
        bool started_ = false;               // mh_videoenc_start 成功

        // 编码参数
        uint32_t width_ = 0;
        uint32_t height_ = 0;
        uint32_t fourcc_ = 0;                // V4L2_PIX_FMT_NV12 / YUYV
        uint32_t bytesperline_ = 0;          // stride
        uint32_t sizeimage_ = 0;             // 单帧 buffer 大小
        MH_VIDEOENC_PXLFMT encoder_format_;  // MHAL 像素格式
        uint32_t fps_ = 30;
        uint32_t bitrate_ = 0;               // bps
        uint32_t gop_ = 30;
        uint32_t init_qp_ = 26;              // 初始 QP
        uint32_t min_qp_ = 20;               // 最小 QP
        uint32_t max_qp_ = 45;               // 最大 QP
        int timeout_ms_ = 2000;
        video_common::VideoCodecType codec_type_ = video_common::kVideoCodecH264;  // H264 或 JPEG
        uint32_t jpeg_quality_ = 85;         // JPEG 质量 1-100

        // MHAL 输出缓冲（照搬 camera_v2_venc 的 output_buffer，malloc）
        unsigned char *output_buffer_ = nullptr;
        uint32_t output_buffer_size_ = 0;

        // SPS/PPS 头（InitEncode 时从 MH_VIDEOENC_PARAM_OUT_HEADER 获取）
        std::vector<uint8_t> header_data_;

        // ===== 预分配的 dma buffer（InitEncode 时 dma_alloc） =====
        // Encode 时用这个 buffer 作为 push_in_buf 的 phy_addr 输入
        MH_VIDEOENC_DMA_BUF input_dma_ = {};
        bool input_dma_ready_ = false;

        // ===== 统计 =====
        uint32_t encoded_frames_ = 0;
        uint64_t encoded_bytes_ = 0;
        uint64_t frame_sequence_ = 0;        // 单调递增帧号，传给 MH_VIDEO_FRAME.count
        uint64_t first_timestamp_ms_ = 0;    // 首帧时间戳(ms)，用于计算相对时间戳

        mutable std::mutex mutex_;
    };

} // namespace ecovacs

#endif // AM311X_ENCODER_IMPL_H_
