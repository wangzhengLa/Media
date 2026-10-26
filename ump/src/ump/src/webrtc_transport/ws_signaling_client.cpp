#include "ws_signaling_client.h"

#include <log.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ecovacs
{
    namespace {

        // ---------------- Base64 (standard alphabet) ----------------
        std::string Base64Encode(const uint8_t* data, size_t len) {
            static const char* kTab =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                "0123456789+/";
            std::string out;
            out.reserve(((len + 2) / 3) * 4);
            for (size_t i = 0; i < len; i += 3) {
                const uint32_t n =
                    (static_cast<uint32_t>(data[i]) << 16) |
                    (i + 1 < len ? static_cast<uint32_t>(data[i + 1]) << 8 : 0) |
                    (i + 2 < len ? static_cast<uint32_t>(data[i + 2]) : 0);
                out.push_back(kTab[(n >> 18) & 0x3F]);
                out.push_back(kTab[(n >> 12) & 0x3F]);
                out.push_back(i + 1 < len ? kTab[(n >> 6) & 0x3F] : '=');
                out.push_back(i + 2 < len ? kTab[n & 0x3F] : '=');
            }
            return out;
        }

        // ---------------- SHA-1 (FIPS 180-4, handshake only) ----------------
        struct Sha1 {
            uint32_t h[5];
            uint64_t bitlen = 0;
            uint8_t buf[64];
            size_t blen = 0;

            Sha1() {
                h[0] = 0x67452301;
                h[1] = 0xEFCDAB89;
                h[2] = 0x98BADCFE;
                h[3] = 0x10325476;
                h[4] = 0xC3D2E1F0;
            }

            static uint32_t Rol(uint32_t v, int n) {
                return (v << n) | (v >> (32 - n));
            }

            void Block(const uint8_t* p) {
                uint32_t w[80];
                for (int i = 0; i < 16; ++i)
                    w[i] = (static_cast<uint32_t>(p[i * 4]) << 24) |
                           (static_cast<uint32_t>(p[i * 4 + 1]) << 16) |
                           (static_cast<uint32_t>(p[i * 4 + 2]) << 8) |
                           static_cast<uint32_t>(p[i * 4 + 3]);
                for (int i = 16; i < 80; ++i)
                    w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
                uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
                for (int i = 0; i < 80; ++i) {
                    uint32_t f, k;
                    if (i < 20) {
                        f = (b & c) | ((~b) & d);
                        k = 0x5A827999;
                    } else if (i < 40) {
                        f = b ^ c ^ d;
                        k = 0x6ED9EBA1;
                    } else if (i < 60) {
                        f = (b & c) | (b & d) | (c & d);
                        k = 0x8F1BBCDC;
                    } else {
                        f = b ^ c ^ d;
                        k = 0xCA62C1D6;
                    }
                    const uint32_t t = Rol(a, 5) + f + e + k + w[i];
                    e = d;
                    d = c;
                    c = Rol(b, 30);
                    b = a;
                    a = t;
                }
                h[0] += a;
                h[1] += b;
                h[2] += c;
                h[3] += d;
                h[4] += e;
            }

            void Update(const uint8_t* data, size_t len) {
                bitlen += static_cast<uint64_t>(len) * 8;
                while (len > 0) {
                    const size_t take = std::min(len, sizeof(buf) - blen);
                    std::memcpy(buf + blen, data, take);
                    blen += take;
                    data += take;
                    len -= take;
                    if (blen == sizeof(buf)) {
                        Block(buf);
                        blen = 0;
                    }
                }
            }

            void Final(uint8_t out[20]) {
                const uint64_t bits = bitlen;
                static const uint8_t kPad = 0x80;
                Update(&kPad, 1);
                const uint8_t kZero = 0;
                while (blen != 56)
                    Update(&kZero, 1);
                uint8_t lenbuf[8];
                for (int i = 0; i < 8; ++i)
                    lenbuf[i] = static_cast<uint8_t>(bits >> (56 - i * 8));
                Update(lenbuf, 8);
                for (int i = 0; i < 5; ++i) {
                    out[i * 4] = static_cast<uint8_t>(h[i] >> 24);
                    out[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
                    out[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
                    out[i * 4 + 3] = static_cast<uint8_t>(h[i]);
                }
            }
        };

    } // namespace

    bool ParseSignalingUrl(const std::string& url, WsEndpoint& ep) {
        size_t scheme = std::string::npos;
        int default_port = 8080;
        if (url.compare(0, 5, "ws://") == 0) {
            scheme = 5;
        } else if (url.compare(0, 6, "wss://") == 0) {
            scheme = 6;
            default_port = 443;
        } else if (url.compare(0, 7, "http://") == 0) {
            scheme = 7;  // legacy mailbox URL, mapped onto /ws
        } else if (url.compare(0, 8, "https://") == 0) {
            scheme = 8;
            default_port = 443;
        }
        if (scheme == std::string::npos)
            return false;

        const size_t slash = url.find('/', scheme);
        const size_t question = url.find('?', scheme);
        const size_t auth_end =
            slash != std::string::npos &&
                    (question == std::string::npos || slash < question)
                ? slash
                : question;
        if (auth_end == std::string::npos)
            return false;

        const std::string authority = url.substr(scheme, auth_end - scheme);
        const size_t colon = authority.find(':');
        if (colon == std::string::npos) {
            ep.host = authority;
            ep.port = default_port;
        } else {
            ep.host = authority.substr(0, colon);
            ep.port = std::atoi(authority.c_str() + colon + 1);
        }
        if (ep.host.empty() || ep.port <= 0 || ep.port > 65535)
            return false;

        ep.room = "room1";
        if (question != std::string::npos) {
            const std::string query = url.substr(question + 1);
            size_t pos = 0;
            while (pos < query.size()) {
                const size_t amp = query.find('&', pos);
                const std::string kv =
                    query.substr(pos, amp == std::string::npos
                                          ? std::string::npos
                                          : amp - pos);
                const size_t eq = kv.find('=');
                if (eq != std::string::npos &&
                    kv.substr(0, eq) == "room" && eq + 1 < kv.size())
                    ep.room = kv.substr(eq + 1);
                if (amp == std::string::npos)
                    break;
                pos = amp + 1;
            }
        }
        return !ep.room.empty();
    }

    std::string JsonEscape(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 8);
        char hex[8];
        for (unsigned char c : s) {
            switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    std::snprintf(hex, sizeof(hex), "\\u%04x", c);
                    out += hex;
                } else {
                    out.push_back(static_cast<char>(c));
                }
            }
        }
        return out;
    }

    bool JsonGetString(const std::string& j, const char* key,
                       std::string& val) {
        const std::string pat = std::string("\"") + key + "\"";
        size_t pos = j.find(pat);
        if (pos == std::string::npos)
            return false;
        pos += pat.size();
        while (pos < j.size() &&
               (j[pos] == ' ' || j[pos] == '\t' || j[pos] == ':'))
            ++pos;
        if (pos >= j.size() || j[pos] != '"')
            return false;
        ++pos;
        val.clear();
        while (pos < j.size() && j[pos] != '"') {
            if (j[pos] == '\\' && pos + 1 < j.size()) {
                const char e = j[pos + 1];
                switch (e) {
                case 'n':
                    val.push_back('\n');
                    break;
                case 'r':
                    val.push_back('\r');
                    break;
                case 't':
                    val.push_back('\t');
                    break;
                case 'b':
                    val.push_back('\b');
                    break;
                case 'f':
                    val.push_back('\f');
                    break;
                case '"':
                    val.push_back('"');
                    break;
                case '\\':
                    val.push_back('\\');
                    break;
                case '/':
                    val.push_back('/');
                    break;
                default:
                    val.push_back(e);
                }
                pos += 2;
            } else {
                val.push_back(j[pos++]);
            }
        }
        return true;
    }

    WsSignalingClient::~WsSignalingClient() {
        Close();
    }

    void WsSignalingClient::RandomBytes(uint8_t* dst, size_t n) {
        int rf = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        size_t got = 0;
        if (rf >= 0) {
            while (got < n) {
                ssize_t r = ::read(rf, dst + got, n - got);
                if (r <= 0)
                    break;
                got += static_cast<size_t>(r);
            }
            ::close(rf);
        }
        while (got < n)  // fallback, extremely unlikely
            dst[got++] = static_cast<uint8_t>(std::rand() & 0xFF);
    }

    bool WsSignalingClient::RecvExact(void* dst, size_t n) {
        size_t off = 0;
        auto* p = static_cast<uint8_t*>(dst);
        // Drain bytes swallowed together with the HTTP upgrade response.
        if (in_pos_ < in_extra_.size()) {
            const size_t take =
                std::min(n, in_extra_.size() - in_pos_);
            std::memcpy(p, in_extra_.data() + in_pos_, take);
            in_pos_ += take;
            off += take;
            if (in_pos_ == in_extra_.size()) {
                in_extra_.clear();
                in_pos_ = 0;
            }
        }
        while (off < n) {
            ssize_t r = ::recv(fd_, p + off, n - off, 0);
            if (r < 0 && errno == EINTR)
                continue;
            if (r <= 0)
                return false;
            off += static_cast<size_t>(r);
        }
        return true;
    }

    bool WsSignalingClient::SendAll(const void* data, size_t n) {
        size_t off = 0;
        const auto* p = static_cast<const uint8_t*>(data);
        while (off < n) {
            ssize_t r = ::send(fd_, p + off, n - off, MSG_NOSIGNAL);
            if (r < 0 && errno == EINTR)
                continue;
            if (r <= 0)
                return false;
            off += static_cast<size_t>(r);
        }
        return true;
    }

    bool WsSignalingClient::SendControl(uint8_t opcode,
                                        const std::string& payload) {
        uint8_t hdr[4] = {static_cast<uint8_t>(0x80 | opcode),
                          static_cast<uint8_t>(payload.size()), 0, 0};
        return SendAll(hdr, 2) &&
               (payload.empty() ||
                SendAll(payload.data(), payload.size()));
    }

    bool WsSignalingClient::Handshake(const WsEndpoint& ep, const char* role) {
        uint8_t nonce[16];
        RandomBytes(nonce, sizeof(nonce));
        const std::string key = Base64Encode(nonce, sizeof(nonce));
        const std::string path =
            "/ws?room=" + ep.room + "&role=" + role;

        uint8_t digest[20];
        Sha1 sha;
        const std::string acc_seed =
            key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        sha.Update(reinterpret_cast<const uint8_t*>(acc_seed.data()),
                   acc_seed.size());
        sha.Final(digest);
        const std::string accept = Base64Encode(digest, sizeof(digest));

        std::string req;
        req.reserve(256);
        req += "GET " + path + " HTTP/1.1\r\n";
        req += "Host: " + ep.host + ":" +
               std::to_string(ep.port) + "\r\n";
        req += "Upgrade: websocket\r\n";
        req += "Connection: Upgrade\r\n";
        req += "Sec-WebSocket-Key: " + key + "\r\n";
        req += "Sec-WebSocket-Version: 13\r\n\r\n";
        if (!SendAll(req.data(), req.size()))
            return false;

        std::string raw;
        char rbuf[1024];
        while (raw.size() < 8192) {
            ssize_t n = ::recv(fd_, rbuf, sizeof(rbuf), 0);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                return false;
            raw.append(rbuf, static_cast<size_t>(n));
            if (raw.find("\r\n\r\n") != std::string::npos)
                break;
        }
        const size_t hend = raw.find("\r\n\r\n");
        if (hend == std::string::npos ||
            raw.compare(0, 12, "HTTP/1.1 101") != 0) {
            ump_error("[webrtc] ws upgrade rejected\n");
            return false;
        }
        const std::string header = raw.substr(0, hend);
        if (header.find(accept) == std::string::npos) {
            ump_error("[webrtc] ws accept mismatch\n");
            return false;
        }
        // Preserve anything received after the header (e.g. the server's
        // "joined" frame piggy-backed on the same TCP segment); RecvExact
        // consumes these first.
        in_extra_.assign(raw, hend + 4, std::string::npos);
        in_pos_ = 0;
        return true;
    }

    bool WsSignalingClient::Connect(const WsEndpoint& ep, const char* role,
                                    std::atomic<bool>& quit) {
        struct addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* addresses = nullptr;
        char port_text[16];
        std::snprintf(port_text, sizeof(port_text), "%d", ep.port);
        if (getaddrinfo(ep.host.c_str(), port_text, &hints, &addresses) != 0)
            return false;
        for (struct addrinfo* a = addresses; a; a = a->ai_next) {
            if (quit.load(std::memory_order_acquire))
                break;
            int fd = ::socket(a->ai_family,
                              a->ai_socktype | SOCK_CLOEXEC,
                              a->ai_protocol);
            if (fd < 0)
                continue;
            const int fl = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, fl | O_NONBLOCK);
            int rc = ::connect(fd, a->ai_addr, a->ai_addrlen);
            if (rc == 0 || errno == EINPROGRESS) {
                if (rc != 0) {
                    struct pollfd pfd{};
                    pfd.fd = fd;
                    pfd.events = POLLOUT;
                    if (poll(&pfd, 1, 5000) != 1) {
                        ::close(fd);
                        continue;
                    }
                    int soerr = 0;
                    socklen_t solen = sizeof(soerr);
                    getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &solen);
                    if (soerr != 0) {
                        ::close(fd);
                        continue;
                    }
                }
                fcntl(fd, F_SETFL, fl);
                const struct timeval rw_timeout{5, 0};
                setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rw_timeout,
                           sizeof(rw_timeout));
                setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &rw_timeout,
                           sizeof(rw_timeout));
                fd_ = fd;
                break;
            }
            ::close(fd);
        }
        freeaddrinfo(addresses);
        return fd_ >= 0 && Handshake(ep, role);
    }

    bool WsSignalingClient::SendText(const std::string& text) {
        uint8_t mask_key[4];
        RandomBytes(mask_key, sizeof(mask_key));
        std::string frame;
        frame.reserve(text.size() + 10);
        frame.push_back(static_cast<char>(0x81));  // FIN + text
        const size_t len = text.size();
        if (len < 126) {
            frame.push_back(static_cast<char>(0x80 | len));
        } else {
            frame.push_back(static_cast<char>(0x80 | 126));
            frame.push_back(static_cast<char>((len >> 8) & 0xFF));
            frame.push_back(static_cast<char>(len & 0xFF));
        }
        frame.append(reinterpret_cast<const char*>(mask_key), 4);
        for (size_t i = 0; i < len; ++i)
            frame.push_back(static_cast<char>(
                static_cast<uint8_t>(text[i]) ^ mask_key[i % 4]));
        return SendAll(frame.data(), frame.size());
    }

    WsSignalingClient::Event WsSignalingClient::RecvText(
        int timeout_ms, std::atomic<bool>& quit, std::string& out) {
        std::string payload;
        bool got_text = false;
        while (!quit.load(std::memory_order_acquire)) {
            struct pollfd pfd{};
            pfd.fd = fd_;
            pfd.events = POLLIN;
            int pr = poll(&pfd, 1, timeout_ms);
            if (pr < 0 && errno == EINTR)
                continue;
            if (pr == 0)
                return Event::kTimeout;
            if (pr < 0 || !(pfd.revents & (POLLIN | POLLHUP | POLLERR)))
                return Event::kError;

            uint8_t hdr[2];
            if (!RecvExact(hdr, 2))
                return Event::kError;
            const uint8_t opcode = hdr[0] & 0x0F;
            bool masked = (hdr[1] & 0x80) != 0;
            uint64_t plen = hdr[1] & 0x7F;
            uint8_t ext[8];
            if (plen == 126) {
                if (!RecvExact(ext, 2))
                    return Event::kError;
                plen = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
            } else if (plen == 127) {
                if (!RecvExact(ext, 8))
                    return Event::kError;
                plen = 0;
                for (int i = 0; i < 8; ++i)
                    plen = (plen << 8) | ext[i];
            }
            uint8_t mask[4] = {};
            if (masked && !RecvExact(mask, 4))
                return Event::kError;
            if (plen > 65536)
                return Event::kError;
            std::string chunk(static_cast<size_t>(plen), '\0');
            if (plen && !RecvExact(&chunk[0], static_cast<size_t>(plen)))
                return Event::kError;
            if (masked)
                for (size_t i = 0; i < chunk.size(); ++i)
                    chunk[i] = static_cast<char>(
                        static_cast<uint8_t>(chunk[i]) ^ mask[i % 4]);

            if (opcode == 0x8) {  // close
                SendControl(0x8, chunk);
                return Event::kClosed;
            }
            if (opcode == 0x9) {  // ping -> pong, keep waiting
                SendControl(0xA, chunk);
                continue;
            }
            if (opcode == 0xA)  // pong
                continue;
            if (opcode == 0x1) {
                payload.clear();
                got_text = true;
            } else if (opcode != 0x0) {
                continue;  // binary/other: ignored
            }
            payload += chunk;
            if ((hdr[0] & 0x80) != 0) {
                if (!got_text)
                    continue;  // fragmented binary
                out = std::move(payload);
                return Event::kText;
            }
        }
        return Event::kQuit;
    }

    void WsSignalingClient::Close() {
        if (fd_ >= 0) {
            ::shutdown(fd_, SHUT_RDWR);
            ::close(fd_);
            fd_ = -1;
        }
    }

} // namespace ecovacs
