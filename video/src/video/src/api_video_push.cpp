#include "api_video_push.h"
#include <log.h>
#include "osp.h"
#include <cstring>

namespace ecovacs
{
    VideoPush::VideoPush() = default;

    VideoPush::~VideoPush() = default;

    std::unique_ptr<VideoPush> VideoPush::Create()
    {
        auto video_push = std::make_unique<VideoPush>();
        if (0 != video_push->Init())
        {
            ump_error("Failed to create VideoPush\n");
            return nullptr;
        }
        return video_push;
    }

    int32_t VideoPush::Init()
    {
        ump_info("VideoPush Init\n");
        if (IPC_INTELLI_SOK != IntelliBufCreate(&h264_buf_hndl_, MAX_H264_QUEUE_SIZE))
        {
            ump_error("h264_buf_hndl_ create failed\n");
            return -1;
        }
        for (int idy = 0; idy < MAX_H264_QUEUE_SIZE; idy++)
        {
            if (IPC_INTELLI_SOK != IntelliPutBuffer(&h264_buf_hndl_, &h264_buf_[idy],
                                                    IPC_INTELLI_EMPTY_BUFFER_FLAG))
            {
                ump_error("IntelliPutBuffer msg_buf[%d] failed\n", idy);
                IntelliBufDelete(&h264_buf_hndl_);
                return -1;
            }
        }

#if defined(ENABLE_ROS2)
        image_pub_node_ = std::make_shared<ImagePublisher>("h264_publish");
        GlobalExecutor::getInstance().addNode(image_pub_node_);
#endif

#ifdef ENABLE_ICEORYX
        // 创建 iceoryx H264 发布者
        iceoryx_h264_publisher_ = std::make_unique<iox::popo::Publisher<H264Frame>>(
            iox::capro::ServiceDescription{"H264", "Video", "Stream"});
        ump_info("iceoryx H264 publisher initialized\n");
#endif

        return 0;
    }

    int32_t VideoPush::StartPush()
    {
        // 原子操作：只有 false 才能设置为 true
        bool expected = false;
        if (!pushStarted_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            ump_info("push is already running\n");
            return 0;
        }

        // 重置退出标志
        quit_.store(false, std::memory_order_release);

        // 启动推送线程
        {
            std::lock_guard<std::mutex> guard(api_lock_);
            pushThread_ = std::thread([this]
                                      {
                const uint32_t KSleepMs = 5;
                while (PushProcess())
                {
                    IntelliUtilSleepMs(KSleepMs);
                } });
        }

        return 0;
    }

    int32_t VideoPush::StopPush()
    {
        // 原子操作：只有 true 才能设置为 false
        bool expected = true;
        if (!pushStarted_.compare_exchange_strong(expected, false, std::memory_order_acq_rel))
        {
            ump_info("push is not running\n");
            return 0;
        }

        // 设置退出标志
        quit_.store(true, std::memory_order_release);

        // 等待推送线程结束
        if (pushThread_.joinable())
        {
            pushThread_.join();
            ump_info("Push thread joined\n");
        }

        // 清理资源
        UnInit();

        return 0;
    }

    bool VideoPush::PushStarted()
    {
        return pushStarted_.load(std::memory_order_acquire);
    }

    bool VideoPush::PushProcess()
    {
        // 快速退出检查
        if (quit_.load(std::memory_order_acquire))
        {
            return false;
        }

        if (!pushStarted_.load(std::memory_order_acquire))
        {
            return false;
        }

        std::lock_guard<std::mutex> guard(api_lock_);

        // 再次检查退出标志
        if (quit_.load(std::memory_order_acquire))
        {
            return false;
        }

        TH264Buffer *ptFrame = nullptr;
        ptFrame = (TH264Buffer *)IntelliGetBuffer(&h264_buf_hndl_,
                                                  IPC_INTELLI_FULL_BUFFER_FLAG,
                                                  IPC_INTELLI_TIMEOUT_NONE);
        if (ptFrame)
        {
            // 获取 H264 帧数据
            uint8_t *frame_data = ptFrame->h264_frame->data();
            uint32_t frame_size = ptFrame->h264_frame->size();

#if defined(ENABLE_ROS2)
            auto msg = std::make_unique<sensor_msgs::msg::CompressedImage>();
            msg->format = "h264";
            msg->header.frame_id = "camera_compressed_h264";
            msg->data.resize(frame_size);
            std::memcpy(msg->data.data(), frame_data, frame_size);
            image_pub_node_->publish_image(topics::CAMERA_IMAGE_H264, std::move(msg));
#endif

#ifdef ENABLE_ICEORYX
            if (iceoryx_h264_publisher_)
            {
                auto sample = iceoryx_h264_publisher_->loan();
                if (!sample.has_error())
                {
                    auto &h264_frame = *sample.value();
                    if (frame_size <= H264Frame::MAX_H264_SIZE)
                    {
                        h264_frame.size = frame_size;
                        std::memcpy(h264_frame.data, frame_data, frame_size);
                        sample.value().publish();

                        static int iceoryx_cnt = 0;
                        if (++iceoryx_cnt % 100 == 0)
                            ump_info("iceoryx H264 published %d frames\n", iceoryx_cnt);
                    }
                    else
                    {
                        ump_error("iceoryx: H264 frame too large (%u > %u)\n",
                                  frame_size, H264Frame::MAX_H264_SIZE);
                    }
                }
                else
                {
                    ump_error("iceoryx: failed to loan H264 sample\n");
                }
            }
#endif

            ptFrame->h264_frame->RelRef();
            IntelliPutBuffer(&h264_buf_hndl_, ptFrame, IPC_INTELLI_EMPTY_BUFFER_FLAG);
            ptFrame = nullptr;
        }

        return true;
    }

    void VideoPush::OnFrame(
        video_common::EncodedImageBuffer *encoded_image,
        const video_common::CodecSpecificInfo *codec_specific_info)
    {
        if (nullptr == encoded_image || nullptr == codec_specific_info)
        {
            ump_error("encoded_image = %p codec_specific_info = %p\n", encoded_image,
                      codec_specific_info);
            return;
        }

        // 快速检查是否还在推送
        if (!pushStarted_.load(std::memory_order_acquire))
        {
            return;
        }

        if (codec_specific_info->codecType != video_common::kVideoCodecH264)
        {
            ump_error("codecType %d error\r\n", codec_specific_info->codecType);
            return;
        }

        TH264Buffer *ptFrame = (TH264Buffer *)IntelliGetBuffer(
            &h264_buf_hndl_, IPC_INTELLI_EMPTY_BUFFER_FLAG, IPC_INTELLI_TIMEOUT_NONE);
        if (ptFrame)
        {
            // 检测到IDR主动插入sps_pps数据
            if (5 == (encoded_image->data()[4] & 0x1F))
            {
                uint32_t h264_size = encoded_image->size();
                uint32_t sps_pps_size =
                    codec_specific_info->codecSpecific.H264.sps_pps_len;
                uint8_t *h264_data = encoded_image->data();
                uint8_t *sps_pps_data = codec_specific_info->codecSpecific.H264.sps_pps;
                uint8_t *data = (uint8_t *)malloc(h264_size + sps_pps_size);
                if (sps_pps_data)
                    memcpy(data, sps_pps_data, sps_pps_size);
                memcpy(data + sps_pps_size, h264_data, h264_size);
                video_common::EncodedImageBuffer *encoded_image_adapter =
                    new video_common::EncodedImageBuffer(data, h264_size + sps_pps_size);
                free(data);
                encoded_image_adapter->AddRef();
                ptFrame->h264_frame = encoded_image_adapter;
            }
            else
            {
                encoded_image->AddRef();
                ptFrame->h264_frame = encoded_image;
            }
            IntelliPutBuffer(&h264_buf_hndl_, ptFrame, IPC_INTELLI_FULL_BUFFER_FLAG);
        }
        else
        {
            ump_error("VideoPush IntelliGetBuffer Empty Failed\n");
        }
    }

    int32_t VideoPush::UnInit()
    {
        ump_info("VideoPush UnInit\n");

        // 清空队列
        std::lock_guard<std::mutex> guard(api_lock_);
        if (IntelliBufGetFullCount(&h264_buf_hndl_) > 0)
        {
            TH264Buffer *ptFrame = nullptr;
            ptFrame = (TH264Buffer *)IntelliGetBuffer(&h264_buf_hndl_,
                                                      IPC_INTELLI_FULL_BUFFER_FLAG,
                                                      IPC_INTELLI_TIMEOUT_NONE);
            while (nullptr != ptFrame)
            {
                IntelliPutBuffer(&h264_buf_hndl_, ptFrame, IPC_INTELLI_EMPTY_BUFFER_FLAG);
                if (ptFrame->h264_frame)
                {
                    ptFrame->h264_frame->RelRef();
                }
                ptFrame = nullptr;
                ptFrame = (TH264Buffer *)IntelliGetBuffer(&h264_buf_hndl_,
                                                          IPC_INTELLI_FULL_BUFFER_FLAG,
                                                          IPC_INTELLI_TIMEOUT_NONE);
            }
        }
        IntelliBufDelete(&h264_buf_hndl_);

#if defined(ENABLE_ROS2)
        if (image_pub_node_)
        {
            image_pub_node_->delete_publisher(topics::CAMERA_IMAGE_H264);
            GlobalExecutor::getInstance().removeNode(image_pub_node_);
            image_pub_node_.reset();
        }
#endif

#ifdef ENABLE_ICEORYX
        iceoryx_h264_publisher_.reset();
#endif

        return 0;
    }

} // namespace ecovacs