#include "api_video_quirc_identify.h"

#include <sys/prctl.h>

#include <cstring>
#include <memory>

#include "cjson/cJSON.h"
#include "libyuv.h"
#include "log.h"
#include "osp.h"
#include "ros2_executor.h"
#include "topic_names.h"

namespace ecovacs
{

    VideoQuircIdentify::VideoQuircIdentify()
        : quirc_identify_started_(false),
          quit_(false),
          skip_frame_num_(0),
          print_frame_cnt_(0),
          qr_state_(std::make_unique<QRState>()) {} // 初始化二维码状态

    VideoQuircIdentify::~VideoQuircIdentify() {}

    VideoQuircIdentify *VideoQuircIdentify::Create()
    {
        std::unique_ptr<VideoQuircIdentify> video_quirc_identify(
            new VideoQuircIdentify());
        if (0 != video_quirc_identify->Init())
        {
            ump_error("video_quirc_identify init failed\n");
            return nullptr;
        }
        return video_quirc_identify.release();
    }

    void VideoQuircIdentify::AIMemcpy(uint8_t *dst, const uint8_t *src, size_t n)
    {
        libyuv::CopyPlane(src, n, dst, n, n, 1);
    }

    int32_t VideoQuircIdentify::Init()
    {
        if (IPC_INTELLI_SOK !=
            IntelliBufCreate(&frame_buf_hndl_, MAX_QUIRC_FRAME_QUEUE_SIZE))
        {
            ump_error("frame_buf_hndl_ create failed\n");
            return -1;
        }
        for (int idy = 0; idy < MAX_QUIRC_FRAME_QUEUE_SIZE; idy++)
        {
            if (IPC_INTELLI_SOK != IntelliPutBuffer(&frame_buf_hndl_, &frame_buf_[idy],
                                                    IPC_INTELLI_EMPTY_BUFFER_FLAG))
            {
                ump_error("IntelliPutBuffer msg_buf[%d] failed\n", idy);
                IntelliBufDelete(&frame_buf_hndl_);
                return -1;
            }
        }

        // 目前quirc_identify模块最大处理5帧，后续写为配置
        // frame_rate_ctl_.Reset();
        // frame_rate_ctl_.SetMaxFramerate(5);

#if defined(ENABLE_ROS2)
        qr_result_pub_node_ = std::make_shared<StringPublisher>("qr_result_publish");
        GlobalExecutor::getInstance().addNode(qr_result_pub_node_);
#endif

        return 0;
    }

    ZXing::ImageView VideoQuircIdentify::CreateZXingImageView(
        const video_common::VideoFrameBuffer *frame)
    {
        const int width = frame->width();
        const int height = frame->height();
        const uint8_t *y_plane = frame->DataY();
        const int stride_y = frame->StrideY();

        // 创建灰度图像缓冲区
        std::vector<uint8_t> grayscale(width * height);

        // 复制Y分量数据（NV12格式的亮度通道）
        if (width == stride_y)
        {
            AIMemcpy(grayscale.data(), y_plane, width * height);
        }
        else
        {
            for (int i = 0; i < height; ++i)
            {
                AIMemcpy(grayscale.data() + i * width, y_plane + i * stride_y, width);
            }
        }

        return ZXing::ImageView(grayscale.data(), width, height,
                                ZXing::ImageFormat::Lum);
    }

    bool VideoQuircIdentify::VideoQuircIdentifyProcess()
    {
        std::lock_guard<std::mutex> guard(api_lock_);
        if (quit_)
        {
            ump_info("quirc_identify is quit\n");
            return false;
        }

        int64_t start_time = 0;
        int64_t end_time = 0;
        start_time = IntelliUtilTimeMillis();
        if (quirc_identify_started_)
        {
            FrameBuffer *ptFrame = nullptr;
            ptFrame = (FrameBuffer *)IntelliGetBuffer(&frame_buf_hndl_,
                                                      IPC_INTELLI_FULL_BUFFER_FLAG,
                                                      IPC_INTELLI_TIMEOUT_NONE);
            if (ptFrame)
            {
                if (++print_frame_cnt_ % 500 == 0)
                {
                    ump_info(
                        "quirc identify frame {width:{%d} height:{%d}}, {StrideY:{%d} "
                        "StrideUV:{%d}}, "
                        "print_frame_cnt_:{%d}",
                        ptFrame->frame->width(), ptFrame->frame->height(),
                        ptFrame->frame->StrideY(), ptFrame->frame->StrideUV(),
                        print_frame_cnt_);
                }
                if (qr_state_->enabled)
                {
                    // ================== ZXING处理逻辑 ==================
                    try
                    {
                        auto imageView = CreateZXingImageView(ptFrame->frame);
                        auto results =
                            ZXing::ReadBarcodes(imageView, qr_state_->decoder_hints);

                        for (auto &&result : results)
                        {
                            if (result.isValid())
                            {
                                qr_state_->last_result = result.text();
                                ump_info("QR detected [zxing]: %s", qr_state_->last_result.c_str());

#if defined(ENABLE_ROS2)
                                auto qr_msg = std::make_unique<std_msgs::msg::String>();
                                qr_msg->data = qr_state_->last_result;
                                qr_result_pub_node_->publish_string(
                                    topics::CAMERA_IMAGE_QR_RESULT, std::move(qr_msg));
#endif
                            }
                        }
                    }
                    catch (const std::exception &e)
                    {
                        ump_error("ZXing exception: %s", e.what());
                    }
                }
                ptFrame->frame->RelRef();
                IntelliPutBuffer(&frame_buf_hndl_, ptFrame,
                                 IPC_INTELLI_EMPTY_BUFFER_FLAG);
                ptFrame = nullptr;
            }
        }

        end_time = IntelliUtilTimeMillis();
        if (end_time - start_time > 40)
        {
            printf("quirc identify end_time[%ld] -start_time[%ld] = %ld\r\n ", end_time,
                   start_time, end_time - start_time);
        }

        return true;
    }

    int32_t VideoQuircIdentify::StartQuircIdentify()
    {
        ump_info("quirc_identify_started_:%d\n", quirc_identify_started_);
        if (true == quirc_identify_started_)
        {
            ump_info("quirc identify is now running\n");
            return 0;
        }

        // start quirc identify thread;
        std::lock_guard<std::mutex> guard(api_lock_);
        {
            quit_ = false;
            quirc_identify_thread_ = std::thread([this]
                                                 {
      prctl(PR_SET_NAME, "VideoQuricIdentify", 0, 0, 0);
      const uint32_t KSleepMs = 5;
      while (VideoQuircIdentifyProcess()) {
        IntelliUtilSleepMs(KSleepMs);
      } });
        }
        quirc_identify_started_ = true;
        return 0;
    }

    int32_t VideoQuircIdentify::StopQuircIdentify()
    {
        {
            std::lock_guard<std::mutex> guard(api_lock_);
            quit_ = true;
        }

        // Make sure the quirc identify thread stops using the mutex.
        quirc_identify_thread_.join();

        std::lock_guard<std::mutex> guard(api_lock_);
        if (quirc_identify_started_)
        {
            UnInit();
            quirc_identify_started_ = false;
        }

        return 0;
    }

    bool VideoQuircIdentify::QuircIdentifyStarted()
    {
        return quirc_identify_started_;
    }

    int32_t VideoQuircIdentify::UnInit()
    {
        if (IntelliBufGetFullCount(&frame_buf_hndl_) > 0)
        {
            FrameBuffer *ptFrame = nullptr;
            ptFrame = (FrameBuffer *)IntelliGetBuffer(&frame_buf_hndl_,
                                                      IPC_INTELLI_FULL_BUFFER_FLAG,
                                                      IPC_INTELLI_TIMEOUT_NONE);
            while (nullptr != ptFrame)
            {
                IntelliPutBuffer(&frame_buf_hndl_, ptFrame,
                                 IPC_INTELLI_EMPTY_BUFFER_FLAG);
                ptFrame->frame->RelRef();
                ptFrame = nullptr;
                ptFrame = (FrameBuffer *)IntelliGetBuffer(&frame_buf_hndl_,
                                                          IPC_INTELLI_FULL_BUFFER_FLAG,
                                                          IPC_INTELLI_TIMEOUT_NONE);
            }
        }
        IntelliBufDelete(&frame_buf_hndl_);

#if defined(ENABLE_ROS2)
        qr_result_pub_node_->delete_publisher(topics::CAMERA_IMAGE_QR_RESULT);
        GlobalExecutor::getInstance().removeNode(qr_result_pub_node_);
#endif
        return 0;
    }

    void VideoQuircIdentify::OnFrame(video_common::VideoFrameBuffer *frame_buffer)
    {
        if (false == quirc_identify_started_)
        {
            ump_info("ai not start,drop the frame\n");
            return;
        }

        if (nullptr == frame_buffer)
        {
            ump_error("frame_buffer = %p\n", frame_buffer);
            return;
        }

        const int KMaxSkipFrameNum = 30;
        if (skip_frame_num_++ <= KMaxSkipFrameNum)
            return;

        // if (frame_rate_ctl_.ShouldDropFrame(IntelliUtilTimeNanos())) return;

        FrameBuffer *ptFrame = (FrameBuffer *)IntelliGetBuffer(
            &frame_buf_hndl_, IPC_INTELLI_EMPTY_BUFFER_FLAG,
            IPC_INTELLI_TIMEOUT_NONE);
        if (ptFrame)
        {
            frame_buffer->AddRef();
            ptFrame->frame = frame_buffer;
            IntelliPutBuffer(&frame_buf_hndl_, ptFrame, IPC_INTELLI_FULL_BUFFER_FLAG);
        }
        else
        {
            ump_error("VideoQuricIdentify IntelliGetBuffer Empty Failed\n");
            return;
        }
    }

} // namespace ecovacs