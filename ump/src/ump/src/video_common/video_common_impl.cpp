#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "osp.h"
#include <algorithm>
#include "video_common_defines.h"

namespace ecovacs
{
    namespace video_common
    {
        // ==================== 基础工具函数 ====================
        int I420DataSize(int height, int stride_y, int stride_u, int stride_v)
        {
            return stride_y * height + (stride_u + stride_v) * ((height + 1) / 2);
        }

        int NV12DataSize(int height, int stride_y, int stride_uv)
        {
            return stride_y * height + stride_uv * ((height + 1) / 2);
        }

        // ==================== VideoFrameBuffer ====================
        VideoFrameBuffer::VideoFrameBuffer(FrameType frame_type, int width, int height)
            : frame_type_(frame_type), width_(width), height_(height)
        {
            switch (frame_type)
            {
            case kI420:
                VideoFrameBuffer(width, height, width, (width_ + 1) / 2, (width_ + 1) / 2);
                break;
            case kNV12:
            case kNV21:
                VideoFrameBuffer(width, height, width, width_ + width_ % 2);
                break;
            default:
                break;
            }
        }

        VideoFrameBuffer::VideoFrameBuffer(int width, int height, int stride_y, int stride_u, int stride_v)
            : frame_type_(kI420), width_(width), height_(height)
        {
            stride_y_ = stride_y > width_ ? stride_y : width_;
            stride_u_ = stride_u > (width_ + 1) / 2 ? stride_u : (width_ + 1) / 2;
            stride_v_ = stride_v > (width_ + 1) / 2 ? stride_v : (width_ + 1) / 2;
            data_y_ = (uint8_t *)IntelliUtilMalloc(sizeof(uint8_t) * I420DataSize(height_, stride_y_, stride_u_, stride_v_));
            data_u_ = data_y_ + stride_y_ * height_;
            data_v_ = data_y_ + stride_y_ * height_ + stride_u_ * ((height_ + 1) / 2);
        }

        VideoFrameBuffer::VideoFrameBuffer(int width, int height, int stride_y, int stride_uv)
            : frame_type_(kNV12), width_(width), height_(height)
        {
            stride_y_ = stride_y > width_ ? stride_y : width_;
            stride_uv_ = stride_uv > width_ + width_ % 2 ? stride_uv : width_ + width_ % 2;
            data_y_ = (uint8_t *)IntelliUtilMalloc(sizeof(uint8_t) * NV12DataSize(height_, stride_y_, stride_uv_));
            data_uv_ = data_y_ + stride_y_ * height_;
        }

        VideoFrameBuffer::~VideoFrameBuffer()
        {
            if (data_y_)
                IntelliUtilFree(data_y_);
            data_y_ = nullptr;
            switch (frame_type_)
            {
            case kI420:
                data_u_ = nullptr;
                data_v_ = nullptr;
                break;
            case kNV12:
            case kNV21:
                data_uv_ = nullptr;
                break;
            default:
                break;
            }
        }

        FrameType VideoFrameBuffer::type() const { return frame_type_; }
        int VideoFrameBuffer::width() const { return width_; }
        int VideoFrameBuffer::height() const { return height_; }

        const uint8_t *VideoFrameBuffer::DataY() const { return data_y_; }
        const uint8_t *VideoFrameBuffer::DataU() const { return data_y_ + stride_y_ * height_; }
        const uint8_t *VideoFrameBuffer::DataV() const { return data_y_ + stride_y_ * height_ + stride_u_ * ((height_ + 1) / 2); }
        const uint8_t *VideoFrameBuffer::DataUV() const { return data_y_ + stride_y_ * height_; }

        uint8_t *VideoFrameBuffer::MutableDataY() { return const_cast<uint8_t *>(DataY()); }
        uint8_t *VideoFrameBuffer::MutableDataU() { return const_cast<uint8_t *>(DataU()); }
        uint8_t *VideoFrameBuffer::MutableDataV() { return const_cast<uint8_t *>(DataV()); }
        uint8_t *VideoFrameBuffer::MutableDataUV() { return const_cast<uint8_t *>(DataUV()); }

        int VideoFrameBuffer::StrideY() const { return stride_y_; }
        int VideoFrameBuffer::StrideU() const { return stride_u_; }
        int VideoFrameBuffer::StrideV() const { return stride_v_; }
        int VideoFrameBuffer::StrideUV() const { return stride_uv_; }

        // ==================== CodecSpecificInfo ====================
        CodecSpecificInfo::CodecSpecificInfo() : codecType(kVideoCodecGeneric)
        {
            memset(&codecSpecific, 0, sizeof(codecSpecific));
        }
        CodecSpecificInfo::CodecSpecificInfo(const CodecSpecificInfo &) = default;
        CodecSpecificInfo::~CodecSpecificInfo() = default;

        // ==================== EncodedImageBuffer ====================
        EncodedImageBuffer::EncodedImageBuffer(size_t size) : size_(size)
        {
            buffer_ = static_cast<uint8_t *>(malloc(size));
        }

        EncodedImageBuffer::EncodedImageBuffer(const uint8_t *data, size_t size) : EncodedImageBuffer(size)
        {
            memcpy(buffer_, data, size);
        }

        EncodedImageBuffer::~EncodedImageBuffer()
        {
            if (buffer_)
                free(buffer_);
            buffer_ = nullptr;
        }

        const uint8_t *EncodedImageBuffer::data() const { return buffer_; }
        uint8_t *EncodedImageBuffer::data() { return buffer_; }
        size_t EncodedImageBuffer::size() const { return size_; }

        void EncodedImageBuffer::Realloc(size_t size)
        {
            buffer_ = static_cast<uint8_t *>(realloc(buffer_, size));
            size_ = size;
        }

        // ==================== VideoBroadcaster 智能指针实现 ====================
        VideoBroadcaster::VideoBroadcaster() = default;
        VideoBroadcaster::~VideoBroadcaster() = default;

        void VideoBroadcaster::AddOrUpdateSink(std::shared_ptr<VideoSinkInterface> sink)
        {
            if (!sink)
                return;

            std::lock_guard<std::mutex> lock(sinks_lock_);
            auto it = std::find_if(sinks_.begin(), sinks_.end(),
                                   [&sink](const SinkEntry &entry)
                                   {
                                       auto sp = entry.sink.lock();
                                       return sp && sp == sink;
                                   });
            if (it != sinks_.end())
                return;
            sinks_.push_back({sink});
        }

        void VideoBroadcaster::RemoveSink(std::shared_ptr<VideoSinkInterface> sink)
        {
            if (!sink)
                return;

            std::lock_guard<std::mutex> lock(sinks_lock_);
            sinks_.erase(std::remove_if(sinks_.begin(), sinks_.end(),
                                        [&sink](const SinkEntry &entry)
                                        {
                                            auto sp = entry.sink.lock();
                                            return sp && sp == sink;
                                        }),
                         sinks_.end());
        }

        bool VideoBroadcaster::frame_wanted() const
        {
            std::lock_guard<std::mutex> lock(sinks_lock_);
            for (const auto &entry : sinks_)
            {
                if (!entry.sink.expired())
                    return true;
            }
            return false;
        }

        void VideoBroadcaster::OnFrame(VideoFrameBuffer *frame)
        {
            if (!frame)
                return;

            std::vector<std::shared_ptr<VideoSinkInterface>> valid;
            {
                std::lock_guard<std::mutex> lock(sinks_lock_);
                valid.reserve(sinks_.size());
                for (auto &entry : sinks_)
                {
                    if (auto sp = entry.sink.lock())
                    {
                        valid.push_back(sp);
                    }
                }
            }
            for (auto &sink : valid)
            {
                sink->OnFrame(frame);
            }
        }

        void VideoBroadcaster::OnDiscardedFrame()
        {
            std::vector<std::shared_ptr<VideoSinkInterface>> valid;
            {
                std::lock_guard<std::mutex> lock(sinks_lock_);
                valid.reserve(sinks_.size());
                for (auto &entry : sinks_)
                {
                    if (auto sp = entry.sink.lock())
                    {
                        valid.push_back(sp);
                    }
                }
            }
            for (auto &sink : valid)
            {
                sink->OnDiscardedFrame();
            }
        }

        // ==================== EncodedImageBroadcaster 智能指针实现 ====================
        EncodedImageBroadcaster::EncodedImageBroadcaster() = default;
        EncodedImageBroadcaster::~EncodedImageBroadcaster() = default;

        void EncodedImageBroadcaster::AddOrUpdateSink(std::shared_ptr<EncodedImageSinkInterface> sink)
        {
            if (!sink)
                return;

            std::lock_guard<std::mutex> lock(sinks_lock_);
            auto it = std::find_if(sinks_.begin(), sinks_.end(),
                                   [&sink](const SinkEntry &entry)
                                   {
                                       auto sp = entry.sink.lock();
                                       return sp && sp == sink;
                                   });
            if (it != sinks_.end())
                return;
            sinks_.push_back({sink});
        }

        void EncodedImageBroadcaster::RemoveSink(std::shared_ptr<EncodedImageSinkInterface> sink)
        {
            if (!sink)
                return;

            std::lock_guard<std::mutex> lock(sinks_lock_);
            sinks_.erase(std::remove_if(sinks_.begin(), sinks_.end(),
                                        [&sink](const SinkEntry &entry)
                                        {
                                            auto sp = entry.sink.lock();
                                            return sp && sp == sink;
                                        }),
                         sinks_.end());
        }

        bool EncodedImageBroadcaster::frame_wanted() const
        {
            std::lock_guard<std::mutex> lock(sinks_lock_);
            for (const auto &entry : sinks_)
            {
                if (!entry.sink.expired())
                    return true;
            }
            return false;
        }

        void EncodedImageBroadcaster::OnFrame(EncodedImageBuffer *encoded_image, const CodecSpecificInfo *codec_specific_info)
        {
            if (!encoded_image)
                return;

            std::vector<std::shared_ptr<EncodedImageSinkInterface>> valid;
            {
                std::lock_guard<std::mutex> lock(sinks_lock_);
                valid.reserve(sinks_.size());
                for (auto &entry : sinks_)
                {
                    if (auto sp = entry.sink.lock())
                    {
                        valid.push_back(sp);
                    }
                }
            }
            for (auto &sink : valid)
            {
                sink->OnFrame(encoded_image, codec_specific_info);
            }
        }

        void EncodedImageBroadcaster::OnDiscardedFrame()
        {
            std::vector<std::shared_ptr<EncodedImageSinkInterface>> valid;
            {
                std::lock_guard<std::mutex> lock(sinks_lock_);
                valid.reserve(sinks_.size());
                for (auto &entry : sinks_)
                {
                    if (auto sp = entry.sink.lock())
                    {
                        valid.push_back(sp);
                    }
                }
            }
            for (auto &sink : valid)
            {
                sink->OnDiscardedFrame();
            }
        }

        // ==================== FramerateController ====================
        namespace
        {
            constexpr double kMinFramerate = 0.5;
            constexpr int64_t kNumNanosecsPerSec = 1000000000;
            constexpr int64_t kNumMicrosecsPerSec = 1000000;
        }

        FramerateController::FramerateController() : FramerateController(std::numeric_limits<double>::max()) {}
        FramerateController::FramerateController(double max_framerate) : max_framerate_(max_framerate) {}
        FramerateController::~FramerateController() {}

        void FramerateController::SetMaxFramerate(double max_framerate) { max_framerate_ = max_framerate; }
        double FramerateController::GetMaxFramerate() const { return max_framerate_; }

        bool FramerateController::ShouldDropFrame(int64_t in_timestamp_ns)
        {
            if (max_framerate_ < kMinFramerate)
                return true;
            int64_t frame_interval_ns = kNumNanosecsPerSec / max_framerate_;
            if (frame_interval_ns <= 0)
                return false;
            if (next_frame_timestamp_ns_)
            {
                const int64_t time_until_next_frame_ns = (*next_frame_timestamp_ns_ - in_timestamp_ns);
                if (std::abs(time_until_next_frame_ns) < 2 * frame_interval_ns)
                {
                    if (time_until_next_frame_ns > 0)
                        return true;
                    *next_frame_timestamp_ns_ += frame_interval_ns;
                    return false;
                }
            }
            next_frame_timestamp_ns_ = in_timestamp_ns + frame_interval_ns / 2;
            return false;
        }

        void FramerateController::Reset()
        {
            max_framerate_ = std::numeric_limits<double>::max();
            next_frame_timestamp_ns_ = boost::none;
        }

        void FramerateController::KeepFrame(int64_t in_timestamp_ns)
        {
            if (ShouldDropFrame(in_timestamp_ns))
            {
                if (max_framerate_ < kMinFramerate)
                    return;
                int64_t frame_interval_ns = kNumNanosecsPerSec / max_framerate_;
                if (next_frame_timestamp_ns_)
                    *next_frame_timestamp_ns_ += frame_interval_ns;
            }
        }

        // ==================== FrameRateEstimator ====================
        FrameRateEstimator::FrameRateEstimator(int64_t averaging_window_us) : averaging_window_us_(averaging_window_us) {}

        void FrameRateEstimator::OnFrame(int64_t time_stamp_now_us)
        {
            CullOld(time_stamp_now_us);
            frame_times_.push_back(time_stamp_now_us);
        }

        boost::optional<double> FrameRateEstimator::GetAverageFps() const
        {
            if (frame_times_.size() < 2)
                return boost::none;
            int64_t time_span = frame_times_.back() - frame_times_.front();
            if (time_span < 1 * 1000 * 1000)
                return boost::none;
            int64_t avg_frame_interval = time_span / (frame_times_.size() - 1);
            return static_cast<double>(kNumMicrosecsPerSec) / avg_frame_interval;
        }

        boost::optional<double> FrameRateEstimator::GetAverageFps(int64_t time_stamp_now_us)
        {
            CullOld(time_stamp_now_us);
            return GetAverageFps();
        }

        void FrameRateEstimator::Reset() { frame_times_.clear(); }

        void FrameRateEstimator::CullOld(int64_t time_stamp_now_us)
        {
            while (!frame_times_.empty() && frame_times_.front() + averaging_window_us_ < time_stamp_now_us)
                frame_times_.pop_front();
        }

        // ==================== H264 Tools ====================
        namespace H264
        {
            const uint8_t kNaluTypeMask = 0x1F;

            std::vector<NaluIndex> FindNaluIndices(const uint8_t *buffer, size_t buffer_size)
            {
                std::vector<NaluIndex> sequences;
                if (buffer_size < kNaluShortStartSequenceSize)
                    return sequences;

                const size_t end = buffer_size - kNaluShortStartSequenceSize;
                for (size_t i = 0; i < end;)
                {
                    if (buffer[i + 2] > 1)
                    {
                        i += 3;
                    }
                    else if (buffer[i + 2] == 1)
                    {
                        if (buffer[i + 1] == 0 && buffer[i] == 0)
                        {
                            NaluIndex index = {i, i + 3, 0};
                            if (index.start_offset > 0 && buffer[index.start_offset - 1] == 0)
                                --index.start_offset;

                            auto it = sequences.rbegin();
                            if (it != sequences.rend())
                                it->payload_size = index.start_offset - it->payload_start_offset;

                            sequences.push_back(index);
                        }
                        i += 3;
                    }
                    else
                    {
                        ++i;
                    }
                }

                auto it = sequences.rbegin();
                if (it != sequences.rend())
                    it->payload_size = buffer_size - it->payload_start_offset;

                return sequences;
            }

            NaluType ParseNaluType(uint8_t data)
            {
                return static_cast<NaluType>(data & kNaluTypeMask);
            }

            std::vector<uint8_t> ParseRbsp(const uint8_t *data, size_t length)
            {
                std::vector<uint8_t> out;
                out.reserve(length);
                for (size_t i = 0; i < length;)
                {
                    if (length - i >= 3 && !data[i] && !data[i + 1] && data[i + 2] == 3)
                    {
                        out.push_back(data[i++]);
                        out.push_back(data[i++]);
                        i++;
                    }
                    else
                    {
                        out.push_back(data[i++]);
                    }
                }
                return out;
            }
        } // namespace H264
    } // namespace video_common
} // namespace ecovacs