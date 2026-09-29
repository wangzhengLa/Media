#include <memory>
#include <sys/time.h>
#include "api_jpeg_upload.h"
#include "log.h"
#include "osp.h"
#include "bus.h"
#include <cstring>

namespace ecovacs
{
    JpegUpload::JpegUpload() = default;

    JpegUpload::~JpegUpload() = default;

    std::unique_ptr<JpegUpload> JpegUpload::Create()
    {
        auto jpeg_upload = std::make_unique<JpegUpload>();
        if (0 != jpeg_upload->Init())
        {
            ump_error("Failed to create JpegUpload\n");
            return nullptr;
        }
        return jpeg_upload;
    }

    int32_t JpegUpload::Init()
    {
        if (IPC_INTELLI_SOK != IntelliBufCreate(&jpeg_buf_hndl_, MAX_JPEG_QUEUE_SIZE))
        {
            ump_error("jpeg_buf_hndl_ create failed\n");
            return -1;
        }
        for (int idy = 0; idy < MAX_JPEG_QUEUE_SIZE; idy++)
        {
            if (IPC_INTELLI_SOK != IntelliPutBuffer(&jpeg_buf_hndl_, &jpeg_buf_[idy],
                                                    IPC_INTELLI_EMPTY_BUFFER_FLAG))
            {
                ump_error("IntelliPutBuffer jpeg_buf_[%d] failed\n", idy);
                IntelliBufDelete(&jpeg_buf_hndl_);
                return -1;
            }
        }
        return 0;
    }

    int32_t JpegUpload::StartUpload(video_common::VideoJpegGrap grap_conf)
    {
        // 原子操作：只有 false 才能设置为 true
        bool expected = false;
        if (!uploadStarted_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            ump_info("upload is already running\n");
            return 0;
        }

        // 重置退出标志
        quit_.store(false, std::memory_order_release);

        // 启动上传线程
        {
            std::lock_guard<std::mutex> guard(api_lock_);
            grap_config_ = grap_conf;
            uploadThread_ = std::thread([this]
                                        {
                while (UploadProcess())
                {
                    // 线程内已有 sleep
                } });
        }

        return 0;
    }

    int32_t JpegUpload::StopUpload()
    {
        // 原子操作：只有 true 才能设置为 false
        bool expected = true;
        if (!uploadStarted_.compare_exchange_strong(expected, false, std::memory_order_acq_rel))
        {
            ump_info("upload is not running\n");
            return 0;
        }

        // 设置退出标志
        quit_.store(true, std::memory_order_release);

        // 等待上传线程结束
        if (uploadThread_.joinable())
        {
            uploadThread_.join();
            ump_info("Upload thread joined\n");
        }

        // 清理资源
        UnInit();

        return 0;
    }

    bool JpegUpload::UploadStarted()
    {
        return uploadStarted_.load(std::memory_order_acquire);
    }

    bool JpegUpload::UploadProcess()
    {
        // 快速退出检查
        if (quit_.load(std::memory_order_acquire))
        {
            return false;
        }

        if (!uploadStarted_.load(std::memory_order_acquire))
        {
            return false;
        }

        std::lock_guard<std::mutex> guard(api_lock_);

        // 再次检查退出标志
        if (quit_.load(std::memory_order_acquire))
        {
            return false;
        }

        TJpegBuffer *ptFrame = nullptr;
        ptFrame = (TJpegBuffer *)IntelliGetBuffer(&jpeg_buf_hndl_,
                                                  IPC_INTELLI_FULL_BUFFER_FLAG,
                                                  IPC_INTELLI_TIMEOUT_NONE);
        if (ptFrame)
        {
            // 保存 JPEG 文件
            IUtilMemToFile(ptFrame->jpeg_frame->data(),
                           ptFrame->jpeg_frame->size(),
                           grap_config_.file_path.c_str());

            // 当前抓拍筛选完成，复位静态变量，主动暂停
            quit_.store(true, std::memory_order_release);

            // 上报暂停事件，触发停止 pipeline
            const char *string = "{\"cmd\":\"stop_grab_pic\"}";
            bus_put_message((char *)string, strlen(string));

            ptFrame->jpeg_frame->RelRef();
            IntelliPutBuffer(&jpeg_buf_hndl_, ptFrame, IPC_INTELLI_EMPTY_BUFFER_FLAG);
            ptFrame = nullptr;
        }

        IntelliUtilSleep(5 * 1000);

        return true;
    }

    void JpegUpload::OnFrame(video_common::EncodedImageBuffer *encoded_image,
                             const video_common::CodecSpecificInfo *codec_specific_info)
    {
        if (nullptr == encoded_image)
        {
            ump_error("encoded_image = %p\n", encoded_image);
            return;
        }

        // 快速检查是否还在上传
        if (!uploadStarted_.load(std::memory_order_acquire))
        {
            return;
        }

        TJpegBuffer *ptFrame = (TJpegBuffer *)IntelliGetBuffer(
            &jpeg_buf_hndl_, IPC_INTELLI_EMPTY_BUFFER_FLAG, IPC_INTELLI_TIMEOUT_NONE);
        if (ptFrame)
        {
            ptFrame->jpeg_frame = encoded_image;
            IntelliPutBuffer(&jpeg_buf_hndl_, ptFrame, IPC_INTELLI_FULL_BUFFER_FLAG);
            encoded_image->AddRef();
        }
        else
        {
            ump_error("JpegUpload IntelliGetBuffer Empty Failed\n");
        }
    }

    int32_t JpegUpload::UnInit()
    {
        std::lock_guard<std::mutex> guard(api_lock_);

        ump_info("JpegUpload UnInit\n");
        if (IntelliBufGetFullCount(&jpeg_buf_hndl_) > 0)
        {
            TJpegBuffer *ptFrame = nullptr;
            ptFrame = (TJpegBuffer *)IntelliGetBuffer(&jpeg_buf_hndl_,
                                                      IPC_INTELLI_FULL_BUFFER_FLAG,
                                                      IPC_INTELLI_TIMEOUT_NONE);
            while (nullptr != ptFrame)
            {
                IntelliPutBuffer(&jpeg_buf_hndl_, ptFrame, IPC_INTELLI_EMPTY_BUFFER_FLAG);
                if (ptFrame->jpeg_frame)
                {
                    ptFrame->jpeg_frame->RelRef();
                }
                ptFrame = nullptr;
                ptFrame = (TJpegBuffer *)IntelliGetBuffer(&jpeg_buf_hndl_,
                                                          IPC_INTELLI_FULL_BUFFER_FLAG,
                                                          IPC_INTELLI_TIMEOUT_NONE);
            }
        }
        IntelliBufDelete(&jpeg_buf_hndl_);

        return 0;
    }

    void JpegUpload::StartUrlUpload(std::string upload_cfg, int32_t iot_cfg_flag)
    {
        // 静态方法，保持不变
    }

} // namespace ecovacs