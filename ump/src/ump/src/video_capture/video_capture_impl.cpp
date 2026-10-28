#include <stdlib.h>
#include <string.h>
#include "osp.h"
#include "log.h"
#include "libyuv.h"
#include "video_capture_impl.h"
#include "video_capture_config.h"

namespace ecovacs
{
    namespace videocapturemodule
    {
        const char *VideoCaptureImpl::CurrentDeviceName() const
        {
            return _deviceUniqueId;
        }

        // static
        int32_t VideoCaptureImpl::RotationFromDegrees(int degrees, video_common::VideoRotation *rotation)
        {
            switch (degrees)
            {
            case 0:
                *rotation = video_common::kVideoRotation_0;
                return 0;
            case 90:
                *rotation = video_common::kVideoRotation_90;
                return 0;
            case 180:
                *rotation = video_common::kVideoRotation_180;
                return 0;
            case 270:
                *rotation = video_common::kVideoRotation_270;
                return 0;
            default:
                return -1;
                ;
            }
        }

        // static
        int32_t VideoCaptureImpl::RotationInDegrees(video_common::VideoRotation rotation, int *degrees)
        {
            switch (rotation)
            {
            case video_common::kVideoRotation_0:
                *degrees = 0;
                return 0;
            case video_common::kVideoRotation_90:
                *degrees = 90;
                return 0;
            case video_common::kVideoRotation_180:
                *degrees = 180;
                return 0;
            case video_common::kVideoRotation_270:
                *degrees = 270;
                return 0;
            }
            return -1;
        }

        VideoCaptureImpl::VideoCaptureImpl()
            : _deviceUniqueId(NULL),
              _requestedCapability(),
              _lastProcessTimeNanos(IntelliUtilTimeNanos()),
              _lastFrameRateCallbackTimeNanos(IntelliUtilTimeNanos()),
              _dataCallBack(NULL),
              _lastProcessFrameTimeNanos(IntelliUtilTimeNanos()),
              _rotateFrame(video_common::kVideoRotation_0),
              apply_rotation_(false)
        {
            _requestedCapability.width = kDefaultWidth;
            _requestedCapability.height = kDefaultHeight;
            _requestedCapability.maxFPS = 30;
            _requestedCapability.videoType = video_common::VideoType::kI420;
            memset(_incomingFrameTimesNanos, 0, sizeof(_incomingFrameTimesNanos));
        }

        VideoCaptureImpl::~VideoCaptureImpl()
        {
            DeRegisterCaptureDataCallback();
            if (_deviceUniqueId)
                delete[] _deviceUniqueId;
        }

        void VideoCaptureImpl::RegisterCaptureDataCallback(video_common::VideoSinkInterface *dataCallBack)
        {
            std::lock_guard<std::mutex> guard(api_lock_);
            _dataCallBack = dataCallBack;
        }

        void VideoCaptureImpl::DeRegisterCaptureDataCallback()
        {
            std::lock_guard<std::mutex> guard(api_lock_);
            _dataCallBack = NULL;
        }

        int32_t VideoCaptureImpl::DeliverCapturedFrame(video_common::VideoFrameBuffer *captureFrame)
        {
            UpdateFrameCount(); // frame count used for local frame rate callback.
            if (_dataCallBack)
            {
                _dataCallBack->OnFrame(captureFrame);
            }
            return 0;
        }

        size_t CalcBufferSize(video_common::VideoType type, int width, int height)
        {
            if (0 == width || 0 == height)
                return 0;
            size_t buffer_size = 0;
            switch (type)
            {
            case video_common::VideoType::kI420:
            case video_common::VideoType::kIYUV:
            case video_common::VideoType::kYV12:
            case video_common::VideoType::kNV12:
            {
                int half_width = (width + 1) >> 1;
                int half_height = (height + 1) >> 1;
                buffer_size = width * height + half_width * half_height * 2;
                break;
            }
            case video_common::VideoType::kRGB565:
            case video_common::VideoType::kYUY2:
            case video_common::VideoType::kUYVY:
                buffer_size = width * height * 2;
                break;
            case video_common::VideoType::kRGB24:
                buffer_size = width * height * 3;
                break;
            case video_common::VideoType::kBGRA:
            case video_common::VideoType::kARGB:
                buffer_size = width * height * 4;
                break;
            default:
                // RTC_DCHECK_NOTREACHED();
                break;
            }
            return buffer_size;
        }

        int ConvertVideoType(video_common::VideoType video_type)
        {
            switch (video_type)
            {
            case video_common::VideoType::kUnknown:
                return libyuv::FOURCC_ANY;
            case video_common::VideoType::kI420:
                return libyuv::FOURCC_I420;
            case video_common::VideoType::kIYUV: // same as VideoType::kYV12
            case video_common::VideoType::kYV12:
                return libyuv::FOURCC_YV12;
            case video_common::VideoType::kRGB24:
                return libyuv::FOURCC_24BG;
            case video_common::VideoType::kRGB565:
                return libyuv::FOURCC_RGBP;
            case video_common::VideoType::kYUY2:
                return libyuv::FOURCC_YUY2;
            case video_common::VideoType::kUYVY:
                return libyuv::FOURCC_UYVY;
            case video_common::VideoType::kMJPEG:
                return libyuv::FOURCC_MJPG;
            case video_common::VideoType::kARGB:
                return libyuv::FOURCC_ARGB;
            case video_common::VideoType::kBGRA:
                return libyuv::FOURCC_BGRA;
            case video_common::VideoType::kNV12:
                return libyuv::FOURCC_NV12;
            }
            return libyuv::FOURCC_ANY;
        }

        int32_t VideoCaptureImpl::IncomingFrame(video_common::VideoFrameBuffer *buffer)
        {
            std::lock_guard<std::mutex> guard(api_lock_);
            buffer->AddRef();
            DeliverCapturedFrame(buffer);
            buffer->RelRef();
            return 0;
        }

        int32_t VideoCaptureImpl::IncomingFrame(uint8_t *videoFrame,
                                                size_t videoFrameLength,
                                                const VideoCaptureCapability &frameInfo,
                                                int64_t captureTime,
                                                uint32_t sequence)
        {
            std::lock_guard<std::mutex> guard(api_lock_);

            const int32_t width = frameInfo.width;
            const int32_t height = frameInfo.height;

            static int32_t print_num = 0;
            if (0 == print_num++ % 100)
            {
                int32_t frame_rate = CalculateFrameRate(IntelliUtilTimeNanos());
                ump_info("ecovacs VC::IncomingFrame VideoType::%d Width::%d Height::%d FrameRate::%u FrameLength::%d", frameInfo.videoType,
                     width, height, frame_rate, videoFrameLength);
            }

            // Not encoded, convert to I420.
            if (frameInfo.videoType != video_common::VideoType::kMJPEG &&
                CalcBufferSize(frameInfo.videoType, width, abs(height)) != videoFrameLength)
            {
                ump_error("Wrong incoming frame length.");
                return -1;
            }

            int stride_y = width;
            int stride_u = (width + 1) / 2;
            int stride_v = (width + 1) / 2;
            int stride_uv = width;
            int target_width = width;
            int target_height = abs(height);

            // SetApplyRotation doesn't take any lock. Make a local copy here.
            bool apply_rotation = apply_rotation_;
            if (apply_rotation)
            {
                // Rotating resolution when for 90/270 degree rotations.
                if (_rotateFrame == video_common::kVideoRotation_90 ||
                    _rotateFrame == video_common::kVideoRotation_270)
                {
                    target_width = abs(height);
                    target_height = width;
                }
            }

            libyuv::RotationMode rotation_mode = libyuv::kRotate0;
            if (apply_rotation)
            {
                switch (_rotateFrame)
                {
                case video_common::kVideoRotation_0:
                    rotation_mode = libyuv::kRotate0;
                    break;
                case video_common::kVideoRotation_90:
                    rotation_mode = libyuv::kRotate90;
                    break;
                case video_common::kVideoRotation_180:
                    rotation_mode = libyuv::kRotate180;
                    break;
                case video_common::kVideoRotation_270:
                    rotation_mode = libyuv::kRotate270;
                    break;
                }
            }

            // Setting absolute height (in case it was negative).
            // In Windows, the image starts bottom left, instead of top left.
            // Setting a negative source height, inverts the image (within LibYuv).
            int64_t start_time = IntelliUtilTimeMillis();
            if (frameInfo.videoType == video_common::VideoType::kYUY2)
            {
                // YUYV 直通：packed 格式直接拷贝，不做转换
                // 优化：预分配 buffer，避免每帧 malloc/free 1.84MB
                static video_common::VideoFrameBuffer *yuyv_buffer = nullptr;
                if (!yuyv_buffer || yuyv_buffer->width() != target_width ||
                    yuyv_buffer->height() != target_height)
                {
                    if (yuyv_buffer)
                        yuyv_buffer->RelRef();
                    yuyv_buffer = new video_common::VideoFrameBuffer(
                        video_common::FrameType::kYUY2, target_width, target_height);
                    yuyv_buffer->AddRef();  // 静态持有一个引用
                }

                video_common::VideoFrameBuffer *buffer;
                if (yuyv_buffer->HasOneRef())
                {
                    // 编码线程已处理完，可以复用预分配 buffer
                    buffer = yuyv_buffer;
                    buffer->AddRef();
                }
                else
                {
                    // 编码线程还在用，新建临时 buffer
                    buffer = new video_common::VideoFrameBuffer(
                        video_common::FrameType::kYUY2, target_width, target_height);
                    buffer->AddRef();
                }

                int src_stride = width * 2;
                int dst_stride = buffer->StrideY();
                if (src_stride == dst_stride)
                {
                    memcpy(buffer->MutableDataY(), videoFrame, dst_stride * target_height);
                }
                else
                {
                    for (int row = 0; row < target_height; row++)
                        memcpy(buffer->MutableDataY() + row * dst_stride,
                               videoFrame + row * src_stride,
                               target_width * 2);
                }
                buffer->SetTimestamp(captureTime);
                buffer->SetSequence(sequence);
                DeliverCapturedFrame(buffer);
                buffer->RelRef();
            }
            else if (frameInfo.videoType == video_common::VideoType::kRGB24)
            {
                // RGB24 直通：packed 格式直接拷贝，不做转换（AI 通路用）
                static video_common::VideoFrameBuffer *rgb_buffer = nullptr;
                if (!rgb_buffer || rgb_buffer->width() != target_width ||
                    rgb_buffer->height() != target_height)
                {
                    if (rgb_buffer)
                        rgb_buffer->RelRef();
                    rgb_buffer = new video_common::VideoFrameBuffer(
                        video_common::FrameType::kRGB24, target_width, target_height);
                    rgb_buffer->AddRef();
                }

                video_common::VideoFrameBuffer *buffer;
                if (rgb_buffer->HasOneRef())
                {
                    buffer = rgb_buffer;
                    buffer->AddRef();
                }
                else
                {
                    buffer = new video_common::VideoFrameBuffer(
                        video_common::FrameType::kRGB24, target_width, target_height);
                    buffer->AddRef();
                }

                int src_stride = width * 3;
                int dst_stride = buffer->StrideY();
                if (src_stride == dst_stride)
                {
                    memcpy(buffer->MutableDataY(), videoFrame, dst_stride * target_height);
                }
                else
                {
                    for (int row = 0; row < target_height; row++)
                        memcpy(buffer->MutableDataY() + row * dst_stride,
                               videoFrame + row * src_stride,
                               target_width * 3);
                }
                buffer->SetTimestamp(captureTime);
                buffer->SetSequence(sequence);
                DeliverCapturedFrame(buffer);
                buffer->RelRef();
            }
            else if (libyuv::FOURCC_NV12 != ConvertVideoType(frameInfo.videoType))
            {
                video_common::VideoFrameBuffer *buffer = new video_common::VideoFrameBuffer(target_width, target_height, stride_y, stride_u, stride_v);
                buffer->AddRef();
                const int conversionResult = libyuv::ConvertToI420(
                    videoFrame, videoFrameLength, buffer->MutableDataY(),
                    buffer->StrideY(), buffer->MutableDataU(),
                    buffer->StrideU(), buffer->MutableDataV(),
                    buffer->StrideV(), 0, 0, // No Cropping
                    width, height, target_width, target_height, rotation_mode,
                    ConvertVideoType(frameInfo.videoType));
                if (conversionResult < 0)
                {
                    ump_error("Failed to convert capture frame from type %d to I420.\r\n", static_cast<int>(frameInfo.videoType));
                    buffer->RelRef();
                    return -1;
                }
                video_common::VideoFrameBuffer *buffer_nv12 = new video_common::VideoFrameBuffer(target_width, target_height, stride_y, stride_uv);
                buffer_nv12->AddRef();
                libyuv::I420ToNV12(buffer->MutableDataY(),
                                   buffer->StrideY(), buffer->MutableDataU(),
                                   buffer->StrideU(), buffer->MutableDataV(),
                                   buffer->StrideV(),
                                   buffer_nv12->MutableDataY(),
                                   buffer_nv12->StrideY(), buffer_nv12->MutableDataUV(),
                                   buffer_nv12->StrideUV(),
                                   target_width,
                                   target_height);
                buffer->RelRef();
                buffer_nv12->SetTimestamp(captureTime);
                buffer_nv12->SetSequence(sequence);
                DeliverCapturedFrame(buffer_nv12);
                buffer_nv12->RelRef();
            }
            else
            {
                uint8_t *src_y = videoFrame;
                int src_stride_y = target_width;
                uint8_t *src_uv = videoFrame + target_width * target_height;
                int src_stride_uv = target_width;
                video_common::VideoFrameBuffer *buffer_nv12 = new video_common::VideoFrameBuffer(target_width, target_height, stride_y, stride_uv);
                buffer_nv12->AddRef();
                libyuv::NV12Copy(src_y,
                                 src_stride_y,
                                 src_uv,
                                 src_stride_uv,
                                 buffer_nv12->MutableDataY(),
                                 buffer_nv12->StrideY(), buffer_nv12->MutableDataUV(),
                                 buffer_nv12->StrideUV(),
                                 target_width,
                                 target_height);
                buffer_nv12->SetTimestamp(captureTime);
                buffer_nv12->SetSequence(sequence);
                DeliverCapturedFrame(buffer_nv12);
                buffer_nv12->RelRef();
            }
            int64_t end_time = IntelliUtilTimeMillis();

            if (0 == print_num % 100)
                ump_info("Successed to convert capture frame from type %d to nv12 %lu ms.\r\n", static_cast<int>(frameInfo.videoType), end_time - start_time);
            print_num++;

            return 0;
        }

        int32_t VideoCaptureImpl::StartCapture(const VideoCaptureCapability &capability)
        {
            _requestedCapability = capability;
            return -1;
        }

        int32_t VideoCaptureImpl::StopCapture()
        {
            return -1;
        }

        bool VideoCaptureImpl::CaptureStarted()
        {
            return false;
        }

        int32_t VideoCaptureImpl::CaptureSettings(VideoCaptureCapability & /*settings*/)
        {
            return -1;
        }

        int32_t VideoCaptureImpl::SetCaptureRotation(video_common::VideoRotation rotation)
        {
            std::lock_guard<std::mutex> guard(api_lock_);
            _rotateFrame = rotation;
            return 0;
        }

        bool VideoCaptureImpl::SetApplyRotation(bool enable)
        {
            // We can't take any lock here as it'll cause deadlock with IncomingFrame.
            // The effect of this is the last caller wins.
            apply_rotation_ = enable;
            return true;
        }

        bool VideoCaptureImpl::GetApplyRotation()
        {
            return apply_rotation_;
        }

        void VideoCaptureImpl::UpdateFrameCount()
        {
            if (_incomingFrameTimesNanos[0] / kNumNanosecsPerMicrosec == 0)
            {
                // first no shift
            }
            else
            {
                // shift
                for (int i = (kFrameRateCountHistorySize - 2); i >= 0; --i)
                {
                    _incomingFrameTimesNanos[i + 1] = _incomingFrameTimesNanos[i];
                }
            }
            _incomingFrameTimesNanos[0] = IntelliUtilTimeNanos();
        }

        uint32_t VideoCaptureImpl::CalculateFrameRate(int64_t now_ns)
        {
            int32_t num = 0;
            int32_t nrOfFrames = 0;
            for (num = 1; num < (kFrameRateCountHistorySize - 1); ++num)
            {
                if (_incomingFrameTimesNanos[num] <= 0 ||
                    (now_ns - _incomingFrameTimesNanos[num]) / kNumNanosecsPerMillisec > kFrameRateHistoryWindowMs)
                {
                    // don't use data older than 2sec
                    break;
                }
                else
                {
                    nrOfFrames++;
                }
            }
            if (num > 1)
            {
                int64_t diff = (now_ns - _incomingFrameTimesNanos[num - 1]) / kNumNanosecsPerMillisec;
                if (diff > 0)
                {
                    return uint32_t((nrOfFrames * 1000.0f / diff) + 0.5f);
                }
            }

            return nrOfFrames;
        }

    } // namespace videocapturemodule

} // namespace ecovacs
