#ifndef API_RTSP_SERVER_H_
#define API_RTSP_SERVER_H_

// ============================================================================
// api_rtsp_server.h
//
//   - 裁剪为单通道（本项目仅一路 rtsp://<ip>:<port>/<stream_name>）
//   - 端口 / 流名 / 音视频开关 / 鉴权均可配置（默认 554 / live）
//   - std::thread + watch variable 替代全局 gStopEventLoop，支持多实例
// ============================================================================

#include <cstdint>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>

#include "BasicUsageEnvironment.hh"
#include "liveMedia.hh"
#include "RTSPServer.hh"
#include "ServerMediaSession.hh"
#include "api_live_server_media_session.h"

namespace apirtsp {

struct ApiRtspServerConfig {
    uint16_t    port = 554;
    std::string stream_name = "live";
    bool        enable_video = true;
    bool        enable_audio = true;
    uint32_t    audio_sample_rate = 8000;  // G711A
    uint8_t     audio_channels = 1;
    std::string username;                  // 非空且 password 非空时启用鉴权
    std::string password;
};

class ApiRtspServer {
public:
    ApiRtspServer();
    ~ApiRtspServer();

    ApiRtspServer(const ApiRtspServer&) = delete;
    ApiRtspServer& operator=(const ApiRtspServer&) = delete;

    // 创建 live555 运行环境、RTSPServer、媒体会话；成功后启动事件循环线程
    bool Start(const ApiRtspServerConfig& config);
    void Stop();
    bool IsStarted() const { return started_.load(std::memory_order_acquire); }

    // 视频帧：Annex-B 整帧（关键帧含 SPS/PPS/IDR），pts 为微秒绝对时间
    void SendNalu(const uint8_t* p_buf, uint32_t n_len,
                  uint64_t n_pts_us, bool b_i_frame);

    // 音频帧：G.711A 裸数据
    void SendAudio(const uint8_t* p_buf, uint32_t n_len, uint64_t n_pts_us);

private:
    void EventLoopFunc();

private:
    ApiRtspServerConfig config_;

    TaskScheduler*               scheduler_  = nullptr;
    UsageEnvironment*            env_        = nullptr;
    RTSPServer*                  rtsp_server_ = nullptr;
    ServerMediaSession*          sms_        = nullptr;
    UserAuthenticationDatabase*  auth_db_    = nullptr;

    ApiLiveServerMediaSession* video_session_ = nullptr;
    ApiLiveServerMediaSession* audio_session_ = nullptr;

    std::thread event_thread_;
    // 新版 live555 中 EventLoopWatchVariable 为 std::atomic<char>（旧版为 volatile char）
    EventLoopWatchVariable watch_var_{0};   // doEventLoop 退出标志：非 0 退出
    std::atomic<bool> started_{false};
};

} // namespace apirtsp

#endif // API_RTSP_SERVER_H_
