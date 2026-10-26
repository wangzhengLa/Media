#include "api_soft_jpeg_encoder.h"

namespace ecovacs
{
    JpegEncoder::JpegEncoder()
        : compressor_(nullptr), width_(0), height_(0),
          stride_y_(0), stride_uv_(0), i420_y_size_(0), i420_u_size_(0)
    {
    }

    JpegEncoder::~JpegEncoder()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (compressor_)
        {
            tjDestroy(compressor_);
            compressor_ = nullptr;
        }
    }

    bool JpegEncoder::IsEven(int value) const
    {
        return (value % 2) == 0;
    }

    bool JpegEncoder::ValidateParams(int width, int height, int stride_y, int stride_uv) const
    {
        if (width <= 0 || height <= 0)
        {
            ump_error("Invalid dimensions: %dx%d\n", width, height);
            return false;
        }

        if (!IsEven(width) || !IsEven(height))
        {
            ump_error("Width and height must be even: %dx%d\n", width, height);
            return false;
        }

        if (stride_y > 0 && !IsEven(stride_y))
        {
            ump_error("Stride Y must be even: %d\n", stride_y);
            return false;
        }

        if (stride_uv > 0 && !IsEven(stride_uv))
        {
            ump_error("Stride UV must be even: %d\n", stride_uv);
            return false;
        }

        return true;
    }

    bool JpegEncoder::Init(int width, int height, int stride_y, int stride_uv)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!ValidateParams(width, height, stride_y, stride_uv))
        {
            return false;
        }

        width_ = width;
        height_ = height;
        stride_y_ = (stride_y > 0) ? stride_y : width;
        stride_uv_ = (stride_uv > 0) ? stride_uv : width;

        i420_y_size_ = static_cast<size_t>(width_) * height_;
        i420_u_size_ = static_cast<size_t>(width_ / 2) * (height_ / 2);

        // 预分配 I420 缓冲区: Y + U + V
        i420_buffer_.resize(i420_y_size_ + i420_u_size_ * 2);

        // 如果已有 compressor，先销毁
        if (compressor_)
        {
            tjDestroy(compressor_);
            compressor_ = nullptr;
        }

        compressor_ = tjInitCompress();
        if (!compressor_)
        {
            ump_error("tjInitCompress failed: %s\n", tjGetErrorStr());
            return false;
        }

        ump_info("JpegEncoder initialized: %dx%d, stride_y=%d, stride_uv=%d\n",
                 width_, height_, stride_y_, stride_uv_);
        return true;
    }

    std::vector<uint8_t> JpegEncoder::EncodeNV12(const uint8_t *y_plane,
                                                 const uint8_t *uv_plane,
                                                 int quality)
    {
        std::vector<uint8_t> result;

        if (!y_plane || !uv_plane)
        {
            ump_error("Invalid input: y_plane=%p, uv_plane=%p\n", y_plane, uv_plane);
            return result;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (!compressor_)
            {
                ump_error("Encoder not initialized\n");
                return result;
            }

            // 验证质量参数
            if (quality < 1)
            {
                ump_warn("Quality %d too low, using 1\n", quality);
                quality = 1;
            }
            if (quality > 100)
            {
                ump_warn("Quality %d too high, using 100\n", quality);
                quality = 100;
            }

            // I420 内存布局: [Y平面][U平面][V平面]
            uint8_t *i420_y = i420_buffer_.data();
            uint8_t *i420_u = i420_y + i420_y_size_;
            uint8_t *i420_v = i420_u + i420_u_size_;

            // NV12 -> I420 转换
            int ret = libyuv::NV12ToI420(
                y_plane, stride_y_,
                uv_plane, stride_uv_,
                i420_y, width_,
                i420_u, width_ / 2,
                i420_v, width_ / 2,
                width_, height_);

            if (ret != 0)
            {
                ump_error("NV12ToI420 failed: %d\n", ret);
                return result;
            }

            // JPEG 编码
            unsigned char *jpeg_buf = nullptr;
            unsigned long jpeg_size = 0;

            int compress_ret = tjCompressFromYUV(
                compressor_,
                i420_buffer_.data(),
                width_,
                1, // pitch = 1 (因为 I420 是连续布局)
                height_,
                TJSAMP_420,
                &jpeg_buf,
                &jpeg_size,
                quality,
                TJFLAG_FASTDCT);

            if (compress_ret == 0 && jpeg_buf && jpeg_size > 0)
            {
                result.assign(jpeg_buf, jpeg_buf + jpeg_size);
                tjFree(jpeg_buf);
            }
            else
            {
                ump_error("JPEG encode failed: %s\n", tjGetErrorStr());
            }
        }

        return result;
    }

} // namespace ecovacs