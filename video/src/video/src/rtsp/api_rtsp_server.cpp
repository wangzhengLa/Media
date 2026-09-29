// ============================================================================
// api_rtsp_server.cpp
// ============================================================================

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <unistd.h>

#include <vector>
#include <string>

#include "rtsp/api_rtsp_server.h"
#include "GroupsockHelper.hh"

namespace apirtsp {

// 查找本机首个可用 IPv4 地址（仅用于打印 URL）
static bool GetLocalIP(char* out_ip, uint32_t n_len) {
    static const char* kNetTypes[] = {"eth", "wlan", "usb"};
    for (size_t t = 0; t < sizeof(kNetTypes) / sizeof(kNetTypes[0]); ++t) {
        for (char c = '0'; c <= '9'; ++c) {
            std::string str_device = std::string(kNetTypes[t]) + c;

            int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
            if (fd < 0) {
                continue;
            }

            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            strncpy(ifr.ifr_name, str_device.c_str(), sizeof(ifr.ifr_name) - 1);
            int ret = ::ioctl(fd, SIOCGIFADDR, &ifr);
            ::close(fd);
            if (ret < 0) {
                continue;
            }

            char* p_ip = inet_ntoa(((struct sockaddr_in*)&(ifr.ifr_addr))->sin_addr);
            if (p_ip) {
                strncpy(out_ip, p_ip, n_len - 1);
                out_ip[n_len - 1] = '\0';
                return true;
            }
        }
    }
    return false;
}

ApiRtspServer::ApiRtspServer() = default;

ApiRtspServer::~ApiRtspServer() {
    Stop();
}

bool ApiRtspServer::Start(const ApiRtspServerConfig& config) {
    if (started_.load(std::memory_order_acquire)) {
        API_RTSP_LOGW("RTSP server already started\n");
        return true;
    }

    config_ = config;
    watch_var_ = 0;

    // ---- 1. live555 运行环境 ----
    scheduler_ = BasicTaskScheduler::createNew();
    if (!scheduler_) {
        API_RTSP_LOGE("Failed to create TaskScheduler\n");
        return false;
    }
    env_ = BasicUsageEnvironment::createNew(*scheduler_);
    if (!env_) {
        API_RTSP_LOGE("Failed to create UsageEnvironment\n");
        delete scheduler_;
        scheduler_ = nullptr;
        return false;
    }

    // ---- 2. 可选鉴权 ----
    if (!config_.username.empty() && !config_.password.empty()) {
        auth_db_ = new UserAuthenticationDatabase;
        auth_db_->addUserRecord(config_.username.c_str(), config_.password.c_str());
    }

    // ---- 3. RTSPServer ----
    unsigned reclamation_seconds = 65;
    if (const char* v = getenv("RTSP_RECLAMATION_SECONDS")) {
        reclamation_seconds = (unsigned)strtoul(v, nullptr, 10);
    }

    rtsp_server_ = RTSPServer::createNew(*env_, config_.port, auth_db_,
                                         reclamation_seconds);
    if (!rtsp_server_) {
        API_RTSP_LOGE("Failed to create RTSPServer on port %u: %s\n",
                     (unsigned)config_.port, env_->getResultMsg());
        if (auth_db_) {
            delete auth_db_;
            auth_db_ = nullptr;
        }
        env_->reclaim();
        env_ = nullptr;
        scheduler_ = nullptr;
        return false;
    }

    // ---- 4. ServerMediaSession + 子会话 ----
    sms_ = ServerMediaSession::createNew(
        *env_,
        config_.stream_name.c_str(),
        config_.stream_name.c_str(),
        "H.264 Live Stream by apirtsp",
        False /*isSSM*/,
        NULL /*miscSDPLines*/);
    if (!sms_) {
        API_RTSP_LOGE("Failed to create ServerMediaSession\n");
        RTSPServer::close(rtsp_server_);
        rtsp_server_ = nullptr;
        if (auth_db_) {
            delete auth_db_;
            auth_db_ = nullptr;
        }
        env_->reclaim();
        env_ = nullptr;
        scheduler_ = nullptr;
        return false;
    }

    if (config_.enable_video) {
        video_session_ = ApiLiveServerMediaSession::createNewVideo(
            *env_, true /*reuseFirstSource*/, API_RTSP_VIDEO_MAX_FRAME_SIZE);
        sms_->addSubsession(video_session_);
    }

    if (config_.enable_audio) {
        audio_session_ = ApiLiveServerMediaSession::createNewAudio(
            *env_, true /*reuseFirstSource*/,
            config_.audio_sample_rate, config_.audio_channels);
        sms_->addSubsession(audio_session_);
    }

    rtsp_server_->addServerMediaSession(sms_);

    // ---- 5. 启动事件循环线程 ----
    started_.store(true, std::memory_order_release);
    event_thread_ = std::thread(&ApiRtspServer::EventLoopFunc, this);

    char sz_ip[64] = {0};
    if (GetLocalIP(sz_ip, sizeof(sz_ip))) {
        API_RTSP_LOGI("RTSP server started: rtsp://%s:%u/%s\n",
                     sz_ip, (unsigned)config_.port, config_.stream_name.c_str());
    } else {
        API_RTSP_LOGI("RTSP server started: rtsp://0.0.0.0:%u/%s\n",
                     (unsigned)config_.port, config_.stream_name.c_str());
    }

    return true;
}

void ApiRtspServer::Stop() {
    if (!started_.exchange(false)) {
        return;
    }

    // 通知事件循环退出（live555 select 最迟约 1s 被定时事件唤醒）
    watch_var_ = 1;
    if (event_thread_.joinable()) {
        event_thread_.join();
    }

    if (rtsp_server_ && sms_) {
        // removeServerMediaSession 内部会 Medium::close(sms)，子会话随之释放
        rtsp_server_->removeServerMediaSession(sms_);
    }
    sms_ = nullptr;
    video_session_ = nullptr;
    audio_session_ = nullptr;

    if (rtsp_server_) {
        RTSPServer::close(rtsp_server_);
        rtsp_server_ = nullptr;
    }
    if (auth_db_) {
        delete auth_db_;
        auth_db_ = nullptr;
    }
    if (env_) {
        // BasicUsageEnvironment::reclaim() 同时释放 scheduler
        env_->reclaim();
        env_ = nullptr;
        scheduler_ = nullptr;
    }

    API_RTSP_LOGI("RTSP server stopped\n");
}

void ApiRtspServer::EventLoopFunc() {
    prctl(PR_SET_NAME, "apirtsp_loop");
    env_->taskScheduler().doEventLoop(&watch_var_);
}

void ApiRtspServer::SendNalu(const uint8_t* p_buf, uint32_t n_len,
                            uint64_t n_pts_us, bool b_i_frame) {
    if (!p_buf || n_len == 0) {
        return;
    }
    if (!started_.load(std::memory_order_acquire) || !video_session_) {
        return;
    }
    video_session_->SendNalu(0, p_buf, n_len, n_pts_us, b_i_frame);
}

void ApiRtspServer::SendAudio(const uint8_t* p_buf, uint32_t n_len,
                             uint64_t n_pts_us) {
    if (!p_buf || n_len == 0) {
        return;
    }
    if (!started_.load(std::memory_order_acquire) || !audio_session_) {
        return;
    }
    audio_session_->SendAudio(0, p_buf, n_len, n_pts_us);
}

} // namespace apirtsp
