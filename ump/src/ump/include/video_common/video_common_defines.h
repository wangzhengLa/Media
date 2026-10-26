#ifndef VIDEO_COMMON_DEFINES_H_
#define VIDEO_COMMON_DEFINES_H_

#include <stdint.h>
#include <string>
#include <vector>
#include <mutex>
#include <limits>
#include <atomic>
#include <memory>
#include <boost/optional.hpp>
#include <deque>

#define CRICKET_FOURCC(a, b, c, d)                                  \
    ((static_cast<uint32_t>(a)) | (static_cast<uint32_t>(b) << 8) | \
     (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24))

namespace ecovacs
{
    namespace video_common
    {
        // ==================== RefCounter ====================
        enum class RefCountReleaseStatus
        {
            kDroppedLastRef,
            kOtherRefsRemained
        };

        class RefCounter
        {
        public:
            explicit RefCounter(int ref_count) : ref_count_(ref_count) {}
            RefCounter() = delete;

            void IncRef() { ref_count_.fetch_add(1, std::memory_order_relaxed); }

            RefCountReleaseStatus DecRef()
            {
                int ref_count_after_subtract = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
                return ref_count_after_subtract == 0 ? RefCountReleaseStatus::kDroppedLastRef
                                                     : RefCountReleaseStatus::kOtherRefsRemained;
            }

            bool HasOneRef() const { return ref_count_.load(std::memory_order_acquire) == 1; }
            int GetRefCount() { return ref_count_.load(std::memory_order_acquire); }

        private:
            std::atomic<int> ref_count_;
        };

        // ==================== Enums ====================
        enum VideoRotation
        {
            kVideoRotation_0 = 0,
            kVideoRotation_90 = 90,
            kVideoRotation_180 = 180,
            kVideoRotation_270 = 270
        };

        enum class VideoType
        {
            kUnknown,
            kI420,
            kIYUV,
            kRGB24,
            kARGB,
            kRGB565,
            kYUY2,
            kYV12,
            kUYVY,
            kMJPEG,
            kBGRA,
            kNV12,
        };

        enum FrameType
        {
            kNative,
            kI420,
            kNV12,
            kNV21,
            kI444,
            kBUTT,
        };

        struct FrameTimingInfo
        {
            int64_t capture_time_ms;
            int64_t encode_start_ms;
            int64_t encode_finish_ms;
        };

        enum ImageSourceType
        {
            IMG_SOURCE_TYPE_NOGDC = 0,
            IMG_SOURCE_TYPE_GDC
        };

        // ==================== VideoFrameBuffer ====================
        class VideoFrameBuffer
        {
        public:
            VideoFrameBuffer(FrameType frame_type, int width, int height);
            VideoFrameBuffer(int width, int height, int stride_y, int stride_u, int stride_v);
            VideoFrameBuffer(int width, int height, int stride_y, int stride_uv);
            VideoFrameBuffer(const VideoFrameBuffer &) = delete;
            VideoFrameBuffer &operator=(const VideoFrameBuffer &) = delete;
            ~VideoFrameBuffer();

            FrameType type() const;
            int width() const;
            int height() const;

            const uint8_t *DataY() const;
            const uint8_t *DataU() const;
            const uint8_t *DataV() const;
            const uint8_t *DataUV() const;

            uint8_t *MutableDataY();
            uint8_t *MutableDataU();
            uint8_t *MutableDataV();
            uint8_t *MutableDataUV();

            int StrideY() const;
            int StrideU() const;
            int StrideV() const;
            int StrideUV() const;

            void AddRef() const { ref_count_.IncRef(); }
            void RelRef() const
            {
                if (ref_count_.DecRef() == RefCountReleaseStatus::kDroppedLastRef)
                    delete this;
            }
            bool HasOneRef() const { return ref_count_.HasOneRef(); }
            int GetRefCount() { return ref_count_.GetRefCount(); }

            ImageSourceType GetImageSourceType() { return image_source_type_; }
            void SetImageSourceType(ImageSourceType type) { image_source_type_ = type; }
            uint64_t GetTimestamp() { return timestamp_; }
            void SetTimestamp(uint64_t timestamp) { timestamp_ = timestamp; }
            uint32_t GetSequence() { return sequence_; }
            void SetSequence(uint32_t sequence) { sequence_ = sequence; }

        private:
            ImageSourceType image_source_type_;
            uint64_t timestamp_;
            uint32_t sequence_;
            FrameType frame_type_;
            int width_;
            int height_;
            int stride_y_;
            int stride_u_;
            int stride_v_;
            int stride_uv_;
            uint8_t *data_y_;
            uint8_t *data_u_;
            uint8_t *data_v_;
            uint8_t *data_uv_;
            mutable RefCounter ref_count_{0};
        };

        // ==================== Video Codec Structs ====================
        enum VideoCodecType
        {
            kVideoCodecGeneric = 0,
            kVideoCodecVP8,
            kVideoCodecVP9,
            kVideoCodecAV1,
            kVideoCodecH264,
            kVideoCodecH265,
            kVideoCodecJPEG,
            kVideoCodecBUTT,
        };

        enum VideoCodecProfile
        {
            kProfileBase = 0,
            kProfileMain,
            kProfileHigh,
        };

        enum class VideoCodecComplexity
        {
            kComplexityLow = -1,
            kComplexityNormal = 0,
            kComplexityHigh = 1,
            kComplexityHigher = 2,
            kComplexityMax = 3
        };

        struct VideoCodecJPEG
        {
        };
        struct VideoJpegGrap
        {
            std::string file_path;
        };
        struct VideoCodecH264
        {
            VideoCodecProfile profile;
            int init_qp;
            int max_qp;
            int min_qp;
            int gop;
            int key_frame_interval;
        };
        struct VideoCodecHEVC
        {
            VideoCodecProfile profile;
            int init_qp;
            int max_qp;
            int min_qp;
            int gop;
            int key_frame_interval;
        };

        union VideoCodecUnion
        {
            VideoCodecH264 H264;
            VideoCodecJPEG JPEG;
            VideoCodecHEVC HEVC;
        };

        struct VideoCodecConfig
        {
            FrameType frameType;
            VideoCodecType codecType;
            int width;
            int height;
            int crop_x;
            int crop_y;
            uint32_t initBitrate;
            uint32_t maxBitrate;
            uint32_t minBitrate;
            uint32_t maxFramerate;
            VideoCodecUnion codec_specific_;
            VideoCodecComplexity complexity_;
        };

        struct AiProcessConfig
        {
            int crop_x;
            int crop_y;
            int crop_width;
            int crop_height;
            int gdc_width;
            int gdc_height;
            std::string aiconf;
            std::string cameras;
            std::vector<std::string> ai_module;
        };

        struct RateControlParameters
        {
            uint32_t target_bitrate;
            uint32_t framerate_fps;
        };

        // ==================== EncodedImageBuffer ====================
        class EncodedImageBuffer
        {
        public:
            EncodedImageBuffer(size_t size);
            EncodedImageBuffer(const uint8_t *data, size_t size);
            ~EncodedImageBuffer();

            const uint8_t *data() const;
            uint8_t *data();
            size_t size() const;
            void Realloc(size_t t);

            void AddRef() const { ref_count_.IncRef(); }
            void RelRef() const
            {
                if (ref_count_.DecRef() == RefCountReleaseStatus::kDroppedLastRef)
                    delete this;
            }
            bool HasOneRef() const { return ref_count_.HasOneRef(); }

        private:
            size_t size_;
            uint8_t *buffer_;
            mutable RefCounter ref_count_{0};
        };

        // ==================== CodecSpecificInfo ====================
        struct CodecSpecificInfoH264
        {
            bool idr_frame;
            uint8_t *sps_pps;
            uint32_t sps_pps_len;
        };

        union CodecSpecificInfoUnion
        {
            CodecSpecificInfoH264 H264;
        };

        struct CodecSpecificInfo
        {
            CodecSpecificInfo();
            CodecSpecificInfo(const CodecSpecificInfo &);
            ~CodecSpecificInfo();

            VideoCodecType codecType;
            CodecSpecificInfoUnion codecSpecific;
        };

        // ==================== 编码帧广播器（智能指针版本） ====================
        class EncodedImageSinkInterface
        {
        public:
            virtual ~EncodedImageSinkInterface() = default;
            virtual void OnFrame(EncodedImageBuffer *encoded_image, const CodecSpecificInfo *codec_specific_info) = 0;
            virtual void OnDiscardedFrame() {}
        };

        class EncodedImageSourceInterface
        {
        public:
            virtual ~EncodedImageSourceInterface() = default;
            virtual void AddOrUpdateSink(std::shared_ptr<EncodedImageSinkInterface> sink) = 0;
            virtual void RemoveSink(std::shared_ptr<EncodedImageSinkInterface> sink) = 0;
        };

        class EncodedImageBroadcaster : public EncodedImageSourceInterface, public EncodedImageSinkInterface
        {
        public:
            EncodedImageBroadcaster();
            ~EncodedImageBroadcaster() override;

            void AddOrUpdateSink(std::shared_ptr<EncodedImageSinkInterface> sink) override;
            void RemoveSink(std::shared_ptr<EncodedImageSinkInterface> sink) override;
            bool frame_wanted() const;
            void OnFrame(EncodedImageBuffer *encoded_image, const CodecSpecificInfo *codec_specific_info) override;
            void OnDiscardedFrame() override;

        protected:
            mutable std::mutex sinks_lock_;

        private:
            struct SinkEntry
            {
                std::weak_ptr<EncodedImageSinkInterface> sink;
            };
            std::vector<SinkEntry> sinks_;
        };

        // ==================== 采集帧广播器（智能指针版本） ====================
        class VideoSinkInterface
        {
        public:
            virtual ~VideoSinkInterface() = default;
            virtual void OnFrame(VideoFrameBuffer *frame) = 0;
            virtual void OnDiscardedFrame() {}
        };

        class VideoSourceInterface
        {
        public:
            virtual ~VideoSourceInterface() = default;
            virtual void AddOrUpdateSink(std::shared_ptr<VideoSinkInterface> sink) = 0;
            virtual void RemoveSink(std::shared_ptr<VideoSinkInterface> sink) = 0;
        };

        class VideoBroadcaster : public VideoSourceInterface, public VideoSinkInterface
        {
        public:
            VideoBroadcaster();
            ~VideoBroadcaster() override;

            void AddOrUpdateSink(std::shared_ptr<VideoSinkInterface> sink) override;
            void RemoveSink(std::shared_ptr<VideoSinkInterface> sink) override;
            bool frame_wanted() const;
            void OnFrame(VideoFrameBuffer *frame) override;
            void OnDiscardedFrame() override;

        protected:
            mutable std::mutex sinks_lock_;

        private:
            struct SinkEntry
            {
                std::weak_ptr<VideoSinkInterface> sink;
            };
            std::vector<SinkEntry> sinks_;
        };

        // ==================== FramerateController ====================
        class FramerateController
        {
        public:
            FramerateController();
            explicit FramerateController(double max_framerate);
            ~FramerateController();

            void SetMaxFramerate(double max_framerate);
            double GetMaxFramerate() const;
            bool ShouldDropFrame(int64_t in_timestamp_ns);
            void Reset();
            void KeepFrame(int64_t in_timestamp_ns);

        private:
            double max_framerate_;
            boost::optional<int64_t> next_frame_timestamp_ns_;
        };

        // ==================== FrameRateEstimator ====================
        class FrameRateEstimator
        {
        public:
            explicit FrameRateEstimator(int64_t averaging_window_us);
            void OnFrame(int64_t time_stamp_now_us);
            boost::optional<double> GetAverageFps() const;
            boost::optional<double> GetAverageFps(int64_t time_stamp_now_us);
            void Reset();

        private:
            void CullOld(int64_t time_stamp_now_us);
            const int64_t averaging_window_us_;
            std::deque<int64_t> frame_times_;
        };

        // ==================== H264 Tools ====================
        namespace H264
        {
            const size_t kNaluLongStartSequenceSize = 4;
            const size_t kNaluShortStartSequenceSize = 3;
            const size_t kNaluTypeSize = 1;

            enum NaluType : uint8_t
            {
                kSlice = 1,
                kIdr = 5,
                kSei = 6,
                kSps = 7,
                kPps = 8,
                kAud = 9,
                kEndOfSequence = 10,
                kEndOfStream = 11,
                kFiller = 12,
                kPrefix = 14,
                kStapA = 24,
                kFuA = 28
            };

            enum SliceType : uint8_t
            {
                kP = 0,
                kB = 1,
                kI = 2,
                kSp = 3,
                kSi = 4
            };

            struct NaluIndex
            {
                size_t start_offset;
                size_t payload_start_offset;
                size_t payload_size;
            };

            std::vector<NaluIndex> FindNaluIndices(const uint8_t *buffer, size_t buffer_size);
            NaluType ParseNaluType(uint8_t data);
            std::vector<uint8_t> ParseRbsp(const uint8_t *data, size_t length);
        }

        // ==================== FourCC ====================
        enum FourCC
        {
            FOURCC_I420 = CRICKET_FOURCC('I', '4', '2', '0'),
            FOURCC_I422 = CRICKET_FOURCC('I', '4', '2', '2'),
            FOURCC_I444 = CRICKET_FOURCC('I', '4', '4', '4'),
            FOURCC_I411 = CRICKET_FOURCC('I', '4', '1', '1'),
            FOURCC_I400 = CRICKET_FOURCC('I', '4', '0', '0'),
            FOURCC_NV21 = CRICKET_FOURCC('N', 'V', '2', '1'),
            FOURCC_NV12 = CRICKET_FOURCC('N', 'V', '1', '2'),
            FOURCC_YUY2 = CRICKET_FOURCC('Y', 'U', 'Y', '2'),
            FOURCC_UYVY = CRICKET_FOURCC('U', 'Y', 'V', 'Y'),
            FOURCC_M420 = CRICKET_FOURCC('M', '4', '2', '0'),
            FOURCC_ARGB = CRICKET_FOURCC('A', 'R', 'G', 'B'),
            FOURCC_BGRA = CRICKET_FOURCC('B', 'G', 'R', 'A'),
            FOURCC_ABGR = CRICKET_FOURCC('A', 'B', 'G', 'R'),
            FOURCC_24BG = CRICKET_FOURCC('2', '4', 'B', 'G'),
            FOURCC_RAW = CRICKET_FOURCC('r', 'a', 'w', ' '),
            FOURCC_RGBA = CRICKET_FOURCC('R', 'G', 'B', 'A'),
            FOURCC_RGBP = CRICKET_FOURCC('R', 'G', 'B', 'P'),
            FOURCC_RGBO = CRICKET_FOURCC('R', 'G', 'B', 'O'),
            FOURCC_R444 = CRICKET_FOURCC('R', '4', '4', '4'),
            FOURCC_RGGB = CRICKET_FOURCC('R', 'G', 'G', 'B'),
            FOURCC_BGGR = CRICKET_FOURCC('B', 'G', 'G', 'R'),
            FOURCC_GRBG = CRICKET_FOURCC('G', 'R', 'B', 'G'),
            FOURCC_GBRG = CRICKET_FOURCC('G', 'B', 'R', 'G'),
            FOURCC_MJPG = CRICKET_FOURCC('M', 'J', 'P', 'G'),
            FOURCC_YV12 = CRICKET_FOURCC('Y', 'V', '1', '2'),
            FOURCC_YV16 = CRICKET_FOURCC('Y', 'V', '1', '6'),
            FOURCC_YV24 = CRICKET_FOURCC('Y', 'V', '2', '4'),
            FOURCC_YU12 = CRICKET_FOURCC('Y', 'U', '1', '2'),
            FOURCC_J420 = CRICKET_FOURCC('J', '4', '2', '0'),
            FOURCC_J400 = CRICKET_FOURCC('J', '4', '0', '0'),
            FOURCC_IYUV = CRICKET_FOURCC('I', 'Y', 'U', 'V'),
            FOURCC_YU16 = CRICKET_FOURCC('Y', 'U', '1', '6'),
            FOURCC_YU24 = CRICKET_FOURCC('Y', 'U', '2', '4'),
            FOURCC_YUYV = CRICKET_FOURCC('Y', 'U', 'Y', 'V'),
            FOURCC_YUVS = CRICKET_FOURCC('y', 'u', 'v', 's'),
            FOURCC_HDYC = CRICKET_FOURCC('H', 'D', 'Y', 'C'),
            FOURCC_2VUY = CRICKET_FOURCC('2', 'v', 'u', 'y'),
            FOURCC_JPEG = CRICKET_FOURCC('J', 'P', 'E', 'G'),
            FOURCC_DMB1 = CRICKET_FOURCC('d', 'm', 'b', '1'),
            FOURCC_BA81 = CRICKET_FOURCC('B', 'A', '8', '1'),
            FOURCC_RGB3 = CRICKET_FOURCC('R', 'G', 'B', '3'),
            FOURCC_BGR3 = CRICKET_FOURCC('B', 'G', 'R', '3'),
            FOURCC_CM32 = CRICKET_FOURCC(0, 0, 0, 32),
            FOURCC_CM24 = CRICKET_FOURCC(0, 0, 0, 24),
            FOURCC_H264 = CRICKET_FOURCC('H', '2', '6', '4'),
        };

        inline std::string GetFourccName(uint32_t fourcc)
        {
            std::string name;
            name.push_back(static_cast<char>(fourcc & 0xFF));
            name.push_back(static_cast<char>((fourcc >> 8) & 0xFF));
            name.push_back(static_cast<char>((fourcc >> 16) & 0xFF));
            name.push_back(static_cast<char>((fourcc >> 24) & 0xFF));
            return name;
        }

    } // namespace video_common
} // namespace ecovacs

#endif // VIDEO_COMMON_DEFINES_H_