#ifndef RK_ENCODER_IMPL_H_
#define RK_ENCODER_IMPL_H_

#include "video_encoder.h"
#include "video_encoder_impl.h"

#include "rk_mpi_sys.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_venc.h"
#include "rk_comm_venc.h"
#include "log.h"

#include <atomic>
#include <mutex>
#include <vector>
#include <chrono>
#include <cstdint>
#include <array>
#include <memory>

// 条件编译保护日志宏
#ifndef LOG_INFO
#define LOG_INFO(fmt, ...) ump_info("[INFO] " fmt "\n", ##__VA_ARGS__)
#endif
#ifndef LOG_ERROR
#define LOG_ERROR(fmt, ...) ump_error("[ERROR] " fmt "\n", ##__VA_ARGS__)
#endif
#ifndef LOG_DEBUG
#define LOG_DEBUG(fmt, ...) ump_debug("[DEBUG] " fmt "\n", ##__VA_ARGS__)
#endif
#ifndef LOG_WARN
#define LOG_WARN(fmt, ...) ump_warn("[WARN] " fmt "\n", ##__VA_ARGS__)
#endif

namespace ecovacs
{
    // 常量定义
    constexpr uint32_t ALIGN(uint32_t x, uint32_t a)
    {
        return (x + a - 1) & ~(a - 1);
    }

    constexpr int MAX_VENC_CHN = 8;
    constexpr int DEFAULT_GOP = 10;
    constexpr int DEFAULT_FPS = 30;
    constexpr int DEFAULT_QP = 26;
    constexpr int DEFAULT_Q_FACTOR = 80;
    constexpr int MAX_WIDTH = 8192;
    constexpr int MAX_HEIGHT = 8192;
    constexpr int MIN_WIDTH = 64;
    constexpr int MIN_HEIGHT = 64;
    constexpr int ALIGNMENT = 16; // 硬件对齐要求

    // 错误码
    enum class RkEncResult
    {
        Success = 0,
        InvalidParam = -1,
        InitFailed = -2,
        EncodeFailed = -3,
        NotInitialized = -4,
        NoFreeChannel = -5,
        ResolutionTooLarge = -6,
        UnsupportedCodec = -7,
        UnsupportedPixelFormat = -8,
        BufferPoolFailed = -9,
        StreamFailed = -10,
    };

    // 性能统计结构
    struct EncoderPerformanceStats
    {
        std::atomic<uint64_t> total_encode_time_us{0};
        std::atomic<uint32_t> max_encode_time_us{0};
        std::atomic<uint32_t> min_encode_time_us{UINT32_MAX};
        std::atomic<uint32_t> encode_count{0};
        std::atomic<uint64_t> total_encoded_bytes{0};

        void Update(uint32_t elapsed_us, uint32_t encoded_bytes = 0)
        {
            total_encode_time_us += elapsed_us;
            total_encoded_bytes += encoded_bytes;
            encode_count++;

            uint32_t current_max = max_encode_time_us.load();
            while (elapsed_us > current_max &&
                   !max_encode_time_us.compare_exchange_weak(current_max, elapsed_us))
                ;

            uint32_t current_min = min_encode_time_us.load();
            while (elapsed_us < current_min &&
                   !min_encode_time_us.compare_exchange_weak(current_min, elapsed_us))
                ;
        }

        uint64_t GetAvgTimeUs() const
        {
            uint32_t count = encode_count.load();
            return count > 0 ? total_encode_time_us.load() / count : 0;
        }

        uint32_t GetAvgBitrateKbps() const
        {
            uint32_t count = encode_count.load();
            if (count == 0)
                return 0;
            // 假设帧率已知，这里简化处理
            return (total_encoded_bytes.load() * 8 / count) / 1000;
        }

        uint32_t GetMaxTimeUs() const { return max_encode_time_us.load(); }
        uint32_t GetMinTimeUs() const { return min_encode_time_us.load(); }
        uint32_t GetEncodeCount() const { return encode_count.load(); }
        uint64_t GetTotalTimeUs() const { return total_encode_time_us.load(); }
        uint64_t GetTotalEncodedBytes() const { return total_encoded_bytes.load(); }

        void Reset()
        {
            total_encode_time_us = 0;
            total_encoded_bytes = 0;
            max_encode_time_us = 0;
            min_encode_time_us = UINT32_MAX;
            encode_count = 0;
        }
    };

    // ============================================================================
    // MPI 系统管理（RAII + 引用计数，线程安全）
    // ============================================================================
    class RkMpiSystem
    {
    public:
        static RkMpiSystem &GetInstance()
        {
            static RkMpiSystem instance;
            return instance;
        }

        void AddRef()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ref_count_++;
            if (ref_count_ == 1)
            {
                RK_S32 ret = RK_MPI_SYS_Init();
                if (ret != RK_SUCCESS)
                {
                    LOG_ERROR("RK_MPI_SYS_Init failed %d", ret);
                    init_failed_ = true;
                }
                else
                {
                    LOG_INFO("RK_MPI_SYS_Init success");
                }
            }
        }

        void Release()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (ref_count_ > 0)
            {
                ref_count_--;
                if (ref_count_ == 0 && !init_failed_)
                {
                    RK_MPI_SYS_Exit();
                    LOG_INFO("RK_MPI_SYS_Exit success");
                }
            }
        }

        bool IsInitialized() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return ref_count_ > 0 && !init_failed_;
        }

    private:
        RkMpiSystem() : ref_count_(0), init_failed_(false) {}
        ~RkMpiSystem() = default;

        int ref_count_;
        bool init_failed_;
        mutable std::mutex mutex_;

        RkMpiSystem(const RkMpiSystem &) = delete;
        RkMpiSystem &operator=(const RkMpiSystem &) = delete;
    };

    // ============================================================================
    // VENC 通道管理器（单例，线程安全）
    // ============================================================================
    class VencChannelManager
    {
    public:
        static VencChannelManager &GetInstance()
        {
            static VencChannelManager instance;
            return instance;
        }

        int Allocate()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (int i = 0; i < MAX_VENC_CHN; ++i)
            {
                if (!used_[i])
                {
                    used_[i] = true;
                    LOG_DEBUG("Allocated VENC channel %d", i);
                    return i;
                }
            }
            LOG_ERROR("No free VENC channel available");
            return -1;
        }

        void Release(int chn)
        {
            if (chn < 0 || chn >= MAX_VENC_CHN)
                return;
            std::lock_guard<std::mutex> lock(mutex_);
            used_[chn] = false;
            LOG_DEBUG("Released VENC channel %d", chn);
        }

        int GetUsedChannelCount() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            int count = 0;
            for (int i = 0; i < MAX_VENC_CHN; ++i)
            {
                if (used_[i])
                    count++;
            }
            return count;
        }

    private:
        VencChannelManager() : used_{} {}
        ~VencChannelManager() = default;

        std::array<bool, MAX_VENC_CHN> used_;
        mutable std::mutex mutex_;
    };

    // ============================================================================
    // RkEncoderImpl
    // ============================================================================
    class RkEncoderImpl : public VideoEncoderImpl
    {
    public:
        RkEncoderImpl();
        ~RkEncoderImpl() override;

        int32_t InitEncode(const video_common::VideoCodecConfig *codec_settings) override;
        int32_t Release() override;
        int32_t Encode(const video_common::VideoFrameBuffer *frame_buffer) override;
        int32_t RequestKeyFrame() override;
        void SetRates(const video_common::RateControlParameters &parameters) override;

        // 扩展接口
        void GetPerformanceStats(EncoderPerformanceStats &stats) const;
        bool IsInitialized() const { return initialized_; }
        int GetChannelId() const { return enc_handle_.venc_chn; }

        // 禁用拷贝
        RkEncoderImpl(const RkEncoderImpl &) = delete;
        RkEncoderImpl &operator=(const RkEncoderImpl &) = delete;

    private:
        struct RkEncHandle
        {
            RK_U32 width;
            RK_U32 height;
            RK_U32 hor_stride;
            RK_U32 ver_stride;
            RK_U32 pixel_format;
            RK_CODEC_ID_E codec_type;

            std::atomic<RK_U32> frame_count;
            std::atomic<RK_U64> stream_size;

            RK_S32 venc_chn;
            VENC_CHN_ATTR_S chn_attr;
            MB_POOL mb_pool;
            VIDEO_FRAME_INFO_S frame_info;
            VENC_STREAM_S stream_info;

            RK_S32 gop;
            RK_S32 fps;
            RK_S32 qp_init;
            RK_U32 q_factor;
            RK_U32 bitrate;     // 目标码率 (kbps)
            RK_U32 bitrate_max; // 最大码率 (kbps)
            RK_U32 bitrate_min; // 最小码率 (kbps)

            std::vector<uint8_t> sps;
            std::vector<uint8_t> pps;

            RkEncHandle();
            ~RkEncHandle();
            void Reset();
        };

        RkEncHandle enc_handle_;
        bool initialized_;
        mutable std::mutex mutex_;
        EncoderPerformanceStats perf_stats_;

        // 辅助函数
        int32_t FillYUVFrame(const video_common::VideoFrameBuffer *frame_buffer, void *dst);
        int32_t ValidateAndAdjustParams(const video_common::VideoCodecConfig *codec_settings);
        int32_t ReleaseInternal();
        int32_t ConfigureEncoder(const video_common::VideoCodecConfig *codec_settings);
        int32_t CreateBufferPool();
        bool ValidateJPEGData(const uint8_t *data, size_t len);
    };

} // namespace ecovacs

#endif // RK_ENCODER_IMPL_H_