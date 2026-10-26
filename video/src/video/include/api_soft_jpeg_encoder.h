#ifndef API_JPEG_ENCODER_H_
#define API_JPEG_ENCODER_H_

#include <vector>
#include <cstdint>
#include <mutex>
#include <turbojpeg.h>
#include <libyuv.h>
#include "log.h"

namespace ecovacs
{
    class JpegEncoder
    {
    public:
        JpegEncoder();
        ~JpegEncoder();

        JpegEncoder(const JpegEncoder &) = delete;
        JpegEncoder &operator=(const JpegEncoder &) = delete;

        /**
         * @brief 初始化编码器
         * @param width 图像宽度（必须为偶数）
         * @param height 图像高度（必须为偶数）
         * @param stride_y Y 平面步长（0 表示使用 width）
         * @param stride_uv UV 平面步长（0 表示使用 width）
         * @return true 成功，false 失败
         */
        bool Init(int width, int height, int stride_y = 0, int stride_uv = 0);

        /**
         * @brief 编码 NV12 格式图像为 JPEG
         * @param y_plane Y 平面数据
         * @param uv_plane UV 平面数据
         * @param quality JPEG 质量 (1-100, 默认 85)
         * @return JPEG 数据，空 vector 表示失败
         */
        std::vector<uint8_t> EncodeNV12(const uint8_t *y_plane,
                                        const uint8_t *uv_plane,
                                        int quality = 85);

        /**
         * @brief 检查编码器是否已初始化
         */
        bool IsInitialized() const { return compressor_ != nullptr; }

        /**
         * @brief 获取当前配置的宽度
         */
        int GetWidth() const { return width_; }

        /**
         * @brief 获取当前配置的高度
         */
        int GetHeight() const { return height_; }

        /**
         * @brief 获取当前配置的 Y 平面步长
         */
        int GetStrideY() const { return stride_y_; }

        /**
         * @brief 获取当前配置的 UV 平面步长
         */
        int GetStrideUV() const { return stride_uv_; }

    private:
        bool IsEven(int value) const;
        bool ValidateParams(int width, int height, int stride_y, int stride_uv) const;

        tjhandle compressor_;
        int width_;
        int height_;
        int stride_y_;
        int stride_uv_;

        std::vector<uint8_t> i420_buffer_;
        size_t i420_y_size_;
        size_t i420_u_size_;

        mutable std::mutex mutex_; // 保护所有成员变量
    };
}

#endif // API_JPEG_ENCODER_H_