#include "api_video_libpeer_push.h"
#include <log.h>
#include "osp.h"
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <chrono>

namespace ecovacs
{
    // ==================== 构造 / 析构 / 创建 ====================

    VideoLibpeerPush::VideoLibpeerPush() = default;
    VideoLibpeerPush::~VideoLibpeerPush() = default;

    std::unique_ptr<VideoLibpeerPush> VideoLibpeerPush::Create()
    {
        auto obj = std::make_unique<VideoLibpeerPush>();
        if (0 != obj->Init()) {
            ump_error("Failed to create VideoLibpeerPush\n");
            return nullptr;
        }
        return obj;
    }

    // ==================== 初始化 / 反初始化 ====================

    int32_t VideoLibpeerPush::Init()
    {
        ump_info("VideoLibpeerPush Init\n");

        // 创建帧队列
        if (IPC_INTELLI_SOK != IntelliBufCreate(&h264_buf_hndl_, MAX_H264_QUEUE_SIZE)) {
            ump_error("h264_buf_hndl_ create failed\n");
            return -1;
        }
        for (int i = 0; i < MAX_H264_QUEUE_SIZE; i++) {
            if (IPC_INTELLI_SOK != IntelliPutBuffer(&h264_buf_hndl_, &h264_buf_[i],
                                                    IPC_INTELLI_EMPTY_BUFFER_FLAG)) {
                ump_error("IntelliPutBuffer msg_buf[%d] failed\n", i);
                IntelliBufDelete(&h264_buf_hndl_);
                return -1;
            }
        }

        // libpeer 全局初始化（整个程序只调一次）
        peer_init();

        return 0;
    }

    int32_t VideoLibpeerPush::UnInit()
    {
        ump_info("VideoLibpeerPush UnInit\n");

        // 清理队列中残留帧
        {
            std::lock_guard<std::mutex> guard(api_lock_);
            TH264Buffer *ptFrame = nullptr;
            ptFrame = (TH264Buffer *)IntelliGetBuffer(&h264_buf_hndl_,
                                                      IPC_INTELLI_FULL_BUFFER_FLAG,
                                                      IPC_INTELLI_TIMEOUT_NONE);
            while (ptFrame) {
                if (ptFrame->h264_frame) ptFrame->h264_frame->RelRef();
                IntelliPutBuffer(&h264_buf_hndl_, ptFrame, IPC_INTELLI_EMPTY_BUFFER_FLAG);
                ptFrame = (TH264Buffer *)IntelliGetBuffer(&h264_buf_hndl_,
                                                          IPC_INTELLI_FULL_BUFFER_FLAG,
                                                          IPC_INTELLI_TIMEOUT_NONE);
            }
        }
        IntelliBufDelete(&h264_buf_hndl_);

        // libpeer 全局反初始化（程序退出时调）
        peer_deinit();

        return 0;
    }

    // ==================== 推流启停 ====================

    int32_t VideoLibpeerPush::StartPush()
    {
        bool expected = false;
        if (!push_started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            ump_info("libpeer push already running\n");
            return 0;
        }

        quit_.store(false, std::memory_order_release);
        ump_info("VideoLibpeerPush StartPush, signaling=%s\n", config_.signaling_url.c_str());

        // 1. 创建 PeerConnection
        PeerConfiguration cfg = {};
        cfg.ice_servers[0].urls       = const_cast<char*>(config_.turn_url.c_str());
        cfg.ice_servers[0].username   = const_cast<char*>(config_.turn_username.c_str());
        cfg.ice_servers[0].credential = const_cast<char*>(config_.turn_password.c_str());
        cfg.video_codec = CODEC_H264;
        cfg.audio_codec = CODEC_NONE;
        cfg.datachannel = DATA_CHANNEL_NONE;
        cfg.user_data   = &signaling_client_;

        pc_ = peer_connection_create(&cfg);
        if (!pc_) {
            ump_error("peer_connection_create failed\n");
            push_started_.store(false);
            return -1;
        }

        // 2. 注册 ICE state 回调
        peer_connection_oniceconnectionstatechange(
        pc_, [](PeerConnectionState s, void* ud) {
            ump_info("peer state -> %s\n", peer_connection_state_to_string(s));
            auto* client = static_cast<SignalingClient*>(ud);
            if (client && client->video_user_data) {
                auto* self = static_cast<VideoLibpeerPush*>(client->video_user_data);
                self->peer_state_.store(s);
            }
        });

        // 3. 连接信令服务器
        memset(&signaling_client_, 0, sizeof(signaling_client_));
        signaling_client_.video_user_data = this;
        if (signaling_client_connect(&signaling_client_, config_.signaling_url.c_str(),
                                     pc_, config_.is_offerer) != 0) {
            ump_error("signaling_client_connect failed\n");
            peer_connection_destroy(pc_); pc_ = nullptr;
            push_started_.store(false);
            return -1;
        }
        ump_info("signaling connected as caller\n");

        // 4. 启动 peer_loop_thread（WebRTC 主循环 + 视频发送）
        peer_quit_.store(false);
        peer_state_.store(PEER_CONNECTION_NEW);
        peer_loop_thread_ = std::thread([this] {
            uint64_t next_frame_us = 0;
            int send_cnt = 0;
            int drop_cnt = 0;
            bool was_connected = false;
            bool need_keyframe = true;
            const int FRAME_INTERVAL_US = 1000000 / config_.video_fps;

            ump_info("peer_loop started, target fps=%d, interval_us=%d\n",
                     config_.video_fps, FRAME_INTERVAL_US);

            while (!peer_quit_.load()) {
                auto loop_start_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();

                // libpeer 主循环（处理 ICE/DTLS/SCTP 等内部状态机）
                peer_connection_loop(pc_);

                bool connected = (peer_state_.load() == PEER_CONNECTION_CONNECTED);

                // 刚连上：清空队列积压帧，等待关键帧
                if (connected && !was_connected) {
                    was_connected = true;
                    send_cnt = 0;
                    drop_cnt = 0;
                    need_keyframe = true;
                    TH264Buffer *old;
                    while ((old = DequeueLatestFrame()) != nullptr) {
                        if (old->h264_frame) old->h264_frame->RelRef();
                        IntelliPutBuffer(&h264_buf_hndl_, old, IPC_INTELLI_EMPTY_BUFFER_FLAG);
                    }
                    ump_info("connected, queue drained, waiting for keyframe\n");
                }
                if (!connected) {
                    was_connected = false;
                }

                // 连接就绪 + 到发送时间 → 取最新帧发送
                if (connected) {
                    auto now_us = loop_start_us;
                    if (next_frame_us == 0) next_frame_us = now_us;
                    if (now_us >= next_frame_us) {
                        next_frame_us += FRAME_INTERVAL_US;
                        // 严重滞后则跳时
                        if (now_us > next_frame_us + 200000)
                            next_frame_us = now_us + FRAME_INTERVAL_US;

                        TH264Buffer *ptFrame = DequeueLatestFrame();
                        if (ptFrame) {
                            uint8_t *frame_data = ptFrame->h264_frame->data();
                            uint32_t frame_size = ptFrame->h264_frame->size();

                            // 首帧必须是关键帧
                            if (need_keyframe) {
                                bool is_keyframe = (frame_size >= 5 && frame_data[4] == 0x67);
                                if (!is_keyframe) {
                                    ptFrame->h264_frame->RelRef();
                                    IntelliPutBuffer(&h264_buf_hndl_, ptFrame, IPC_INTELLI_EMPTY_BUFFER_FLAG);
                                    ptFrame = nullptr;
                                } else {
                                    need_keyframe = false;
                                    ump_info("keyframe found, start sending\n");
                                }
                            }

                            if (ptFrame) {
                                int ret = peer_connection_send_video(pc_, frame_data, frame_size);
                                if (++send_cnt % 50 == 0)
                                    ump_info("[libpeer] sent #%d size=%u ret=%d dropped=%d\n",
                                             send_cnt, frame_size, ret, drop_cnt);
                                ptFrame->h264_frame->RelRef();
                                IntelliPutBuffer(&h264_buf_hndl_, ptFrame, IPC_INTELLI_EMPTY_BUFFER_FLAG);
                            }
                        }
                    }
                }

                usleep(1000);
            }
            ump_info("peer_loop_thread exited\n");
        });

        // 5. 启动信令轮询线程
        signaling_loop_thread_ = std::thread([this] {
            while (!peer_quit_.load()) {
                signaling_client_loop(&signaling_client_);
                usleep(1000);
            }
            ump_info("signaling_loop_thread exited\n");
        });

        ump_info("VideoLibpeerPush StartPush done\n");
        return 0;
    }

    int32_t VideoLibpeerPush::StopPush()
    {
        bool expected = true;
        if (!push_started_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
            ump_info("libpeer push not running\n");
            return 0;
        }

        // 先停 OnFrame 入队（push_started_=false 后快速 return）

        // 停两个线程
        peer_quit_.store(true);
        if (peer_loop_thread_.joinable()) {
            peer_loop_thread_.join();
            ump_info("peer_loop_thread joined\n");
        }
        if (signaling_loop_thread_.joinable()) {
            signaling_loop_thread_.join();
            ump_info("signaling_loop_thread joined\n");
        }

        // 断信令 + 销毁 PeerConnection
        signaling_client_disconnect(&signaling_client_);
        if (pc_) {
            peer_connection_destroy(pc_);
            pc_ = nullptr;
        }
        peer_state_.store(PEER_CONNECTION_NEW);

        ump_info("VideoLibpeerPush StopPush done\n");
        return 0;
    }

    // ==================== 队列操作 ====================

    void VideoLibpeerPush::OnFrame(
        video_common::EncodedImageBuffer *encoded_image,
        const video_common::CodecSpecificInfo *codec_specific_info)
    {
        if (!encoded_image || !codec_specific_info) return;
        if (!push_started_.load(std::memory_order_acquire)) return;
        if (codec_specific_info->codecType != video_common::kVideoCodecH264) return;

        EnqueueFrame(encoded_image, codec_specific_info);
    }

    void VideoLibpeerPush::EnqueueFrame(
        video_common::EncodedImageBuffer *encoded_image,
        const video_common::CodecSpecificInfo *codec_specific_info)
    {
        TH264Buffer *ptFrame = (TH264Buffer *)IntelliGetBuffer(
            &h264_buf_hndl_, IPC_INTELLI_EMPTY_BUFFER_FLAG, IPC_INTELLI_TIMEOUT_NONE);

        if (!ptFrame) {
            // 队列满了 → 丢最旧帧再重试
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
            ump_error("EnqueueFrame: queue full, drop frame\n");
            return;
        }

        // IDR 帧前面拼接 SPS/PPS
        if ((encoded_image->data()[4] & 0x1F) == 5 &&
            codec_specific_info->codecSpecific.H264.sps_pps_len > 0)
        {
            uint32_t h264_size = encoded_image->size();
            uint32_t sps_pps_size = codec_specific_info->codecSpecific.H264.sps_pps_len;
            uint8_t *h264_data = encoded_image->data();
            uint8_t *sps_pps_data = codec_specific_info->codecSpecific.H264.sps_pps;
            uint8_t *data = (uint8_t *)malloc(h264_size + sps_pps_size);
            if (sps_pps_data) memcpy(data, sps_pps_data, sps_pps_size);
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

    VideoLibpeerPush::TH264Buffer* VideoLibpeerPush::DequeueLatestFrame()
    {
        TH264Buffer *ptFrame = (TH264Buffer *)IntelliGetBuffer(
            &h264_buf_hndl_, IPC_INTELLI_FULL_BUFFER_FLAG, IPC_INTELLI_TIMEOUT_NONE);
        if (!ptFrame) return nullptr;

        // drain 所有积压帧，只保留最后一帧（丢积压）
        TH264Buffer *next;
        while ((next = (TH264Buffer *)IntelliGetBuffer(
            &h264_buf_hndl_, IPC_INTELLI_FULL_BUFFER_FLAG, IPC_INTELLI_TIMEOUT_NONE)) != nullptr)
        {
            if (ptFrame->h264_frame) ptFrame->h264_frame->RelRef();
            IntelliPutBuffer(&h264_buf_hndl_, ptFrame, IPC_INTELLI_EMPTY_BUFFER_FLAG);
            ptFrame = next;
        }
        return ptFrame;
    }

} // namespace ecovacs
