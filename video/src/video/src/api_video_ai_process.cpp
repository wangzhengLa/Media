#include "api_video_ai_process.h"
#include <chrono>

namespace ecovacs
{
    VideoAI::VideoAI()
        : ai_started_(false), quit_(false)
#ifdef ENABLE_WEBSOCKETS
          ,
          jpeg_quality_(85), enable_jpeg_(false), jpeg_inited_(false)
#endif
    {
    }

    VideoAI::~VideoAI() = default;

    std::unique_ptr<VideoAI> VideoAI::Create()
    {
        auto video_ai = std::make_unique<VideoAI>();
        if (0 != video_ai->Init())
        {
            ump_error("[VideoAI] Init failed");
            return nullptr;
        }
        return video_ai;
    }

    void VideoAI::AIMemcpy(uint8_t *dst, const uint8_t *src, size_t n)
    {
        libyuv::CopyPlane(src, n, dst, n, n, 1);
    }

    int32_t VideoAI::Init()
    {
        ump_info("[VideoAI] Initializing...");

        if (IPC_INTELLI_SOK != IntelliBufCreate(&frame_buf_hndl_, MAX_AI_FRAME_QUEUE_SIZE))
        {
            ump_error("[VideoAI] Frame buffer create failed");
            return -1;
        }

        for (int idy = 0; idy < MAX_AI_FRAME_QUEUE_SIZE; idy++)
        {
            if (IPC_INTELLI_SOK != IntelliPutBuffer(&frame_buf_hndl_, &frame_buf_[idy], IPC_INTELLI_EMPTY_BUFFER_FLAG))
            {
                ump_error("[VideoAI] Put buffer failed for idx %d", idy);
                IntelliBufDelete(&frame_buf_hndl_);
                return -1;
            }
        }

        frame_rate_ctl_.Reset();
        frame_rate_ctl_.SetMaxFramerate(10);
        ump_info("[VideoAI] Framerate set to 10 fps");

#if defined(ENABLE_ROS2)
        frame_pub_node_ = std::make_shared<FramePublisher>("nv12_publish");
        GlobalExecutor::getInstance().addNode(frame_pub_node_);
        ump_info("[VideoAI] ROS2 publisher initialized");
#endif

#ifdef ENABLE_WEBSOCKETS
        jpeg_encoder_ = std::make_unique<JpegEncoder>();
        ws_server_ = std::make_unique<WebSocketServer>();
        ump_info("[VideoAI] WebSocket initialized");
#endif

#ifdef ENABLE_ICEORYX
        iceoryx_publisher_ = std::make_unique<iox::popo::Publisher<NV12Frame>>(
            iox::capro::ServiceDescription{"NV12", "Image", "Frame"});
        ump_info("[VideoAI] iceoryx publisher initialized");
#endif

        ump_info("[VideoAI] Init complete");
        return 0;
    }

#if defined(ENABLE_ROS2)
    void VideoAI::publishToROS2(video_common::VideoFrameBuffer *frame)
    {
        uint32_t width = frame->width();
        uint32_t height = frame->height();
        bool is_rgb = (frame->type() == video_common::FrameType::kRGB24);

        auto msg = std::make_unique<sensor_msgs::msg::Image>();
        msg->header.frame_id = is_rgb ? "camera_frame_rgb" : "camera_frame_nv12";
        msg->header.stamp = rclcpp::Time(frame->timestamp() * 1000000ULL);
        msg->width = width;
        msg->height = height;
        msg->is_bigendian = false;

        if (is_rgb)
        {
            msg->encoding = "rgb8";
            msg->step = width * 3;
            uint32_t rgb_size = width * height * 3;
            msg->data.resize(rgb_size);
            int src_stride = frame->StrideY();
            int dst_stride = width * 3;
            if (src_stride == dst_stride)
            {
                AIMemcpy(msg->data.data(), frame->DataY(), rgb_size);
            }
            else
            {
                for (uint32_t row = 0; row < height; row++)
                    AIMemcpy(msg->data.data() + row * dst_stride,
                             frame->DataY() + row * src_stride,
                             dst_stride);
            }
        }
        else
        {
            uint32_t y_size = width * height;
            uint32_t uv_size = (width * height) / 2;
            msg->encoding = "nv12";
            msg->step = width;
            msg->data.resize(y_size + uv_size);

            uint8_t *ros2_buf_y = &msg->data[0];
            uint8_t *ros2_buf_uv = &msg->data[y_size];

            if (frame->StrideY() != static_cast<int>(width))
            {
                for (uint32_t row = 0; row < height; row++)
                    AIMemcpy(ros2_buf_y + row * width,
                             frame->DataY() + row * frame->StrideY(), width);
            }
            else
            {
                AIMemcpy(ros2_buf_y, frame->DataY(), y_size);
            }

            if (frame->StrideUV() != static_cast<int>(width))
            {
                for (uint32_t row = 0; row < height / 2; row++)
                    AIMemcpy(ros2_buf_uv + row * width,
                             frame->DataUV() + row * frame->StrideUV(), width);
            }
            else
            {
                AIMemcpy(ros2_buf_uv, frame->DataUV(), uv_size);
            }
        }

        frame_pub_node_->publish_image(topics::CAMERA_IMAGE_NV12, std::move(msg));
    }
#endif

#ifdef ENABLE_WEBSOCKETS
    void VideoAI::publishToWebSocket(video_common::VideoFrameBuffer *frame)
    {
        if (!enable_jpeg_ || !ws_server_ || ws_server_->GetClientCount() == 0)
            return;

        uint32_t width = frame->width();
        uint32_t height = frame->height();
        bool is_rgb = (frame->type() == video_common::FrameType::kRGB24);
        int stride_y = frame->StrideY();

        if (!jpeg_inited_ ||
            jpeg_encoder_->GetWidth() != static_cast<int>(width) ||
            jpeg_encoder_->GetHeight() != static_cast<int>(height))
        {
            int stride_uv = is_rgb ? 0 : frame->StrideUV();
            jpeg_inited_ = jpeg_encoder_->Init(width, height, stride_y, stride_uv);
            if (!jpeg_inited_)
            {
                ump_error("[VideoAI] JPEG init failed for %dx%d", width, height);
                enable_jpeg_ = false;
                return;
            }
            ump_info("[VideoAI] JPEG encoder reinitialized: %dx%d (%s)",
                     width, height, is_rgb ? "RGB" : "NV12");
        }

        std::vector<uint8_t> jpeg_data;
        if (is_rgb)
        {
            jpeg_data = jpeg_encoder_->EncodeRGB(frame->DataY(), stride_y, jpeg_quality_);
        }
        else
        {
            jpeg_data = jpeg_encoder_->EncodeNV12(frame->DataY(), frame->DataUV(), jpeg_quality_);
        }

        if (!jpeg_data.empty())
        {
            ws_server_->QueueFrame(jpeg_data);
            static int jpeg_cnt = 0;
            if (++jpeg_cnt % 100 == 0)
            {
                ump_info("[VideoAI] JPEG: %zu bytes, clients=%zu",
                         jpeg_data.size(), ws_server_->GetClientCount());
            }
        }
    }
#endif

#ifdef ENABLE_ICEORYX
    void VideoAI::publishToIceoryx(video_common::VideoFrameBuffer *frame)
    {
        if (!iceoryx_publisher_)
            return;

        uint32_t width = frame->width();
        uint32_t height = frame->height();
        bool is_rgb = (frame->type() == video_common::FrameType::kRGB24);

        auto sample = iceoryx_publisher_->loan();
        if (sample.has_error())
            return;

        auto &iox_frame = *sample.value();
        iox_frame.width = width;
        iox_frame.height = height;
        iox_frame.format = is_rgb ? 1 : 0;

        if (is_rgb)
        {
            // RGB24: 单平面
            iox_frame.stride = width * 3;
            uint32_t rgb_size = width * height * 3;
            int src_stride = frame->StrideY();
            int dst_stride = width * 3;
            if (src_stride == dst_stride)
            {
                AIMemcpy(iox_frame.data, frame->DataY(), rgb_size);
            }
            else
            {
                for (uint32_t row = 0; row < height; ++row)
                    AIMemcpy(iox_frame.data + row * dst_stride,
                             frame->DataY() + row * src_stride,
                             dst_stride);
            }
        }
        else
        {
            iox_frame.stride = width;
            uint32_t y_size = width * height;
            uint32_t uv_size = y_size / 2;

            if (frame->StrideY() != static_cast<int>(width))
            {
                for (uint32_t row = 0; row < height; ++row)
                    AIMemcpy(iox_frame.data + row * width,
                             frame->DataY() + row * frame->StrideY(), width);
            }
            else
            {
                AIMemcpy(iox_frame.data, frame->DataY(), y_size);
            }

            uint8_t *uv_dst = iox_frame.data + y_size;
            if (frame->StrideUV() != static_cast<int>(width))
            {
                for (uint32_t row = 0; row < height / 2; ++row)
                    AIMemcpy(uv_dst + row * width,
                             frame->DataUV() + row * frame->StrideUV(), width);
            }
            else
            {
                AIMemcpy(uv_dst, frame->DataUV(), uv_size);
            }
        }

        sample.value().publish();

        static int iceoryx_cnt = 0;
        if (++iceoryx_cnt % 100 == 0)
        {
            ump_info("[VideoAI] iceoryx: %d frames", iceoryx_cnt);
        }
    }
#endif

    void VideoAI::processFrame(video_common::VideoFrameBuffer *frame)
    {
        if (!frame)
            return;

#if defined(ENABLE_ROS2)
        publishToROS2(frame);
#endif
#ifdef ENABLE_WEBSOCKETS
        publishToWebSocket(frame);
#endif
#ifdef ENABLE_ICEORYX
        publishToIceoryx(frame);
#endif
    }

    bool VideoAI::VideoAIProcess()
    {
        if (quit_.load(std::memory_order_acquire))
            return false;

        if (!ai_started_.load(std::memory_order_acquire))
            return false;

        std::lock_guard<std::mutex> guard(api_lock_);

        if (quit_.load(std::memory_order_acquire))
            return false;

        FrameBuffer *ptFrame = static_cast<FrameBuffer *>(IntelliGetBuffer(&frame_buf_hndl_,
                                                                           IPC_INTELLI_FULL_BUFFER_FLAG,
                                                                           IPC_INTELLI_TIMEOUT_NONE));
        if (ptFrame)
        {
            processFrame(ptFrame->frame);
            ptFrame->frame->RelRef();
            IntelliPutBuffer(&frame_buf_hndl_, ptFrame, IPC_INTELLI_EMPTY_BUFFER_FLAG);
        }
        return true;
    }

    int32_t VideoAI::StartAI()
    {
        ump_info("[VideoAI] StartAI");

        bool expected = false;
        if (!ai_started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            ump_info("[VideoAI] AI already running");
            return 0;
        }

        quit_.store(false, std::memory_order_release);

        {
            std::lock_guard<std::mutex> guard(api_lock_);
            ai_thread_ = std::thread([this]()
                                     {
                ump_info("[VideoAI] AI thread started");
                while (VideoAIProcess())
                {
                    IntelliUtilSleepMs(15);
                }
                ump_info("[VideoAI] AI thread stopped"); });
        }

        return 0;
    }

    int32_t VideoAI::StopAI()
    {
        ump_info("[VideoAI] StopAI");

        bool expected = true;
        if (!ai_started_.compare_exchange_strong(expected, false, std::memory_order_acq_rel))
        {
            ump_info("[VideoAI] AI not running");
            return 0;
        }

        quit_.store(true, std::memory_order_release);

        if (ai_thread_.joinable())
        {
            ai_thread_.join();
            ump_info("[VideoAI] AI thread joined");
        }

        UnInit();

        return 0;
    }

    bool VideoAI::AIStarted()
    {
        return ai_started_.load(std::memory_order_acquire);
    }

    int32_t VideoAI::UnInit()
    {
        ump_info("[VideoAI] UnInit");

        if (ai_thread_.joinable())
        {
            ai_thread_.join();
        }

#if defined(ENABLE_ROS2)
        if (frame_pub_node_)
        {
            frame_pub_node_->delete_publisher(topics::CAMERA_IMAGE_NV12);
            GlobalExecutor::getInstance().removeNode(frame_pub_node_);
            frame_pub_node_.reset();
        }
#endif

#ifdef ENABLE_WEBSOCKETS
        if (ws_server_)
        {
            ws_server_->Stop();
            ws_server_.reset();
        }
        jpeg_encoder_.reset();
        jpeg_inited_ = false;
#endif

#ifdef ENABLE_ICEORYX
        iceoryx_publisher_.reset();
#endif

        std::lock_guard<std::mutex> guard(api_lock_);
        if (IntelliBufGetFullCount(&frame_buf_hndl_) > 0)
        {
            FrameBuffer *ptFrame = static_cast<FrameBuffer *>(IntelliGetBuffer(&frame_buf_hndl_,
                                                                               IPC_INTELLI_FULL_BUFFER_FLAG,
                                                                               IPC_INTELLI_TIMEOUT_NONE));
            while (ptFrame)
            {
                IntelliPutBuffer(&frame_buf_hndl_, ptFrame, IPC_INTELLI_EMPTY_BUFFER_FLAG);
                if (ptFrame->frame)
                    ptFrame->frame->RelRef();
                ptFrame = static_cast<FrameBuffer *>(IntelliGetBuffer(&frame_buf_hndl_,
                                                                      IPC_INTELLI_FULL_BUFFER_FLAG,
                                                                      IPC_INTELLI_TIMEOUT_NONE));
            }
        }
        IntelliBufDelete(&frame_buf_hndl_);

        return 0;
    }

    void VideoAI::OnFrame(video_common::VideoFrameBuffer *frame_buffer)
    {
        if (!frame_buffer)
            return;

        if (!ai_started_.load(std::memory_order_acquire))
            return;

        if (frame_rate_ctl_.ShouldDropFrame(IntelliUtilTimeNanos()))
            return;

        FrameBuffer *ptFrame = static_cast<FrameBuffer *>(IntelliGetBuffer(&frame_buf_hndl_,
                                                                           IPC_INTELLI_EMPTY_BUFFER_FLAG,
                                                                           IPC_INTELLI_TIMEOUT_NONE));
        if (ptFrame)
        {
            frame_buffer->AddRef();
            ptFrame->frame = frame_buffer;
            IntelliPutBuffer(&frame_buf_hndl_, ptFrame, IPC_INTELLI_FULL_BUFFER_FLAG);
        }
    }

} // namespace ecovacs