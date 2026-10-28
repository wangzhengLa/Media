#include "api_video_record.h"

#include "log.h"
#include "osp.h"

#include <cstring>
#include <vector>

namespace ecovacs
{
    VideoRecord::VideoRecord() = default;

    VideoRecord::~VideoRecord() = default;

    std::unique_ptr<VideoRecord> VideoRecord::Create()
    {
        auto record = std::make_unique<VideoRecord>();
        if (0 != record->Init())
        {
            ump_error("VideoRecord: Init failed\n");
            return nullptr;
        }
        return record;
    }

    int32_t VideoRecord::Init()
    {
        recorder_ = std::make_unique<ump::video_recording::Mp4Recorder>();

        if (IPC_INTELLI_SOK != IntelliBufCreate(&buf_hndl_, kMaxQueueSize))
        {
            ump_error("VideoRecord: IntelliBufCreate failed\n");
            recorder_.reset();
            return -1;
        }
        for (int i = 0; i < kMaxQueueSize; i++)
        {
            if (IPC_INTELLI_SOK != IntelliPutBuffer(&buf_hndl_, &record_buf_[i],
                                                    IPC_INTELLI_EMPTY_BUFFER_FLAG))
            {
                ump_error("VideoRecord: IntelliPutBuffer[%d] failed\n", i);
                IntelliBufDelete(&buf_hndl_);
                buf_hndl_ = {};
                recorder_.reset();
                return -1;
            }
        }

        return 0;
    }

    int32_t VideoRecord::UnInit()
    {
        std::lock_guard<std::mutex> lock(api_lock_);

        if (buf_hndl_.emptyQue.pu64Queue)
        {
            IntelliBufDelete(&buf_hndl_);
            buf_hndl_ = {};
        }

        if (recorder_)
        {
            recorder_->Stop();
            recorder_.reset();
        }

        return 0;
    }

    int32_t VideoRecord::StartRecord(const std::string &filename, int width, int height, int fps)
    {
        bool expected = false;
        if (!recording_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            ump_warn("VideoRecord: already recording\n");
            return -1;
        }

        if (!recorder_ || recorder_->Start(filename, width, height, fps) != 0)
        {
            ump_error("VideoRecord: recorder Start failed\n");
            recording_.store(false, std::memory_order_release);
            return -1;
        }

        quit_.store(false, std::memory_order_release);

        {
            std::lock_guard<std::mutex> lock(api_lock_);
            write_thread_ = std::thread(&VideoRecord::WriteThread, this);
        }

        ump_info("VideoRecord: started %s %dx%d@%d\n", filename.c_str(), width, height, fps);
        return 0;
    }

    int32_t VideoRecord::StopRecord()
    {
        bool expected = true;
        if (recording_.compare_exchange_strong(expected, false, std::memory_order_acq_rel))
        {
            quit_.store(true, std::memory_order_release);
            if (write_thread_.joinable())
            {
                write_thread_.join();
            }
            DrainQueue();
        }

        UnInit();
        ump_info("VideoRecord: stopped\n");
        return 0;
    }

    bool VideoRecord::IsRecording()
    {
        return recording_.load(std::memory_order_acquire);
    }

    void VideoRecord::DrainQueue()
    {
        TRecordBuffer *ptFrame = nullptr;
        while (nullptr != (ptFrame = (TRecordBuffer *)IntelliGetBuffer(
                   &buf_hndl_, IPC_INTELLI_FULL_BUFFER_FLAG, IPC_INTELLI_TIMEOUT_NONE)))
        {
            if (ptFrame->h264_frame)
            {
                ptFrame->h264_frame->RelRef();
                ptFrame->h264_frame = nullptr;
            }
            IntelliPutBuffer(&buf_hndl_, ptFrame, IPC_INTELLI_EMPTY_BUFFER_FLAG);
        }
    }

    void VideoRecord::OnFrame(video_common::EncodedImageBuffer *encoded_image,
                              const video_common::CodecSpecificInfo *codec_specific_info)
    {
        if (!encoded_image || !codec_specific_info)
        {
            return;
        }

        if (!recording_.load(std::memory_order_acquire))
        {
            return;
        }

        if (codec_specific_info->codecType != video_common::kVideoCodecH264)
        {
            return;
        }

        uint8_t *frame_data = encoded_image->data();
        uint32_t frame_size = encoded_image->size();

        // IDR frame: NALU type 5
        bool is_idr = (frame_size >= 5 && (frame_data[4] & 0x1F) == 5);

        TRecordBuffer *ptFrame = (TRecordBuffer *)IntelliGetBuffer(
            &buf_hndl_, IPC_INTELLI_EMPTY_BUFFER_FLAG, IPC_INTELLI_TIMEOUT_NONE);
        if (!ptFrame)
        {
            // Queue full: drop the oldest frame and retry once.
            TRecordBuffer *old = (TRecordBuffer *)IntelliGetBuffer(
                &buf_hndl_, IPC_INTELLI_FULL_BUFFER_FLAG, IPC_INTELLI_TIMEOUT_NONE);
            if (old)
            {
                if (old->h264_frame)
                {
                    old->h264_frame->RelRef();
                    old->h264_frame = nullptr;
                }
                IntelliPutBuffer(&buf_hndl_, old, IPC_INTELLI_EMPTY_BUFFER_FLAG);
                ump_warn("VideoRecord: queue full, drop oldest frame\n");
            }
            ptFrame = (TRecordBuffer *)IntelliGetBuffer(
                &buf_hndl_, IPC_INTELLI_EMPTY_BUFFER_FLAG, IPC_INTELLI_TIMEOUT_NONE);
        }
        if (!ptFrame)
        {
            ump_error("VideoRecord: queue full, drop current frame\n");
            return;
        }

        if (is_idr)
        {
            // Prepend SPS/PPS to the IDR frame so Mp4Recorder can create the track.
            const auto &h264 = codec_specific_info->codecSpecific.H264;
            uint32_t sps_pps_size = h264.sps_pps_len;
            uint8_t *sps_pps_data = h264.sps_pps;

            std::vector<uint8_t> data(frame_size + sps_pps_size);
            if (sps_pps_data && sps_pps_size > 0)
            {
                memcpy(data.data(), sps_pps_data, sps_pps_size);
            }
            memcpy(data.data() + sps_pps_size, frame_data, frame_size);

            auto *adapter = new video_common::EncodedImageBuffer(data.data(), data.size());
            adapter->AddRef();
            ptFrame->h264_frame = adapter;
        }
        else
        {
            encoded_image->AddRef();
            ptFrame->h264_frame = encoded_image;
        }

        IntelliPutBuffer(&buf_hndl_, ptFrame, IPC_INTELLI_FULL_BUFFER_FLAG);
    }

    void VideoRecord::WriteThread()
    {
        const uint32_t kSleepMs = 5;
        while (true)
        {
            TRecordBuffer *ptFrame = (TRecordBuffer *)IntelliGetBuffer(
                &buf_hndl_, IPC_INTELLI_FULL_BUFFER_FLAG, IPC_INTELLI_TIMEOUT_NONE);
            if (!ptFrame)
            {
                if (quit_.load(std::memory_order_acquire))
                {
                    break;
                }
                IntelliUtilSleepMs(kSleepMs);
                continue;
            }

            video_common::EncodedImageBuffer *frame = ptFrame->h264_frame;
            if (frame)
            {
                bool is_idr = (frame->size() >= 5 && (frame->data()[4] & 0x1F) == 5);
                recorder_->WriteH264Frame(frame->data(), frame->size(), is_idr);
                frame->RelRef();
            }

            ptFrame->h264_frame = nullptr;
            IntelliPutBuffer(&buf_hndl_, ptFrame, IPC_INTELLI_EMPTY_BUFFER_FLAG);
        }
    }

} // namespace ecovacs
