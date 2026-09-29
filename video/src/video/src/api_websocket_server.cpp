// api_websocket_server.cpp
#include "api_websocket_server.h"
#include <algorithm>

namespace ecovacs
{
    // 静态成员
    WebSocketServer *WebSocketServer::s_instance = nullptr;
    lws_sorted_usec_list_t WebSocketServer::sul_timer_;
    const struct lws_protocols WebSocketServer::protocols_[] = {
        {
            kProtocolName,          // name
            callback,               // callback
            sizeof(PerSessionData), // per_session_data_size
            1024 * 1024,            // rx_buffer_size
            0,                      // id
            nullptr                 // user
        },
        {nullptr, nullptr, 0, 0, 0, nullptr}};

    // PerSessionData
    WebSocketServer::PerSessionData::PerSessionData()
        : want_write(false), last_activity(0), frames_sent(0), frames_dropped(0)
    {
    }

    WebSocketServer::PerSessionData::~PerSessionData()
    {
        while (!frame_queue.empty())
            frame_queue.pop();
    }

    WebSocketServer::WebSocketServer()
        : context_(nullptr), running_(false)
    {
        s_instance = nullptr;
        memset(&sul_timer_, 0, sizeof(sul_timer_));
    }

    WebSocketServer::~WebSocketServer()
    {
        Stop();
    }

    void WebSocketServer::timer_cb(lws_sorted_usec_list_t *sul)
    {
        if (!s_instance || !s_instance->running_)
            return;

        uint64_t now = lws_now_usecs() / 1000;

        {
            std::lock_guard<std::mutex> lock(s_instance->clients_mutex_);

            for (size_t i = 0; i < s_instance->clients_.size(); i++)
            {
                auto client = s_instance->clients_[i];

                if (!client)
                    continue;

                PerSessionData *sess = (PerSessionData *)lws_wsi_user(client);

                if (!sess)
                    continue;

                // 触发写通知
                if (!sess->frame_queue.empty() && !lws_send_pipe_choked(client))
                    lws_callback_on_writable(client);
            }
        }

        // 重新调度定时器
        if (s_instance && s_instance->running_ && s_instance->context_)
        {
            lws_sul_schedule(s_instance->context_, 0, &sul_timer_, timer_cb,
                             kTimerIntervalMs * 1000);
        }
    }

    int WebSocketServer::callback(struct lws *wsi, enum lws_callback_reasons reason,
                                  void *user, void *in, size_t len)
    {
        PerSessionData *session = (PerSessionData *)user;

        switch (reason)
        {
        case LWS_CALLBACK_PROTOCOL_INIT:
            ump_info("WebSocket protocol initialized\n");
            break;

        case LWS_CALLBACK_ESTABLISHED:
        {
            ump_info("WebSocket client connected\n");

            lws_set_timeout(wsi, PENDING_TIMEOUT_HTTP_KEEPALIVE_IDLE, kTimeoutSeconds);

            if (session)
            {
                //  placement new
                new (session) PerSessionData();
                session->last_activity = lws_now_usecs() / 1000;
            }

            if (s_instance)
            {
                std::lock_guard<std::mutex> lock(s_instance->clients_mutex_);
                s_instance->clients_.push_back(wsi);
                ump_info("Total clients: %zu\n", s_instance->clients_.size());

                // 如果有客户端且定时器未启动，启动定时器
                if (s_instance->clients_.size() == 1 && s_instance->context_)
                {
                    ump_info("Starting timer\n");
                    lws_sul_schedule(s_instance->context_, 0, &sul_timer_, timer_cb,
                                     kTimerIntervalMs * 1000);
                }
            }
            break;
        }

        case LWS_CALLBACK_CLOSED:
        {
            ump_info("WebSocket client disconnected\n");

            if (s_instance)
            {
                std::lock_guard<std::mutex> lock(s_instance->clients_mutex_);
                auto it = std::find(s_instance->clients_.begin(),
                                    s_instance->clients_.end(), wsi);
                if (it != s_instance->clients_.end())
                    s_instance->clients_.erase(it);
                ump_info("Remaining clients: %zu\n", s_instance->clients_.size());
            }

            if (session)
                session->~PerSessionData();
            break;
        }

        case LWS_CALLBACK_SERVER_WRITEABLE:
        {
            if (!session || session->frame_queue.empty())
                break;

            session->last_activity = lws_now_usecs() / 1000;
            auto &frame = session->frame_queue.front();

            if (frame.size() > kMaxJpegSize)
            {
                ump_error("Frame too large: %zu bytes\n", frame.size());
                session->frame_queue.pop();
                session->frames_dropped++;
                break;
            }

            // 分配发送缓冲区
            unsigned char *buf = (unsigned char *)malloc(LWS_PRE + frame.size());
            if (!buf)
            {
                ump_error("Failed to allocate buffer\n");
                break;
            }

            memcpy(buf + LWS_PRE, frame.data(), frame.size());

            int n = lws_write(wsi, buf + LWS_PRE, frame.size(), LWS_WRITE_BINARY);
            free(buf);

            if (n >= (int)frame.size())
            {
                session->frame_queue.pop();
                session->frames_sent++;

                if (session->frames_sent % 100 == 0)
                {
                    ump_info("Client sent %u frames, dropped: %u\n",
                             session->frames_sent, session->frames_dropped);
                }
            }
            else if (n < 0)
            {
                ump_error("lws_write failed: %d\n", n);
            }

            // 如果还有数据，继续触发写事件
            if (!session->frame_queue.empty())
                lws_callback_on_writable(wsi);
            break;
        }

        case LWS_CALLBACK_RECEIVE:
        {
            if (session)
                session->last_activity = lws_now_usecs() / 1000;

            if (len > 0 && in)
                ump_info("Received from client: %.*s\n", (int)len, (char *)in);
            break;
        }

        case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE:
            ump_info("Client initiated close\n");
            break;

        default:
            break;
        }
        return 0;
    }

    void WebSocketServer::ServerThread()
    {
        ump_info("WebSocket server thread started\n");

        s_instance = this;

        struct lws_context_creation_info info;
        memset(&info, 0, sizeof(info));

        info.port = kPort;
        info.protocols = protocols_;
        info.gid = -1;
        info.uid = -1;
        info.options = 0;
        info.timeout_secs = kTimeoutSeconds;

        context_ = lws_create_context(&info);
        if (!context_)
        {
            ump_error("lws_create_context failed\n");
            s_instance = nullptr;
            return;
        }

        ump_info("WebSocket server listening on port %d, protocol: %s\n",
                 kPort, kProtocolName);

        while (running_)
        {
            lws_service(context_, kLwsTimeoutMs);
        }

        lws_context_destroy(context_);
        context_ = nullptr;
        ump_info("WebSocket server thread stopped\n");

        s_instance = nullptr;
    }

    bool WebSocketServer::ValidateJpeg(const std::vector<uint8_t> &data) const
    {
        if (data.empty() || data.size() > kMaxJpegSize)
            return false;

        // 检查 JPEG SOI (0xFFD8)
        if (data.size() < 2 || data[0] != 0xFF || data[1] != 0xD8)
            return false;

        // 检查 JPEG EOI (0xFFD9) - 在文件末尾
        if (data.size() < 2 || data[data.size() - 2] != 0xFF || data[data.size() - 1] != 0xD9)
            return false;

        return true;
    }

    bool WebSocketServer::Start()
    {
        if (running_)
        {
            ump_warn("Server already running\n");
            return true;
        }

        running_ = true;
        server_thread_ = std::thread(&WebSocketServer::ServerThread, this);
        return true;
    }

    void WebSocketServer::Stop()
    {
        if (!running_)
            return;

        ump_info("Stopping WebSocket server...\n");
        running_ = false;

        if (server_thread_.joinable())
            server_thread_.join();

        ump_info("WebSocket server stopped\n");
    }

    void WebSocketServer::QueueFrame(const std::vector<uint8_t> &frame)
    {
        if (!running_ || !ValidateJpeg(frame))
            return;

        std::lock_guard<std::mutex> lock(clients_mutex_);

        if (clients_.empty())
            return;

        for (size_t i = 0; i < clients_.size(); i++)
        {
            auto client = clients_[i];

            if (!client)
                continue;

            PerSessionData *session = (PerSessionData *)lws_wsi_user(client);

            if (!session)
                continue;

            if (session->frame_queue.size() < kMaxQueueSize)
            {
                session->frame_queue.push(frame);
            }
            else
            {
                session->frames_dropped++;
            }
        }
    }

    size_t WebSocketServer::GetClientCount() const
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        return clients_.size();
    }

    bool WebSocketServer::HasClients() const
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        return !clients_.empty();
    }

    void WebSocketServer::GetStats(size_t &client_count, size_t &total_queued,
                                   size_t &total_sent, size_t &total_dropped) const
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);

        client_count = clients_.size();
        total_queued = 0;
        total_sent = 0;
        total_dropped = 0;

        for (auto client : clients_)
        {
            if (!client)
                continue;

            PerSessionData *session = (PerSessionData *)lws_wsi_user(client);
            if (session)
            {
                total_queued += session->frame_queue.size();
                total_sent += session->frames_sent;
                total_dropped += session->frames_dropped;
            }
        }
    }
}