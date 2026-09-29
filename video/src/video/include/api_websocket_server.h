// api_websocket_server.h
#ifndef API_WEBSOCKET_SERVER_H_
#define API_WEBSOCKET_SERVER_H_

#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <libwebsockets.h>
#include "log.h"

namespace ecovacs
{
    class WebSocketServer
    {
    private:
        struct lws_context *context_;
        std::thread server_thread_;
        std::atomic<bool> running_;

        std::vector<struct lws *> clients_;
        mutable std::mutex clients_mutex_;

        struct PerSessionData
        {
            std::queue<std::vector<uint8_t>> frame_queue;
            bool want_write;
            uint64_t last_activity;
            uint32_t frames_sent;
            uint32_t frames_dropped;

            PerSessionData();
            ~PerSessionData();
        };

        static constexpr const char *kProtocolName = "rv1106-video";
        static constexpr int kPort = 9000;
        static constexpr int kTimerIntervalMs = 16;
        static constexpr size_t kMaxQueueSize = 10;              // 增大队列
        static constexpr size_t kMaxJpegSize = 10 * 1024 * 1024; // 10MB
        static constexpr int kTimeoutSeconds = 300;              // 增大超时时间到300秒
        static constexpr int kLwsTimeoutMs = 10;                 // 减小服务超时

        static lws_sorted_usec_list_t sul_timer_;
        static WebSocketServer *s_instance;
        static const struct lws_protocols protocols_[2];

        static void timer_cb(lws_sorted_usec_list_t *sul);
        static int callback(struct lws *wsi, enum lws_callback_reasons reason,
                            void *user, void *in, size_t len);

        void ServerThread();
        bool ValidateJpeg(const std::vector<uint8_t> &data) const;

    public:
        WebSocketServer();
        ~WebSocketServer();

        WebSocketServer(const WebSocketServer &) = delete;
        WebSocketServer &operator=(const WebSocketServer &) = delete;

        bool Start();
        void Stop();
        void QueueFrame(const std::vector<uint8_t> &frame);
        size_t GetClientCount() const;
        bool HasClients() const;
        void GetStats(size_t &client_count, size_t &total_queued,
                      size_t &total_sent, size_t &total_dropped) const;
    };
}

#endif