#include "api_jpeg_publish.h"

#include <sys/prctl.h>
#include <sys/time.h>

#include <cstring>

#include "bus.h"
#include "log.h"
#include "osp.h"
#include "ros2_executor.h"
#include "topic_names.h"

namespace ecovacs
{
    JpegPublish::JpegPublish() = default;

    JpegPublish::~JpegPublish() = default;

    std::unique_ptr<JpegPublish> JpegPublish::Create()
    {
        auto jpeg_publish = std::make_unique<JpegPublish>();
        if (0 != jpeg_publish->Init())
        {
            ump_error("Failed to create JpegPublish\n");
            return nullptr;
        }
        return jpeg_publish;
    }

    int32_t JpegPublish::Init()
    {
        if (IPC_INTELLI_SOK != IntelliBufCreate(&jpeg_buf_hndl_, MAX_JPEG_PUB_QUEUE_SIZE))
        {
            ump_error("jpeg_buf_hndl_ create failed\n");
            return -1;
        }
        for (int idy = 0; idy < MAX_JPEG_PUB_QUEUE_SIZE; idy++)
        {
            if (IPC_INTELLI_SOK != IntelliPutBuffer(&jpeg_buf_hndl_, &jpeg_buf_[idy],
                                                    IPC_INTELLI_EMPTY_BUFFER_FLAG))
            {
                ump_error("IntelliPutBuffer jpeg_buf_[%d] failed\n", idy);
                IntelliBufDelete(&jpeg_buf_hndl_);
                return -1;
            }
        }
#if defined(ENABLE_ROS2)
        image_pub_node_ = std::make_shared<ImagePublisher>("jpeg_publish");
        GlobalExecutor::getInstance().addNode(image_pub_node_);
#endif
        return 0;
    }

    int32_t JpegPublish::StartPublish()
    {
        // 原子操作：只有 false 才能设置为 true
        bool expected = false;
        if (!publishStarted_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            ump_info("publish is already running\n");
            return 0;
        }

        // 重置退出标志
        quit_.store(false, std::memory_order_release);

        // 启动发布线程
        {
            std::lock_guard<std::mutex> guard(api_lock_);
            publishThread_ = std::thread([this]
                                         {
                prctl(PR_SET_NAME, "JpegPublish", 0, 0, 0);
                while (PublishProcess())
                {
                    IntelliUtilSleepMs(5);
                } });
        }

        return 0;
    }

    int32_t JpegPublish::StopPublish()
    {
        // 原子操作：只有 true 才能设置为 false
        bool expected = true;
        if (!publishStarted_.compare_exchange_strong(expected, false, std::memory_order_acq_rel))
        {
            ump_info("publish is not running\n");
            return 0;
        }

        // 设置退出标志
        quit_.store(true, std::memory_order_release);

        // 等待发布线程结束
        if (publishThread_.joinable())
        {
            publishThread_.join();
            ump_info("Publish thread joined\n");
        }

        // 清理资源
        UnInit();

        return 0;
    }

    bool JpegPublish::PublishStarted()
    {
        return publishStarted_.load(std::memory_order_acquire);
    }

    bool JpegPublish::PublishProcess()
    {
        // 快速退出检查
        if (quit_.load(std::memory_order_acquire))
        {
            return false;
        }

        if (!publishStarted_.load(std::memory_order_acquire))
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
#if defined(ENABLE_ROS2)
            auto msg = std::make_unique<sensor_msgs::msg::CompressedImage>();
            msg->format = "jpeg";
            msg->header.frame_id = "camera_compressed_jpeg";
            msg->data.resize(ptFrame->jpeg_frame->size());
            std::memcpy(msg->data.data(), ptFrame->jpeg_frame->data(),
                        ptFrame->jpeg_frame->size());
            image_pub_node_->publish_image(topics::CAMERA_IMAGE_JPEG, std::move(msg));
#endif
            ptFrame->jpeg_frame->RelRef();
            IntelliPutBuffer(&jpeg_buf_hndl_, ptFrame, IPC_INTELLI_EMPTY_BUFFER_FLAG);
            ptFrame = nullptr;
        }

        return true;
    }

    void JpegPublish::OnFrame(
        video_common::EncodedImageBuffer *encoded_image,
        const video_common::CodecSpecificInfo *codec_specific_info)
    {
        if (nullptr == encoded_image)
        {
            ump_error("encoded_image = %p\n", encoded_image);
            return;
        }

        // 快速检查是否还在发布
        if (!publishStarted_.load(std::memory_order_acquire))
        {
            return;
        }

        TJpegBuffer *ptFrame = (TJpegBuffer *)IntelliGetBuffer(
            &jpeg_buf_hndl_, IPC_INTELLI_EMPTY_BUFFER_FLAG, IPC_INTELLI_TIMEOUT_NONE);
        if (ptFrame)
        {
            encoded_image->AddRef();
            ptFrame->jpeg_frame = encoded_image;
            IntelliPutBuffer(&jpeg_buf_hndl_, ptFrame, IPC_INTELLI_FULL_BUFFER_FLAG);
        }
        else
        {
            ump_error("JpegPublish IntelliGetBuffer Empty Failed\n");
        }
    }

    int32_t JpegPublish::UnInit()
    {
        std::lock_guard<std::mutex> guard(api_lock_);

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

#if defined(ENABLE_ROS2)
        if (image_pub_node_)
        {
            image_pub_node_->delete_publisher(topics::CAMERA_IMAGE_JPEG);
            GlobalExecutor::getInstance().removeNode(image_pub_node_);
            image_pub_node_.reset();
        }
#endif

        return 0;
    }

} // namespace ecovacs