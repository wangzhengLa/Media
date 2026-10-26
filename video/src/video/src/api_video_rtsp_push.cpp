#include "api_video_rtsp_push.h"
#include <log.h>
#include "osp.h"
#include <cstring>
#include <chrono>

using namespace xop;

namespace ecovacs
{
    // ==================== 构造函数 / 析构函数 ====================

    VideoRtspPush::VideoRtspPush() = default;

    VideoRtspPush::~VideoRtspPush() = default;

    std::unique_ptr<VideoRtspPush> VideoRtspPush::Create()
    {
        auto video_push = std::make_unique<VideoRtspPush>();
        if (0 != video_push->Init())
        {
            ump_error("Failed to create VideoRtspPush\n");
            return nullptr;
        }
        return video_push;
    }

    // ==================== 初始化 / 反初始化 ====================

    int32_t VideoRtspPush::Init()
    {
        ump_info("VideoRtspPush Init\n");

        // ----- 1. 初始化队列 -----
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
        iceoryx_h264_publisher_ = std::make_unique<iox::popo::Publisher<H264Frame>>(
            iox::capro::ServiceDescription{"H264", "Video", "Stream"});
        ump_info("iceoryx H264 publisher initialized\n");
#endif

        // 初始化音频
        AudioInit();

        return 0;
    }

    int32_t VideoRtspPush::UnInit()
    {
        ump_info("VideoPush UnInit\n");

        // 先停音频
        AudioStop();
        AudioUnInit();

        // 强制停止 RTSP（安全且必要）
        StopRtspServer();

        // 清空队列
        std::lock_guard<std::mutex> guard(api_lock_);
        if (IntelliBufGetFullCount(&h264_buf_hndl_) > 0) {
            TH264Buffer *ptFrame = nullptr;
            ptFrame = (TH264Buffer *)IntelliGetBuffer(&h264_buf_hndl_,
                                                      IPC_INTELLI_FULL_BUFFER_FLAG,
                                                      IPC_INTELLI_TIMEOUT_NONE);
            while (nullptr != ptFrame) {
                IntelliPutBuffer(&h264_buf_hndl_, ptFrame, IPC_INTELLI_EMPTY_BUFFER_FLAG);
                if (ptFrame->h264_frame) {
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
        if (image_pub_node_) {
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

    // ==================== 推送控制 ====================
    int32_t VideoRtspPush::StartPush()
    {
        bool expected = false;
        if (!pushStarted_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            ump_info("push is already running\n");
            return 0;
        }

        quit_.store(false, std::memory_order_release);
        video_start_us_ = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        audio_start_us_ = video_start_us_;  // 音视频共用同一个起点

        // 自动启动 RTSP（若配置开启）
        if (rtsp_config_.auto_start) {
            if (!rtsp_started_.load(std::memory_order_acquire)) {
                ump_info("Auto-starting RTSP server (port=%u, suffix=%s)\n",
                         rtsp_config_.port, rtsp_config_.suffix.c_str());
                if (StartRtspServer(rtsp_config_) != 0) {
                    ump_warn("Auto-start RTSP server failed, continue without RTSP\n");
                }
            }
        }

        // 启动音频采集
        AudioStart();

        {
            std::lock_guard<std::mutex> guard(api_lock_);
            pushThread_ = std::thread([this] {
                const uint32_t KSleepMs = 5;
                while (PushProcess()) {
                    IntelliUtilSleepMs(KSleepMs);
                }
            });
        }

        return 0;
    }

    int32_t VideoRtspPush::StopPush()
    {
        bool expected = true;
        if (!pushStarted_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
            ump_info("push is not running\n");
            return 0;
        }

        quit_.store(true, std::memory_order_release);

        if (pushThread_.joinable()) {
            pushThread_.join();
            ump_info("Push thread joined\n");
        }

        // 停音频
        AudioStop();

        // 自动停止 RTSP（若配置开启且为自动启动）
        if (rtsp_config_.auto_start && rtsp_started_.load(std::memory_order_acquire)) {
            ump_info("Auto-stopping RTSP server\n");
            StopRtspServer();
        }

        return 0;
    }

    // ==================== 核心处理线程 ====================
    bool VideoRtspPush::PushProcess()
    {
        if (quit_.load(std::memory_order_acquire) || !pushStarted_.load(std::memory_order_acquire)) {
            return false;
        }

        std::lock_guard<std::mutex> guard(api_lock_);

        if (quit_.load(std::memory_order_acquire)) {
            return false;
        }

        // 队列深度 > 4 时跳过旧帧防止延迟累积
        // 策略：只丢 P 帧，遇到 I/IDR/SPS/PPS 帧就停止（必须保留解码起点）
        // 避免丢到 P 帧的参考帧导致 VLC 花屏
        while (IntelliBufGetFullCount(&h264_buf_hndl_) > 4) {
            TH264Buffer *old = (TH264Buffer *)IntelliGetBuffer(
                &h264_buf_hndl_, IPC_INTELLI_FULL_BUFFER_FLAG, IPC_INTELLI_TIMEOUT_NONE);
            if (!old) break;

            // 判断这个帧是不是关键帧（NALU type 5=IDR, 7=SPS, 8=PPS）
            bool is_key = false;
            uint8_t *d = old->h264_frame->data();
            uint32_t sz = old->h264_frame->size();
            int off = 0;
            if (sz >= 4 && d[0]==0 && d[1]==0 && d[2]==0 && d[3]==1) off = 4;
            else if (sz >= 3 && d[0]==0 && d[1]==0 && d[2]==1) off = 3;
            if (off > 0 && sz > off + 1) {
                uint8_t nalu_type = d[off] & 0x1F;
                if (nalu_type == 5 || nalu_type == 7 || nalu_type == 8)
                    is_key = true;
            }

            if (is_key) {
                // 遇到关键帧 → 放回去，停止 drain（它是解码起点，必须保留）
                IntelliPutBuffer(&h264_buf_hndl_, old, IPC_INTELLI_FULL_BUFFER_FLAG);
                break;
            }
            // P 帧 → 安全丢弃
            if (old->h264_frame) old->h264_frame->RelRef();
            IntelliPutBuffer(&h264_buf_hndl_, old, IPC_INTELLI_EMPTY_BUFFER_FLAG);
        }

        TH264Buffer *ptFrame = nullptr;
        ptFrame = (TH264Buffer *)IntelliGetBuffer(&h264_buf_hndl_,
                                                IPC_INTELLI_FULL_BUFFER_FLAG,
                                                IPC_INTELLI_TIMEOUT_NONE);
        if (ptFrame) {
            // ====== 视频推流性能统计（每秒打印一次） ======
            static int64_t s_push_last_log_us = 0;
            static uint32_t s_push_frame_count = 0;
            static int64_t s_push_total_us = 0;
            static int64_t s_push_max_us = 0;

            auto p0 = std::chrono::steady_clock::now();
            int64_t push_start_us = std::chrono::duration_cast<std::chrono::microseconds>(
                p0.time_since_epoch()).count();
            // ============================================

            uint8_t *frame_data = ptFrame->h264_frame->data();
            uint32_t frame_size = ptFrame->h264_frame->size();

            // ---------- 1. ROS2 发布 ----------
    #if defined(ENABLE_ROS2)
            auto msg = std::make_unique<sensor_msgs::msg::CompressedImage>();
            msg->format = "h264";
            msg->header.frame_id = "camera_compressed_h264";
            msg->data.resize(frame_size);
            std::memcpy(msg->data.data(), frame_data, frame_size);
            image_pub_node_->publish_image(topics::CAMERA_IMAGE_H264, std::move(msg));
    #endif

            // ---------- 2. iceoryx 发布 ----------
    #ifdef ENABLE_ICEORYX
            if (iceoryx_h264_publisher_) {
                auto sample = iceoryx_h264_publisher_->loan();
                if (!sample.has_error()) {
                    auto &h264_frame = *sample.value();
                    if (frame_size <= H264Frame::MAX_H264_SIZE) {
                        h264_frame.size = frame_size;
                        std::memcpy(h264_frame.data, frame_data, frame_size);
                        sample.value().publish();
                    } else {
                        ump_error("iceoryx: H264 frame too large (%u > %u)\n",
                                frame_size, H264Frame::MAX_H264_SIZE);
                    }
                }
            }
    #endif

            // ---------- 3. RTSP 推流（拆分 NALU，分开发送） ----------
            if (rtsp_started_.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> rtsp_lock(rtsp_mutex_);
                if (rtsp_server_ && rtsp_session_id_ != 0 &&
                    rtsp_client_count_.load(std::memory_order_acquire) > 0) {

                    // 获取当前时间（微秒）
                    int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();

                    // 整个访问单元共用同一个时间戳（SPS/PPS/IDR 必须一致）
                    uint32_t common_ts = static_cast<uint32_t>((now_us - video_start_us_) * 90 / 1000);

                    // 遍历整个 frame，逐个拆出 NALU 分别发送
                    uint32_t offset = 0;
                    while (offset < frame_size) {
                        // 查找起始码（支持 3/4 字节）
                        int start_len = 0;
                        if (offset + 4 <= frame_size &&
                            frame_data[offset] == 0 && frame_data[offset+1] == 0 &&
                            frame_data[offset+2] == 0 && frame_data[offset+3] == 1) {
                            start_len = 4;
                        } else if (offset + 3 <= frame_size &&
                                frame_data[offset] == 0 && frame_data[offset+1] == 0 &&
                                frame_data[offset+2] == 1) {
                            start_len = 3;
                        }

                        // 没有起始码（裸流退化情况）：剩余数据视为单个 NALU
                        if (start_len == 0) {
                            if (frame_size - offset > 0) {
                                xop::AVFrame frame;
                                frame.size = frame_size - offset;
                                frame.buffer.reset(new uint8_t[frame.size]);
                                memcpy(frame.buffer.get(), frame_data + offset, frame.size);
                                frame.timestamp = common_ts;

                                uint8_t nalu_type = frame_data[offset] & 0x1F;
                                frame.type = (nalu_type == 5) ? xop::VIDEO_FRAME_I : xop::VIDEO_FRAME_P;

                                rtsp_server_->PushFrame(rtsp_session_id_, xop::channel_0, frame);
                            }
                            break;
                        }

                        // 跳过起始码，定位 NALU 数据起始
                        uint32_t nalu_start = offset + start_len;

                        // 查找下一个起始码
                        uint32_t next_offset = nalu_start;
                        bool found_next = false;
                        while (next_offset < frame_size) {
                            if (next_offset + 4 <= frame_size &&
                                frame_data[next_offset] == 0 && frame_data[next_offset+1] == 0 &&
                                frame_data[next_offset+2] == 0 && frame_data[next_offset+3] == 1) {
                                found_next = true;
                                break;
                            } else if (next_offset + 3 <= frame_size &&
                                    frame_data[next_offset] == 0 && frame_data[next_offset+1] == 0 &&
                                    frame_data[next_offset+2] == 1) {
                                found_next = true;
                                break;
                            }
                            next_offset++;
                        }

                        // 当前 NALU 长度（不含起始码）
                        uint32_t nalu_size = found_next ? (next_offset - nalu_start) : (frame_size - nalu_start);

                        if (nalu_size > 0) {
                            xop::AVFrame frame;
                            frame.size = nalu_size;
                            frame.buffer.reset(new uint8_t[nalu_size]);
                            memcpy(frame.buffer.get(), frame_data + nalu_start, nalu_size);

                            // 所有 NALU 共用同一时间戳
                            frame.timestamp = common_ts;

                            // 只有 IDR(5) 才是 I 帧，SPS(7)/PPS(8) 均按 P 帧推
                            uint8_t nalu_type = frame_data[nalu_start] & 0x1F;
                            if (nalu_type == 5) {
                                frame.type = xop::VIDEO_FRAME_I;
                            } else {
                                frame.type = xop::VIDEO_FRAME_P;
                            }

                            rtsp_server_->PushFrame(rtsp_session_id_, xop::channel_0, frame);
                        }

                        if (found_next) {
                            offset = next_offset;
                        } else {
                            break;
                        }
                    } // while 拆 NALU 结束
                }
            }

            // 释放帧并标记缓冲区为空
            ptFrame->h264_frame->RelRef();
            IntelliPutBuffer(&h264_buf_hndl_, ptFrame, IPC_INTELLI_EMPTY_BUFFER_FLAG);
            ptFrame = nullptr;

            // ====== 推流统计收尾 ======
            auto p1 = std::chrono::steady_clock::now();
            int64_t push_us = std::chrono::duration_cast<std::chrono::microseconds>(p1 - p0).count();

            s_push_frame_count++;
            s_push_total_us += push_us;
            if (push_us > s_push_max_us) s_push_max_us = push_us;

            if (s_push_last_log_us == 0) s_push_last_log_us = push_start_us;
            if (push_start_us - s_push_last_log_us >= 1000000LL) {
                double fps = s_push_frame_count * 1000000.0 / (push_start_us - s_push_last_log_us);
                double avg_ms = (s_push_total_us / (double)s_push_frame_count) / 1000.0;
                double max_ms = s_push_max_us / 1000.0;
                ump_info("[PERF] push(video): fps=%.1f, avg=%.2fms, max=%.2fms, frames=%u\n",
                         fps, avg_ms, max_ms, s_push_frame_count);
                s_push_frame_count = 0;
                s_push_total_us = 0;
                s_push_max_us = 0;
                s_push_last_log_us = push_start_us;
            }
            // ==========================
        }

        return true;
    }

    // ==================== RTSP 服务器控制 ====================
    int32_t VideoRtspPush::StartRtspServer(const RtspConfig& config)
    {
        std::lock_guard<std::mutex> lock(rtsp_mutex_);
        if (rtsp_started_.load(std::memory_order_acquire)) {
            ump_info("RTSP server already started\n");
            return 0;
        }

        rtsp_config_ = config;

        rtsp_event_loop_ = std::make_shared<xop::EventLoop>();
        rtsp_server_ = xop::RtspServer::Create(rtsp_event_loop_.get());
        if (!rtsp_server_) {
            ump_error("Failed to create RtspServer\n");
            return -1;
        }

        if (!rtsp_server_->Start(config.ip, config.port)) {
            ump_error("Failed to start RTSP server on %s:%u\n", config.ip.c_str(), config.port);
            rtsp_server_.reset();
            return -1;
        }

        if (config.enable_auth) {
            rtsp_server_->SetAuthConfig(config.realm, config.username, config.password);
            ump_info("RTSP auth enabled: realm=%s, user=%s\n",
                     config.realm.c_str(), config.username.c_str());
        }

        xop::MediaSession* session = xop::MediaSession::CreateNew(config.suffix);
        session->AddSource(xop::channel_0, xop::H264Source::CreateNew(config.framerate));
        session->AddSource(xop::channel_1, xop::G711ASource::CreateNew());  // 音频 G711A (PCMA), 8kHz mono

        session->AddNotifyConnectedCallback([this](xop::MediaSessionId, std::string ip, uint16_t port) {
            uint32_t count = ++rtsp_client_count_;
            ump_info("RTSP client connected: %s:%u, total=%u\n", ip.c_str(), port, count);
        });

        session->AddNotifyDisconnectedCallback([this](xop::MediaSessionId, std::string ip, uint16_t port) {
            uint32_t count = --rtsp_client_count_;
            ump_info("RTSP client disconnected: %s:%u, total=%u\n", ip.c_str(), port, count);
        });

        rtsp_session_id_ = rtsp_server_->AddSession(session);
        if (rtsp_session_id_ == 0) {
            ump_error("Failed to add RTSP session\n");
            rtsp_server_.reset();
            return -1;
        }

        rtsp_started_.store(true, std::memory_order_release);

        ump_info("RTSP server started: rtsp://%s:%u/%s\n",
                 config.ip.c_str(), config.port, config.suffix.c_str());
        return 0;
    }

    int32_t VideoRtspPush::StopRtspServer()
    {
        std::lock_guard<std::mutex> lock(rtsp_mutex_);
        if (!rtsp_started_.load(std::memory_order_acquire)) {
            return 0;
        }

        if (rtsp_server_) {
            if (rtsp_session_id_ != 0) {
                rtsp_server_->RemoveSession(rtsp_session_id_);
                rtsp_session_id_ = 0;
            }
            rtsp_server_.reset();
        }

        if (rtsp_event_loop_) {
            rtsp_event_loop_->Quit();
        }

        rtsp_started_.store(false, std::memory_order_release);
        rtsp_client_count_.store(0, std::memory_order_release);
        ump_info("RTSP server stopped\n");
        return 0;
    }

    void VideoRtspPush::RtspEventLoopThread()
    {
        if (rtsp_event_loop_) {
            rtsp_event_loop_->Loop();
        }
        ump_info("RTSP event loop exited\n");
    }

    // ==================== 编码帧回调（仅入队） ====================
    void VideoRtspPush::OnFrame(
        video_common::EncodedImageBuffer *encoded_image,
        const video_common::CodecSpecificInfo *codec_specific_info)
    {
        if (!encoded_image || !codec_specific_info) {
            ump_error("Invalid parameters\n");
            return;
        }

        if (!pushStarted_.load(std::memory_order_acquire)) {
            return;
        }

        if (codec_specific_info->codecType != video_common::kVideoCodecH264) {
            ump_error("Unsupported codec type: %d\n", codec_specific_info->codecType);
            return;
        }

        // 入队（队列满时丢最旧帧，保证最新帧能入队）
        TH264Buffer *ptFrame = (TH264Buffer *)IntelliGetBuffer(
            &h264_buf_hndl_, IPC_INTELLI_EMPTY_BUFFER_FLAG, IPC_INTELLI_TIMEOUT_NONE);
        if (!ptFrame) {
            // 队列满 → 丢最旧帧再重试
            TH264Buffer *old = (TH264Buffer *)IntelliGetBuffer(
                &h264_buf_hndl_, IPC_INTELLI_FULL_BUFFER_FLAG, IPC_INTELLI_TIMEOUT_NONE);
            if (old) {
                if (old->h264_frame) old->h264_frame->RelRef();
                IntelliPutBuffer(&h264_buf_hndl_, old, IPC_INTELLI_EMPTY_BUFFER_FLAG);
            }
            ptFrame = (TH264Buffer *)IntelliGetBuffer(
                &h264_buf_hndl_, IPC_INTELLI_EMPTY_BUFFER_FLAG, IPC_INTELLI_TIMEOUT_NONE);
        }
        if (!ptFrame) {
            ump_error("RTSP OnFrame: queue full, drop frame\n");
            return;
        }

        // IDR 帧插入 SPS/PPS
        if (5 == (encoded_image->data()[4] & 0x1F)) {
            uint32_t h264_size = encoded_image->size();
            uint32_t sps_pps_size = codec_specific_info->codecSpecific.H264.sps_pps_len;
            uint8_t *h264_data = encoded_image->data();
            uint8_t *sps_pps_data = codec_specific_info->codecSpecific.H264.sps_pps;
            uint8_t *data = (uint8_t *)malloc(h264_size + sps_pps_size);
            if (sps_pps_data)
                memcpy(data, sps_pps_data, sps_pps_size);
            memcpy(data + sps_pps_size, h264_data, h264_size);
            video_common::EncodedImageBuffer *adapter =
                new video_common::EncodedImageBuffer(data, h264_size + sps_pps_size);
            free(data);
            adapter->AddRef();
            ptFrame->h264_frame = adapter;
        } else {
            encoded_image->AddRef();
            ptFrame->h264_frame = encoded_image;
        }
        IntelliPutBuffer(&h264_buf_hndl_, ptFrame, IPC_INTELLI_FULL_BUFFER_FLAG);
    }

    // ==================== 音频实现 ====================

    bool VideoRtspPush::AudioInit()
    {
        // 1. 创建 ring buffer
        audio_ring_buf_ = ringbuffer_create(AUDIO_RING_BUFFER_SIZE);
        if (!audio_ring_buf_) {
            ump_error("audio: ringbuffer_create failed\n");
            return false;
        }

        // 2. 初始化重采样器
        if (!audio_resampler_.Init(AUDIO_INPUT_CHANNELS, AUDIO_INPUT_RATE,
                                   AUDIO_OUTPUT_RATE, 3)) {
            ump_error("audio: resampler init failed\n");
            ringbuffer_destroy(audio_ring_buf_);
            audio_ring_buf_ = nullptr;
            return false;
        }

        // 3. audio_processor 延后到 AudioStart 里根据 audio_noise_level_ 动态创建

        ump_info("audio: init OK (in=%dHz ch=%d, out=%dHz ch=%d, ring=%u bytes)\n",
                 AUDIO_INPUT_RATE, AUDIO_INPUT_CHANNELS,
                 AUDIO_OUTPUT_RATE, AUDIO_OUTPUT_CHANNELS,
                 AUDIO_RING_BUFFER_SIZE);
        return true;
    }

    void VideoRtspPush::AudioUnInit()
    {
        if (audio_ring_buf_) {
            ringbuffer_destroy(audio_ring_buf_);
            audio_ring_buf_ = nullptr;
        }
        if (audio_processor_) {
            audio_processor_destroy(audio_processor_);
            audio_processor_ = nullptr;
        }
        audio_resampler_.Reset();
        ump_info("audio: uninit\n");
    }

    bool VideoRtspPush::AudioStart()
    {
        if (audio_running_.load(std::memory_order_acquire))
            return true;

        // 0. 根据降噪等级创建 audio_processor（0=关闭降噪，>=1 传给库：1=Low 2=Moderate 3=High 4=VeryHigh）
        if (audio_processor_) {
            audio_processor_destroy(audio_processor_);
            audio_processor_ = nullptr;
        }
        if (audio_noise_level_ != 0) {
            audio_processor_ = audio_processor_create(AUDIO_INPUT_RATE, audio_noise_level_);
            if (audio_processor_) {
                ump_info("audio: noise processor ready, level=[%d] (1=Low 2=Moderate 3=High 4=VeryHigh)\n",
                         audio_noise_level_);
            } else {
                ump_warn("audio: audio_processor_create(level=%d) failed, running without noise reduction\n",
                         audio_noise_level_);
            }
        } else {
            ump_info("audio: noise reduction disabled (audio_noise_level=0)\n");
        }

        // 1. 注册采集回调
        audio_recorder_.RegisterCallback(
            [this](const int16_t* data, size_t bytes,
                   int sample_rate, int channels) {
                AudioCaptureCallback(data, bytes, sample_rate, channels);
            });

        // 2. 初始化 ALSA 采集
        //    板卡 card 1 = snddmic (数字麦克风)，device 0
        //    单声道采集（AudioCaptureCallback 里 channels==1 直接 memcpy，不需要 downmix）
        if (!audio_recorder_.Init("plughw:1,0", AUDIO_INPUT_RATE, AUDIO_INPUT_CHANNELS,
                                   AUDIO_FRAME_MS, 64)) {
            ump_error("audio: AlsaRecorder init failed: %s\n",
                      audio_recorder_.GetLastError().c_str());
            return false;
        }

        // 3. 启动 ALSA 采集
        if (!audio_recorder_.Start()) {
            ump_error("audio: AlsaRecorder start failed: %s\n",
                      audio_recorder_.GetLastError().c_str());
            audio_recorder_.CloseDevice();
            return false;
        }

        // 4. 启动音频处理线程（重采样 → 编码 → 推流）
        audio_quit_.store(false, std::memory_order_release);
        audio_running_.store(true, std::memory_order_release);
        // audio_start_us_ 已在 StartPush 中初始化，和 video_start_us_ 共用同一起点

        std::lock_guard<std::mutex> guard(audio_mutex_);
        audio_thread_ = std::thread([this] {
            ump_info("audio: AudioPushProcess thread started\n");
            while (AudioPushProcess()) {
                IntelliUtilSleepMs(2);
            }
            ump_info("audio: AudioPushProcess thread exited\n");
        });

        ump_info("audio: started\n");
        return true;
    }

    void VideoRtspPush::AudioStop()
    {
        if (!audio_running_.load(std::memory_order_acquire))
            return;

        audio_quit_.store(true, std::memory_order_release);

        // 停 ALSA 采集
        audio_recorder_.Stop();

        // 等音频线程退出
        {
            std::lock_guard<std::mutex> guard(audio_mutex_);
            if (audio_thread_.joinable()) {
                audio_thread_.join();
            }
        }

        audio_running_.store(false, std::memory_order_release);

        // 清空 ring buffer
        if (audio_ring_buf_) {
            ringbuffer_reset(audio_ring_buf_);
        }

        ump_info("audio: stopped\n");
    }

    void VideoRtspPush::AudioCaptureCallback(const int16_t* data, size_t bytes,
                                              int sample_rate, int channels)
    {
        static uint32_t log_counter = 0;
        if (!audio_ring_buf_ || !audio_running_.load(std::memory_order_acquire))
            return;

        size_t total_samples = bytes / sizeof(int16_t);
        size_t mono_samples = total_samples / channels;

        // 栈上临时缓冲（避免每帧 malloc）
        int16_t mono_buf[4096];  // 足够装 20ms @ 16kHz mono = 320 samples
        if (mono_samples > sizeof(mono_buf) / sizeof(int16_t)) {
            // 超过临时缓冲，直接跳过（正常不会）
            if (log_counter++ % 100 == 0)
                ump_warn("audio: frame too large (%zu mono samples), skip\n", mono_samples);
            return;
        }

        if (channels == 1) {
            memcpy(mono_buf, data, mono_samples * sizeof(int16_t));
        } else {
            // 多声道平均
            for (size_t i = 0; i < mono_samples; ++i) {
                int32_t sum = 0;
                for (int c = 0; c < channels; ++c)
                    sum += data[i * channels + c];
                mono_buf[i] = (int16_t)(sum / channels);
            }
        }

        // 存 downmix 后的 mono PCM 到 ringbuffer
        unsigned int mono_bytes = mono_samples * sizeof(int16_t);
        unsigned int written = ringbuffer_put(audio_ring_buf_,
                                              (const char*)mono_buf, mono_bytes);
        if (log_counter++ % 100 == 0) {
            ump_info("audio: Capture OK, rate=%d, ch=%d->mono, written=%u, ring=%u\n",
                     sample_rate, channels, written, ringbuffer_len(audio_ring_buf_));
        }
    }

    bool VideoRtspPush::AudioPushProcess()
    {
        if (audio_quit_.load(std::memory_order_acquire))
            return false;

        const uint32_t frame_bytes = AUDIO_INPUT_RATE * AUDIO_FRAME_MS * 2 * AUDIO_INPUT_CHANNELS / 1000;

        // 从 ring buffer 取一帧
        if (!audio_ring_buf_) {
            static bool logged_once = false;
            if (!logged_once) {
                ump_error("audio: ringbuffer is null!\n");
                logged_once = true;
            }
            return true;
        }

        unsigned int ring_len = ringbuffer_len(audio_ring_buf_);
        if (ring_len < frame_bytes) {
            static uint32_t underrun_count = 0;
            // 每 500 次打一次（避免刷屏）
            if (underrun_count++ % 500 == 0 && ring_len > 0) {
                ump_warn("audio: need=%u, have=%u (underrun)\n", frame_bytes, ring_len);
            }
            return true;
        }

        // 直接在 ring buffer 上读（不拷贝）—— 但 ringbuffer_get 会拷贝到目标
        // 先读出来
        std::vector<int16_t> pcm_in(frame_bytes / sizeof(int16_t));
        unsigned int got = ringbuffer_get(audio_ring_buf_,
                                          (char*)pcm_in.data(), frame_bytes);
        if (got < frame_bytes) {
            // 不够一帧，放回去（简化：直接丢掉，等后续数据）
            return true;
        }

        // ====== 音频推流性能统计（每秒打印一次） ======
        static int64_t s_audio_last_log_us = 0;
        static uint32_t s_audio_frame_count = 0;
        static int64_t s_audio_total_us = 0;
        static int64_t s_audio_max_us = 0;

        auto a0 = std::chrono::steady_clock::now();
        int64_t audio_start_us = std::chrono::duration_cast<std::chrono::microseconds>(
            a0.time_since_epoch()).count();
        // ============================================

        // 0. 降噪处理（audio_processor 要求 10ms 帧 = sample_rate/100）
        //    20ms 数据拆成两个 10ms 分别过 APM NS
        //    AEC 已关闭，render_pcm 传 nullptr（ump 内部自动填零）
        std::vector<int16_t> denoised(pcm_in.size());
        if (audio_processor_) {
            const int proc_frame = AUDIO_INPUT_RATE / 100;  // 160 samples @ 16kHz
            // 前 10ms
            audio_processor_process(audio_processor_,
                                    pcm_in.data(),
                                    nullptr,                        // render_pcm 自动填零
                                    denoised.data(),
                                    0);                             // delay_ms (AEC 关闭，暂传 0)
            // 后 10ms
            audio_processor_process(audio_processor_,
                                    pcm_in.data() + proc_frame,
                                    nullptr,
                                    denoised.data() + proc_frame,
                                    0);
        } else {
            // audio_processor_ 未创建（level=0 关闭降噪 或 create 失败），直接透传
            denoised = pcm_in;
        }

        // 1. 重采样 16kHz → 8kHz
        std::vector<int16_t> pcm_out;
        size_t input_samples = frame_bytes / sizeof(int16_t);
        size_t output_samples = audio_resampler_.Process(
            denoised.data(), input_samples, pcm_out);
        if (output_samples == 0) {
            ump_warn("audio: resample produced 0 samples\n");
            return true;
        }

        // 2. G711 ALAW 编码
        std::vector<uint8_t> g711_data = g711_encoder_.Encode(
            pcm_out.data(), output_samples);
        if (g711_data.empty()) {
            ump_warn("audio: g711 encode empty\n");
            return true;
        }

        // 3. 推到 RTSP channel_1
        {
            std::lock_guard<std::mutex> rtsp_lock(rtsp_mutex_);
            if (rtsp_server_ && rtsp_session_id_ != 0 &&
                rtsp_client_count_.load(std::memory_order_acquire) > 0) {

                xop::AVFrame frame(g711_data.size());
                frame.type = xop::AUDIO_FRAME;
                memcpy(frame.buffer.get(), g711_data.data(), g711_data.size());
                // 获取当前时间（微秒）
                int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();

                // 原始时间戳生成（无校准）
                frame.timestamp = static_cast<uint32_t>((now_us - audio_start_us_) * 8 / 1000);  // 8kHz 时钟


                rtsp_server_->PushFrame(rtsp_session_id_, xop::channel_1, frame);
            }
        }

        // ====== 音频统计收尾 ======
        auto a1 = std::chrono::steady_clock::now();
        int64_t audio_us = std::chrono::duration_cast<std::chrono::microseconds>(a1 - a0).count();

        s_audio_frame_count++;
        s_audio_total_us += audio_us;
        if (audio_us > s_audio_max_us) s_audio_max_us = audio_us;

        if (s_audio_last_log_us == 0) s_audio_last_log_us = audio_start_us;
        if (audio_start_us - s_audio_last_log_us >= 1000000LL) {
            double fps = s_audio_frame_count * 1000000.0 / (audio_start_us - s_audio_last_log_us);
            double avg_ms = (s_audio_total_us / (double)s_audio_frame_count) / 1000.0;
            double max_ms = s_audio_max_us / 1000.0;
            ump_info("[PERF] push(audio): fps=%.1f, avg=%.2fms, max=%.2fms, frames=%u\n",
                     fps, avg_ms, max_ms, s_audio_frame_count);
            s_audio_frame_count = 0;
            s_audio_total_us = 0;
            s_audio_max_us = 0;
            s_audio_last_log_us = audio_start_us;
        }
        // ============================

        return true;
    }

} // namespace ecovacs
