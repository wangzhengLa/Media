#include <memory>
#include <chrono>
#include "api_video_encode.h"
#include "video_encoder_factory.h"
#include "log.h"
#include "osp.h"
#include "libyuv.h"

namespace ecovacs
{
    VideoEncode::VideoEncode() = default;

    VideoEncode::~VideoEncode() = default;

    std::unique_ptr<VideoEncode> VideoEncode::Create(const video_common::VideoCodecConfig *codec_settings)
    {
        auto vem_encode = std::make_unique<VideoEncode>();
        if (0 != vem_encode->Init(codec_settings))
        {
            ump_error("Failed to create VideoEncode\n");
            return nullptr;
        }
        return vem_encode;
    }

    int32_t VideoEncode::Init(const video_common::VideoCodecConfig *codec_settings)
    {
        if (nullptr == codec_settings)
        {
            ump_error("codec_settings is null\n");
            return -1;
        }

        if (IPC_INTELLI_SOK != IntelliBufCreate(&frame_buf_hndl_, MAX_FRAME_QUEUE_SIZE))
        {
            ump_error("IntelliBufCreate failed\n");
            return -1;
        }
        for (int idy = 0; idy < MAX_FRAME_QUEUE_SIZE; idy++)
        {
            if (IPC_INTELLI_SOK != IntelliPutBuffer(&frame_buf_hndl_, &frame_buf_[idy], IPC_INTELLI_EMPTY_BUFFER_FLAG))
            {
                ump_error("IntelliPutBuffer failed for idx %d\n", idy);
                IntelliBufDelete(&frame_buf_hndl_);
                return -1;
            }
        }

        vem_ = std::unique_ptr<VideoEncoderModule>(VideoEncoderFactory::Create(codec_settings->codecType));
        if (!vem_)
        {
            ump_error("VideoEncoderFactory::Create failed\n");
            IntelliBufDelete(&frame_buf_hndl_);
            return -1;
        }

        if (0 != vem_->InitEncode(codec_settings))
        {
            ump_error("vem_->InitEncode failed\n");
            vem_.reset();
            IntelliBufDelete(&frame_buf_hndl_);
            return -1;
        }

        frame_rate_ctl_.Reset();
        frame_rate_ctl_.SetMaxFramerate(codec_settings->maxFramerate);
        ump_info("[VideoEncode] framerate limit set to %.1f fps\n", codec_settings->maxFramerate);

        cfg_ = *codec_settings;
        return 0;
    }

    int32_t VideoEncode::Release()
    {
        if (!vem_)
        {
            ump_error("vem_ not created\n");
            return -1;
        }

        DeRegisterEncodeCompleteCallback();
        vem_->Release();
        vem_.reset();

        return 0;
    }

    int32_t VideoEncode::RegisterEncodeCompleteCallback(video_common::EncodedImageSinkInterface *callback)
    {
        if (!vem_)
        {
            ump_error("vem_ not created\n");
            return -1;
        }
        if (!callback)
        {
            ump_error("callback is nullptr\n");
            return -1;
        }

        std::lock_guard<std::mutex> guard(api_lock_);
        vem_->RegisterEncodeCompleteCallback(callback);
        return 0;
    }

    int32_t VideoEncode::DeRegisterEncodeCompleteCallback()
    {
        if (!vem_)
        {
            ump_error("vem_ not created\n");
            return -1;
        }

        std::lock_guard<std::mutex> guard(api_lock_);
        vem_->DeRegisterEncodeCompleteCallback();
        return 0;
    }

    int32_t VideoEncode::Encode(const video_common::VideoFrameBuffer *frame_buffer)
    {
        if (!vem_)
        {
            ump_error("vem_ not created\n");
            return -1;
        }
        if (0 != vem_->Encode(frame_buffer))
        {
            ump_error("Encode failed\n");
            frame_buffer->RelRef();
            return -1;
        }

        frame_buffer->RelRef();
        return 0;
    }

    int32_t VideoEncode::StartEncode()
    {
        bool expected = false;
        if (!encodeStarted_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            ump_info("Encode already started\n");
            return 0;
        }

        quit_.store(false, std::memory_order_release);

        {
            std::lock_guard<std::mutex> guard(api_lock_);
            encodeThread_ = std::thread([this]
                                        {
                while (EncodeProcess())
                {
                    IntelliUtilSleep(5 * 1000);
                } });
        }

        return 0;
    }

    int32_t VideoEncode::StopEncode()
    {
        bool expected = true;
        if (!encodeStarted_.compare_exchange_strong(expected, false, std::memory_order_acq_rel))
        {
            ump_info("Encode not running\n");
            return 0;
        }

        quit_.store(true, std::memory_order_release);

        // 1. 注销回调
        {
            std::lock_guard<std::mutex> guard(api_lock_);
            if (vem_)
            {
                vem_->DeRegisterEncodeCompleteCallback();
                ump_info("DeRegistered encode callback\n");
            }
        }

        // 2. 等待编码线程结束
        if (encodeThread_.joinable())
        {
            encodeThread_.join();
            ump_info("Encode thread joined\n");
        }

        // 3. 清空队列
        {
            std::lock_guard<std::mutex> guard(api_lock_);
            if (IntelliBufGetFullCount(&frame_buf_hndl_) > 0)
            {
                FrameBuffer *ptFrame = nullptr;
                ptFrame = (FrameBuffer *)IntelliGetBuffer(&frame_buf_hndl_, IPC_INTELLI_FULL_BUFFER_FLAG, IPC_INTELLI_TIMEOUT_NONE);
                while (nullptr != ptFrame)
                {
                    IntelliPutBuffer(&frame_buf_hndl_, ptFrame, IPC_INTELLI_EMPTY_BUFFER_FLAG);
                    if (ptFrame->frame)
                    {
                        ptFrame->frame->RelRef();
                    }
                    ptFrame = nullptr;
                    ptFrame = (FrameBuffer *)IntelliGetBuffer(&frame_buf_hndl_, IPC_INTELLI_FULL_BUFFER_FLAG, IPC_INTELLI_TIMEOUT_NONE);
                }
            }
            IntelliBufDelete(&frame_buf_hndl_);
        }

        // 4. 释放底层资源
        if (vem_)
        {
            vem_->Release();
            vem_.reset();
        }

        return 0;
    }

    bool VideoEncode::EncodeStarted()
    {
        return encodeStarted_.load(std::memory_order_acquire);
    }

    int32_t VideoEncode::RequestKeyFrame()
    {
        std::lock_guard<std::mutex> guard(api_lock_);
        if (EncodeStarted() && vem_)
        {
            vem_->RequestKeyFrame();
        }
        return 0;
    }

    int32_t VideoEncode::SetRates(video_common::RateControlParameters &parameters)
    {
        std::lock_guard<std::mutex> guard(api_lock_);
        if (EncodeStarted() && vem_)
        {
            vem_->SetRates(parameters);
        }
        return 0;
    }

    bool VideoEncode::EncodeProcess()
    {
        if (quit_.load(std::memory_order_acquire))
        {
            return false;
        }

        if (!encodeStarted_.load(std::memory_order_acquire))
        {
            return false;
        }

        std::lock_guard<std::mutex> guard(api_lock_);
        if (quit_.load(std::memory_order_acquire))
        {
            return false;
        }

        FrameBuffer *ptFrame = nullptr;
        ptFrame = (FrameBuffer *)IntelliGetBuffer(&frame_buf_hndl_, IPC_INTELLI_FULL_BUFFER_FLAG, IPC_INTELLI_TIMEOUT_NONE);
        if (ptFrame)
        {
            // ====== 编码性能统计（每秒打印一次） ======
            static int64_t s_last_log_us = 0;
            static uint32_t s_frame_count = 0;
            static int64_t s_total_us = 0;
            static int64_t s_max_us = 0;

            auto t0 = std::chrono::steady_clock::now();
            int64_t frame_start_us = std::chrono::duration_cast<std::chrono::microseconds>(
                t0.time_since_epoch()).count();

            Encode(ptFrame->frame);

            auto t1 = std::chrono::steady_clock::now();
            int64_t encode_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

            s_frame_count++;
            s_total_us += encode_us;
            if (encode_us > s_max_us) s_max_us = encode_us;

            if (s_last_log_us == 0) s_last_log_us = frame_start_us;
            if (frame_start_us - s_last_log_us >= 1000000LL) {
                double fps = s_frame_count * 1000000.0 / (frame_start_us - s_last_log_us);
                double avg_ms = (s_total_us / (double)s_frame_count) / 1000.0;
                double max_ms = s_max_us / 1000.0;
                ump_info("[PERF] encode: fps=%.1f, avg=%.2fms, max=%.2fms, frames=%u\n",
                         fps, avg_ms, max_ms, s_frame_count);
                s_frame_count = 0;
                s_total_us = 0;
                s_max_us = 0;
                s_last_log_us = frame_start_us;
            }
            // ============================================

            IntelliPutBuffer(&frame_buf_hndl_, ptFrame, IPC_INTELLI_EMPTY_BUFFER_FLAG);
        }

        return true;
    }

    void VideoEncode::OnFrame(video_common::VideoFrameBuffer *frame_buffer)
    {
        if (nullptr == frame_buffer)
        {
            ump_error("frame_buffer is nullptr\n");
            return;
        }

        if (!encodeStarted_.load(std::memory_order_acquire))
        {
            return;
        }

        if (frame_rate_ctl_.ShouldDropFrame(IntelliUtilTimeNanos()))
            return;

        FrameBuffer *ptFrame = (FrameBuffer *)IntelliGetBuffer(&frame_buf_hndl_, IPC_INTELLI_EMPTY_BUFFER_FLAG, IPC_INTELLI_TIMEOUT_NONE);
        if (ptFrame)
        {
            ptFrame->frame = frame_buffer;
            IntelliPutBuffer(&frame_buf_hndl_, ptFrame, IPC_INTELLI_FULL_BUFFER_FLAG);
            frame_buffer->AddRef();
        }
        else
        {
            ump_error("IntelliGetBuffer Empty Failed, dropping frame\n");
        }
    }

} // namespace ecovacs