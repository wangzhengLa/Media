#ifndef WS_SIGNALING_CLIENT_H_
#define WS_SIGNALING_CLIENT_H_

#include <atomic>
#include <string>

namespace ecovacs
{
    // Signaling server endpoint parsed from ws:// or legacy http:// URLs.
    struct WsEndpoint {
        std::string host;
        int port = 0;
        std::string room;
    };

    // Accepts:
    //   ws://host:port/ws?room=name
    //   http://host:port/signal?room=name  (legacy mailbox URL, mapped to /ws)
    // Returns false on malformed input. Room defaults to "room1".
    bool ParseSignalingUrl(const std::string& url, WsEndpoint& ep);

    // Minimal JSON helpers for the tiny frame schema of the own server
    // ({"type":"offer"/"answer"/...,"sdp":"..."}).
    std::string JsonEscape(const std::string& s);
    bool JsonGetString(const std::string& json, const char* key,
                       std::string& val);

    // Minimal RFC 6455 client used by the WebRTC push worker:
    // blocking socket, masked client text frames, automatic pong replies,
    // single-frame messages up to 64KiB. No external dependency.
    class WsSignalingClient
    {
    public:
        enum class Event { kText, kClosed, kTimeout, kQuit, kError };

        WsSignalingClient() = default;
        ~WsSignalingClient();

        WsSignalingClient(const WsSignalingClient&) = delete;
        WsSignalingClient& operator=(const WsSignalingClient&) = delete;

        // Non-blocking connect (5s timeout) followed by the RFC 6455
        // upgrade as the given role ("caller"/"callee").
        bool Connect(const WsEndpoint& ep, const char* role,
                     std::atomic<bool>& quit);
        bool SendText(const std::string& text);

        // Wait up to timeout_ms for one complete text frame. Server pings
        // are answered transparently. kTimeout means "no frame in time",
        // the caller may simply loop.
        Event RecvText(int timeout_ms, std::atomic<bool>& quit,
                       std::string& out);
        void Close();

    private:
        static void RandomBytes(uint8_t* dst, size_t n);
        bool RecvExact(void* dst, size_t n);
        bool SendAll(const void* data, size_t n);
        bool SendControl(uint8_t opcode, const std::string& payload);
        bool Handshake(const WsEndpoint& ep, const char* role);

        int fd_ = -1;
        // Bytes read past the HTTP upgrade header (the server may send the
        // first WebSocket frame in the same TCP segment as the 101 reply);
        // RecvExact drains these before touching the socket.
        std::string in_extra_;
        size_t in_pos_ = 0;
    };

} // namespace ecovacs

#endif // WS_SIGNALING_CLIENT_H_
