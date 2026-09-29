#include <memory>
#include <sys/time.h>
#include "api_raw_video_save.h"
#include "log.h"
#include "osp.h"
#include <cstring>
#include "bus.h"

namespace ecovacs
{
    RawVideoSave::RawVideoSave() = default;

    RawVideoSave::~RawVideoSave() = default;

    std::unique_ptr<RawVideoSave> RawVideoSave::Create()
    {
        auto raw_video_save = std::make_unique<RawVideoSave>();
        if (0 != raw_video_save->Init())
        {
            ump_error("Failed to create RawVideoSave\n");
            return nullptr;
        }
        return raw_video_save;
    }

    int32_t RawVideoSave::Init()
    {
        if (IPC_INTELLI_SOK != IntelliBufCreate(&frame_buf_hndl_, MAX_RAW_FRAME_QUEUE_SIZE))
        {
            ump_error("frame_buf_hndl_ create failed\n");
            return -1;
        }
        for (int idy = 0; idy < MAX_RAW_FRAME_QUEUE_SIZE; idy++)
        {
            if (IPC_INTELLI_SOK != IntelliPutBuffer(&frame_buf_hndl_, &frame_buf_[idy],
                                                    IPC_INTELLI_EMPTY_BUFFER_FLAG))
            {
                ump_error("IntelliPutBuffer frame_buf_[%d] failed\n", idy);
                IntelliBufDelete(&frame_buf_hndl_);
                return -1;
            }
        }
        return 0;
    }

    int32_t RawVideoSave::StartSave()
    {
        // 原子操作：只有 false 才能设置为 true
        bool expected = false;
        if (!saveStarted_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            ump_info("save is already running\n");
            return 0;
        }

        // 重置标志
        quit_.store(false, std::memory_order_release);
        save_raw_video_flag_.store(false, std::memory_order_release);

        // 启动保存线程
        {
            std::lock_guard<std::mutex> guard(api_lock_);
            saveThread_ = std::thread([this]
                                      {
                while (SaveProcess())
                {
                    // 线程内已有 sleep
                } });
        }

        return 0;
    }

    int32_t RawVideoSave::StopSave()
    {
        // 原子操作：只有 true 才能设置为 false
        bool expected = true;
        if (!saveStarted_.compare_exchange_strong(expected, false, std::memory_order_acq_rel))
        {
            ump_info("save is not running\n");
            return 0;
        }

        // 设置退出标志
        quit_.store(true, std::memory_order_release);

        // 等待保存线程结束
        if (saveThread_.joinable())
        {
            saveThread_.join();
            ump_info("Save thread joined\n");
        }

        // 清理资源
        UnInit();

        return 0;
    }

    bool RawVideoSave::SaveStarted()
    {
        return saveStarted_.load(std::memory_order_acquire);
    }

    bool RawVideoSave::SaveProcess()
    {
        char file_name[256] = {0};

        // 快速退出检查
        if (quit_.load(std::memory_order_acquire))
        {
            return false;
        }

        if (!saveStarted_.load(std::memory_order_acquire))
        {
            return false;
        }

        std::lock_guard<std::mutex> guard(api_lock_);

        // 再次检查退出标志
        if (quit_.load(std::memory_order_acquire))
        {
            return false;
        }

        FrameBuffer *ptFrame = nullptr;
        ptFrame = (FrameBuffer *)IntelliGetBuffer(&frame_buf_hndl_,
                                                  IPC_INTELLI_FULL_BUFFER_FLAG,
                                                  IPC_INTELLI_TIMEOUT_NONE);
        if (ptFrame)
        {
            if (false == save_raw_video_flag_.load(std::memory_order_acquire))
            {
                if (video_common::kI420 == ptFrame->frame->type())
                {
                    sprintf(file_name, "/tmp/factory_test_%d_%d_i420.yuv",
                            ptFrame->frame->width(), ptFrame->frame->height());
                    IUtilImageToFile(ptFrame->frame->MutableDataY(),
                                     ptFrame->frame->MutableDataU(),
                                     ptFrame->frame->MutableDataV(),
                                     ptFrame->frame->width(),
                                     ptFrame->frame->height(),
                                     ptFrame->frame->StrideY(),
                                     ptFrame->frame->StrideU(),
                                     ptFrame->frame->StrideV(),
                                     0, file_name);
                }
                else if (video_common::kNV12 == ptFrame->frame->type())
                {
                    sprintf(file_name, "/tmp/factory_test_%d_%d_nv12.yuv",
                            ptFrame->frame->width(), ptFrame->frame->height());
                    IUtilImageToFile(ptFrame->frame->MutableDataY(),
                                     ptFrame->frame->MutableDataUV(),
                                     NULL,
                                     ptFrame->frame->width(),
                                     ptFrame->frame->height(),
                                     ptFrame->frame->StrideY(),
                                     ptFrame->frame->StrideUV(),
                                     ptFrame->frame->StrideUV(),
                                     1, file_name);
                }
                else if (video_common::kNV21 == ptFrame->frame->type())
                {
                    sprintf(file_name, "/tmp/factory_test_%d_%d_nv21.yuv",
                            ptFrame->frame->width(), ptFrame->frame->height());
                    IUtilImageToFile(ptFrame->frame->MutableDataY(),
                                     ptFrame->frame->MutableDataUV(),
                                     NULL,
                                     ptFrame->frame->width(),
                                     ptFrame->frame->height(),
                                     ptFrame->frame->StrideY(),
                                     ptFrame->frame->StrideUV(),
                                     ptFrame->frame->StrideUV(),
                                     2, file_name);
                }

                // 对于工厂测试而言，每一次触发抓拍原图，目前只是保存一张图片
                save_raw_video_flag_.store(true, std::memory_order_release);
            }
            else
            {
                ump_info("raw_video has saved one image\n");
                // 当前出vin图已经完成，主动暂停
                quit_.store(true, std::memory_order_release);

                // 上报暂停事件，触发停止 pipeline
                const char *string = "{\"cmd\":\"stop_save_raw_video\"}";
                bus_put_message((char *)string, strlen(string));
            }

            ptFrame->frame->RelRef();
            IntelliPutBuffer(&frame_buf_hndl_, ptFrame, IPC_INTELLI_EMPTY_BUFFER_FLAG);
            ptFrame = nullptr;
        }

        IntelliUtilSleep(5 * 1000);

        return true;
    }

    void RawVideoSave::OnFrame(video_common::VideoFrameBuffer *frame_buffer)
    {
        if (nullptr == frame_buffer)
        {
            ump_error("frame_buffer is nullptr\n");
            return;
        }

        // 快速检查是否还在保存
        if (!saveStarted_.load(std::memory_order_acquire))
        {
            return;
        }

        FrameBuffer *ptFrame = (FrameBuffer *)IntelliGetBuffer(&frame_buf_hndl_,
                                                               IPC_INTELLI_EMPTY_BUFFER_FLAG,
                                                               IPC_INTELLI_TIMEOUT_NONE);
        if (ptFrame)
        {
            ptFrame->frame = frame_buffer;
            IntelliPutBuffer(&frame_buf_hndl_, ptFrame, IPC_INTELLI_FULL_BUFFER_FLAG);
            frame_buffer->AddRef();
        }
        else
        {
            // 队列满，丢弃帧
        }
    }

    int32_t RawVideoSave::UnInit()
    {
        std::lock_guard<std::mutex> guard(api_lock_);

        ump_info("RawVideoSave UnInit\n");
        if (IntelliBufGetFullCount(&frame_buf_hndl_) > 0)
        {
            FrameBuffer *ptFrame = nullptr;
            ptFrame = (FrameBuffer *)IntelliGetBuffer(&frame_buf_hndl_,
                                                      IPC_INTELLI_FULL_BUFFER_FLAG,
                                                      IPC_INTELLI_TIMEOUT_NONE);
            while (nullptr != ptFrame)
            {
                IntelliPutBuffer(&frame_buf_hndl_, ptFrame, IPC_INTELLI_EMPTY_BUFFER_FLAG);
                if (ptFrame->frame)
                {
                    ptFrame->frame->RelRef();
                }
                ptFrame = nullptr;
                ptFrame = (FrameBuffer *)IntelliGetBuffer(&frame_buf_hndl_,
                                                          IPC_INTELLI_FULL_BUFFER_FLAG,
                                                          IPC_INTELLI_TIMEOUT_NONE);
            }
        }
        IntelliBufDelete(&frame_buf_hndl_);

        return 0;
    }

} // namespace ecovacs