#include "webrtc_transport/peerconnection.h"

#include <log.h>
#include "osp.h"
#include "ws_signaling_client.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "api/audio_codecs/builtin_audio_decoder_factory.h"

namespace ecovacs {
namespace wt {

using webrtc::scoped_refptr;

// ---- Helpers (file-local) ----
static bool H264AccessUnitIsKey(const uint8_t* data, size_t size) {
    size_t i = 0;
    while (i + 2 < size) {
        if (data[i] == 0 && data[i + 1] == 0) {
            size_t start_len = 0;
            if (data[i + 2] == 1) start_len = 3;
            else if (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1) start_len = 4;
            if (start_len) {
                uint8_t nalu_type = data[i + start_len] & 0x1F;
                if (nalu_type == 5 || nalu_type == 7) return true;
                i += start_len + 1; continue;
            }
        }
        ++i;
    }
    return false;
}

static uint64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

template<typename Rep, typename Period>
static void SleepInterruptible(std::chrono::duration<Rep, Period> d,
                               const std::atomic<bool>& quit) {
    auto end = std::chrono::steady_clock::now() + d;
    while (!quit.load(std::memory_order_acquire)) {
        auto now = std::chrono::steady_clock::now();
        if (now >= end) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// =====================================================================
//  PushingObserver
// =====================================================================
void PushingObserver::OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState s) {
    ump_info("[webrtc] signaling state -> %d\n", static_cast<int>(s));
}

void PushingObserver::OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState s) {
    ump_info("[webrtc] ICE gathering state -> %d\n", static_cast<int>(s));
    if (s == webrtc::PeerConnectionInterface::kIceGatheringComplete) {
        std::lock_guard<std::mutex> g(ice_mu_); ice_complete_ = true; ice_cv_.notify_all();
    }
}

bool PushingObserver::WaitIceComplete(std::chrono::milliseconds timeout, const std::atomic<bool>& quit) {
    std::unique_lock<std::mutex> lk(ice_mu_);
    return ice_cv_.wait_for(lk, timeout, [&] { return ice_complete_ || quit.load(std::memory_order_acquire); });
}

void PushingObserver::OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState s) {
    ump_info("[webrtc] peer connection state -> %d\n", static_cast<int>(s));
    using PCState = webrtc::PeerConnectionInterface::PeerConnectionState;
    bool connected = (s == PCState::kConnected);
    bool down = (s == PCState::kFailed || s == PCState::kDisconnected || s == PCState::kClosed);
    if ((connected && !connected_) || (down && connected_)) {
        connected_ = connected;
        if (on_connection_edge) on_connection_edge(connected);
    }
}

// =====================================================================
//  PromiseCreateSdpObserver
// =====================================================================
void PromiseCreateSdpObserver::OnSuccess(webrtc::SessionDescriptionInterface* d) {
    std::lock_guard<std::mutex> g(mu_); desc_.reset(d); done_ = true; cv_.notify_all();
}

void PromiseCreateSdpObserver::OnFailure(webrtc::RTCError e) {
    std::lock_guard<std::mutex> g(mu_); err_ = e.message(); done_ = true; cv_.notify_all();
}

bool PromiseCreateSdpObserver::Wait(const std::atomic<bool>& quit) {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait_for(lk, std::chrono::seconds(10), [&] { return done_ || quit.load(std::memory_order_acquire); });
    return done_ && desc_ != nullptr;
}

std::unique_ptr<webrtc::SessionDescriptionInterface> PromiseCreateSdpObserver::TakeDesc() {
    return std::move(desc_);
}

// =====================================================================
//  WebrtcTransportImpl
// =====================================================================
WebrtcTransportImpl::~WebrtcTransportImpl() { Stop(); }

int32_t WebrtcTransportImpl::Init(const WebrtcTransportConfig& c) { config_ = c; return 0; }

void WebrtcTransportImpl::SetVideoEncoder(IWebrtcVideoEncoder* e) {
    video_encoder_ = e;
    if (aggregator_) aggregator_->SetVideoEncoder(e);
}

void WebrtcTransportImpl::SetViewerListener(IWebrtcViewerListener* l) { viewer_listener_ = l; }

void WebrtcTransportImpl::SetAudioReceiver(IWebrtcAudioReceiver* r) {
    audio_receiver_ = r;
    ump_info("[webrtc] SetAudioReceiver called (audio_recv not implemented yet, reserved)\n");
}

bool WebrtcTransportImpl::Started() const { return started_.load(std::memory_order_acquire); }

int32_t WebrtcTransportImpl::Start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        ump_info("[webrtc] transport already running\n"); return 0;
    }
    quit_.store(false, std::memory_order_release);
    ump_info("[webrtc] Start (master), fps=%d, max_viewers=%d, audio=%d, signaling=%s\n",
             config_.video_fps, config_.max_viewers, config_.enable_audio ? 1 : 0,
             config_.signaling_url.c_str());
    if (!CreateFactory()) { started_.store(false); return -1; }
    sig_quit_.store(false);
    start_time_us_ = NowUs();
    mailbox_thread_ = std::thread([this] { RunSignaling(); });
    send_thread_ = std::thread([this] { SendLoop(); });
    ump_info("[webrtc] Start done\n");
    return 0;
}

int32_t WebrtcTransportImpl::Stop() {
    bool expected = true;
    if (!started_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) return 0;
    quit_.store(true); sig_quit_.store(true);
    if (send_thread_.joinable()) { send_thread_.join(); ump_info("[webrtc] send thread joined\n"); }
    if (mailbox_thread_.joinable()) { mailbox_thread_.join(); ump_info("[webrtc] signaling thread joined\n"); }
    DestroyFactory();
    {
        std::lock_guard<std::mutex> lk(q_mu_);
        while (!frame_q_.empty()) { frame_q_.front()->RelRef(); frame_q_.pop(); }
    }
    ump_info("[webrtc] Stop done\n");
    return 0;
}

void WebrtcTransportImpl::SendH264Frame(const uint8_t* data, size_t size, bool keyframe) {
    if (!started_.load(std::memory_order_acquire) || !data || size == 0) return;
    auto holder = std::make_shared<FrameHolder>();
    holder->data = new uint8_t[size];
    holder->size = size;
    holder->keyframe = keyframe;
    std::memcpy(holder->data, data, size);
    std::lock_guard<std::mutex> lk(q_mu_);
    if (frame_q_.size() >= 60) {
        if (!frame_q_.empty()) { frame_q_.front()->RelRef(); frame_q_.pop(); }
        ump_error("[webrtc] video queue full, drop frame\n");
    }
    frame_q_.push(holder);
}

void WebrtcTransportImpl::SendG711Frame(const uint8_t* data, size_t size) {
    if (!started_.load(std::memory_order_acquire) || !config_.enable_audio) return;
    if (!data || size == 0) return;
    std::vector<std::shared_ptr<PeerContext>> snapshot;
    { std::lock_guard<std::mutex> lk(peers_mu_); snapshot = peers_; }
    int fed = 0;
    for (auto& peer : snapshot) {
        if (!peer->pc || !peer->connected.load(std::memory_order_acquire)) continue;
        ExternalG711Encoder* enc = peer->audio_registry ? peer->audio_registry->active : nullptr;
        if (enc) { enc->FeedG711Frame(data, size); ++fed; }
    }
    static uint32_t g711_cnt = 0;
    if (++g711_cnt % 50 == 0) {
        ump_info("[webrtc] g711 #%u size=%zu fed_peers=%d total_peers=%zu\n",
                 g711_cnt, size, fed, snapshot.size());
    }
}

bool WebrtcTransportImpl::CreateFactory() {
    network_thread_ = webrtc::Thread::CreateWithSocketServer();
    signaling_thread_ = webrtc::Thread::Create();
    network_thread_->Start(); signaling_thread_->Start();
    track_source_ = webrtc::make_ref_counted<ExternalVideoTrackSource>();
    aggregator_ = std::make_shared<BitrateAggregator>();
    if (video_encoder_) aggregator_->SetVideoEncoder(video_encoder_);
    ump_info("[webrtc] shared threads + track source created\n");
    return true;
}

void WebrtcTransportImpl::DestroyFactory() {
    {
        std::lock_guard<std::mutex> lk(peers_mu_);
        for (auto& p : peers_) {
            if (p->pc) p->pc->Close();
            p->track = nullptr;
            p->audio_track = nullptr;
            p->pc = nullptr;
            p->owned_factory = nullptr;
        }
        peers_.clear();
    }
    track_source_ = nullptr;
    aggregator_ = nullptr;
    if (network_thread_) { network_thread_->Stop(); network_thread_.reset(); }
    if (signaling_thread_) { signaling_thread_->Stop(); signaling_thread_.reset(); }
}

std::shared_ptr<WebrtcTransportImpl::PeerContext> WebrtcTransportImpl::CreatePeer(const std::string& peer_id) {
    auto peer = std::make_shared<PeerContext>();
    peer->peer_id = peer_id;
    peer->registry = std::make_shared<ExternalH264Encoder::Registry>();
    peer->registry->video_encoder = video_encoder_;
    peer->registry->aggregator = aggregator_;

    if (config_.enable_audio) {
        peer->audio_registry = std::make_shared<ExternalG711Encoder::Registry>();
    }

    peer->observer = std::make_unique<PushingObserver>();
    std::weak_ptr<PeerContext> weak = peer;
    peer->observer->on_connection_edge = [this, weak](bool c) {
        auto p = weak.lock();
        if (!p) return;
        p->connected.store(c, std::memory_order_release);
        if (c) {
            p->need_keyframe = true;
            ump_info("[webrtc] viewer #%d connected\n", p->id);
            if (viewer_listener_) viewer_listener_->OnViewerConnected(p->id);
        } else {
            ump_info("[webrtc] viewer #%d disconnected\n", p->id);
            if (viewer_listener_) viewer_listener_->OnViewerDisconnected(p->id);
            std::lock_guard<std::mutex> lk(peers_mu_);
            p->pc = nullptr;
        }
    };

    webrtc::PeerConnectionInterface::RTCConfiguration rtc;
    if (!config_.stun_url.empty()) {
        webrtc::PeerConnectionInterface::IceServer s; s.urls.push_back(config_.stun_url);
        rtc.servers.push_back(s);
    }
    if (!config_.turn_url.empty()) {
        webrtc::PeerConnectionInterface::IceServer s; s.urls.push_back(config_.turn_url);
        s.username = config_.turn_username; s.password = config_.turn_password;
        rtc.servers.push_back(s);
    }

    auto enc_factory = std::make_unique<ExternalH264EncoderFactory>(peer->registry);
    scoped_refptr<webrtc::AudioEncoderFactory> audio_enc_factory;
    if (config_.enable_audio) {
        audio_enc_factory = scoped_refptr<webrtc::AudioEncoderFactory>(
            new ExternalG711EncoderFactory(peer->audio_registry));
    } else {
        audio_enc_factory = scoped_refptr<webrtc::AudioEncoderFactory>(new EmptyAudioEncoderFactory());
    }
    scoped_refptr<webrtc::AudioDeviceModule> adm(
        new FakeAudioDeviceModule());
    scoped_refptr<webrtc::AudioDecoderFactory> audio_dec_factory;
    if (config_.enable_audio) {
        // Even though we only send audio (sendonly), libwebrtc validates
        // the recv codec list against the offer during SetLocalDescription.
        // An empty decoder factory fails that check, so use the builtin one.
        audio_dec_factory = webrtc::CreateBuiltinAudioDecoderFactory();
    } else {
        audio_dec_factory = scoped_refptr<webrtc::AudioDecoderFactory>(new EmptyAudioDecoderFactory());
    }
    peer->owned_factory = webrtc::CreatePeerConnectionFactory(
        network_thread_.get(), signaling_thread_.get(),
        adm,
        audio_enc_factory,
        audio_dec_factory,
        std::move(enc_factory),
        nullptr, nullptr, nullptr);
    if (!peer->owned_factory) { ump_error("[webrtc] per-peer factory failed\n"); return nullptr; }

    peer->track = peer->owned_factory->CreateVideoTrack(track_source_, "video0");
    if (!peer->track) { ump_error("[webrtc] CreateVideoTrack failed\n"); return nullptr; }

    if (config_.enable_audio) {
        peer->audio_track = peer->owned_factory->CreateAudioTrack("audio0", nullptr);
        if (!peer->audio_track) {
            ump_error("[webrtc] CreateAudioTrack failed\n"); return nullptr;
        }
    }

    bool pc_ok = signaling_thread_->BlockingCall([&] {
        auto r = peer->owned_factory->CreatePeerConnectionOrError(
            rtc, webrtc::PeerConnectionDependencies(peer->observer.get()));
        if (!r.ok()) return false;
        peer->pc = r.MoveValue();
        auto add = peer->pc->AddTrack(peer->track, {"stream0"});
        if (!add.ok()) return false;
        if (config_.enable_audio && peer->audio_track) {
            auto add_audio = peer->pc->AddTrack(peer->audio_track, {"stream0"});
            if (!add_audio.ok()) {
                ump_error("[webrtc] AddTrack(audio) failed: %s\n",
                          add_audio.error().message());
                return false;
            }
        }
        return true;
    });
    if (!pc_ok) { ump_error("[webrtc] CreatePeerConnection/AddTrack failed\n"); return nullptr; }

    peer->id = next_viewer_id_.fetch_add(1, std::memory_order_acq_rel);
    ump_info("[webrtc] viewer #%d peer created, peer_id=%s, audio=%d\n",
             peer->id, peer_id.c_str(), config_.enable_audio ? 1 : 0);
    return peer;
}

std::string WebrtcTransportImpl::HandleOffer(std::shared_ptr<PeerContext> peer, const std::string& offer_sdp) {
    auto offer_desc = webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, offer_sdp);
    if (!offer_desc) { ump_error("[webrtc] parse offer SDP failed\n"); return {}; }
    auto set_remote = scoped_refptr<PromiseSetRemoteObserver>(new PromiseSetRemoteObserver());
    peer->pc->SetRemoteDescription(std::move(offer_desc), set_remote);
    if (!set_remote->Wait(sig_quit_)) {
        ump_error("[webrtc] SetRemoteDescription failed: %s\n", set_remote->error().c_str()); return {};
    }
    ump_info("[webrtc] remote offer applied, creating answer\n");

    auto answer_obs = scoped_refptr<PromiseCreateSdpObserver>(new PromiseCreateSdpObserver());
    peer->pc->CreateAnswer(answer_obs.get(), webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
    if (!answer_obs->Wait(sig_quit_)) {
        ump_error("[webrtc] CreateAnswer failed: %s\n", answer_obs->error().c_str()); return {};
    }
    auto answer_desc = answer_obs->TakeDesc();
    auto set_local = scoped_refptr<PromiseSetLocalObserver>(new PromiseSetLocalObserver());
    peer->pc->SetLocalDescription(std::move(answer_desc), set_local);
    if (!set_local->Wait(sig_quit_)) {
        ump_error("[webrtc] SetLocalDescription failed: %s\n", set_local->error().c_str()); return {};
    }
    if (!peer->observer->WaitIceComplete(std::chrono::seconds(10), sig_quit_)) {
        ump_warn("[webrtc] ICE gathering timeout, sending answer anyway\n");
    }
    std::string sdp;
    bool ok = signaling_thread_->BlockingCall([&] {
        const webrtc::SessionDescriptionInterface* local = peer->pc->local_description();
        if (!local) return false;
        return local->ToString(&sdp) && !sdp.empty();
    });
    if (!ok) { ump_error("[webrtc] serialize answer SDP failed\n"); return {}; }
    ump_info("[webrtc] answer ready, sdp_len=%zu\n", sdp.size());
    return sdp;
}

void WebrtcTransportImpl::RemoveDeadPeers() {
    std::lock_guard<std::mutex> lk(peers_mu_);
    peers_.erase(
        std::remove_if(peers_.begin(), peers_.end(),
            [](const std::shared_ptr<PeerContext>& p) { return p->pc == nullptr; }),
        peers_.end());
}

void WebrtcTransportImpl::RunSignaling() {
    WsEndpoint ep;
    if (!ParseSignalingUrl(config_.signaling_url, ep)) {
        ump_error("[webrtc] bad signaling url: %s\n", config_.signaling_url.c_str());
        return;
    }
    ump_info("[webrtc] signaling start (master), ws://%s:%d/ws?room=%s\n",
             ep.host.c_str(), ep.port, ep.room.c_str());

    while (!sig_quit_.load(std::memory_order_acquire)) {
        WsSignalingClient ws;
        if (!ws.Connect(ep, "callee", sig_quit_)) {
            ump_error("[webrtc] ws connect failed, retry in 1s\n");
            SleepInterruptible(std::chrono::seconds(1), sig_quit_); continue;
        }
        ump_info("[webrtc] ws connected as callee, room=%s\n", ep.room.c_str());

        std::string msg;
        bool joined = false;
        while (!sig_quit_.load(std::memory_order_acquire)) {
            using Ev = WsSignalingClient::Event;
            Ev ev = ws.RecvText(10000, sig_quit_, msg);
            if (ev == Ev::kQuit) return;
            if (ev == Ev::kTimeout) { RemoveDeadPeers(); continue; }
            if (ev != Ev::kText) { ump_error("[webrtc] ws closed, reconnect\n"); break; }

            std::string type;
            if (!JsonGetString(msg, "type", type)) continue;

            if (type == "joined") { joined = true; ump_info("[webrtc] joined, waiting for offers\n"); continue; }
            if (!joined) continue;

            if (type == "offer") {
                std::string offer_sdp, peer_id;
                JsonGetString(msg, "sdp", offer_sdp);
                JsonGetString(msg, "peer_id", peer_id);
                if (offer_sdp.empty()) { ump_error("[webrtc] offer with empty sdp\n"); continue; }

                {
                    std::lock_guard<std::mutex> lk(peers_mu_);
                    int live = 0;
                    for (auto& p : peers_) if (p->pc) ++live;
                    if (live >= config_.max_viewers) {
                        ump_warn("[webrtc] max viewers (%d) reached, rejecting offer\n", config_.max_viewers);
                        continue;
                    }
                }

                auto peer = CreatePeer(peer_id.empty() ? "viewer" : peer_id);
                if (!peer) { ump_error("[webrtc] create peer failed\n"); continue; }
                {
                    std::lock_guard<std::mutex> lk(peers_mu_);
                    peers_.push_back(peer);
                }

                std::string answer_sdp = HandleOffer(peer, offer_sdp);
                if (answer_sdp.empty()) {
                    ump_error("[webrtc] handle offer failed\n");
                    std::lock_guard<std::mutex> lk(peers_mu_);
                    if (peer->pc) { peer->pc->Close(); peer->pc = nullptr; }
                    continue;
                }

                std::string answer_frame =
                    "{\"type\":\"answer\",\"sdp\":\"" + JsonEscape(answer_sdp) + "\"";
                if (!peer_id.empty()) {
                    answer_frame += ",\"peer_id\":\"" + JsonEscape(peer_id) + "\"";
                }
                answer_frame += "}";
                if (!ws.SendText(answer_frame)) {
                    ump_error("[webrtc] send answer failed\n");
                } else {
                    ump_info("[webrtc] answer sent to viewer #%d, sdp_len=%zu\n",
                             peer->id, answer_sdp.size());
                }
            } else if (type == "bye") {
                std::string bye_peer;
                JsonGetString(msg, "peer_id", bye_peer);
                scoped_refptr<webrtc::PeerConnectionInterface> to_close;
                {
                    std::lock_guard<std::mutex> lk(peers_mu_);
                    for (auto& p : peers_) {
                        if (p->peer_id == bye_peer && p->pc) {
                            ump_info("[webrtc] viewer #%d (peer_id=%s) left, closing\n",
                                     p->id, bye_peer.c_str());
                            p->connected.store(false, std::memory_order_release);
                            to_close = p->pc;
                            p->pc = nullptr;
                        }
                    }
                }
                if (to_close) to_close->Close();
            }
        }
        RemoveDeadPeers();
        SleepInterruptible(std::chrono::seconds(1), sig_quit_);
    }
}

void WebrtcTransportImpl::SendLoop() {
    const int interval_us = 1000000 / config_.video_fps;
    uint64_t next_frame_us = 0;
    int sent_cnt = 0;
    while (!quit_.load(std::memory_order_acquire)) {
        uint64_t now_us = NowUs();
        if (next_frame_us == 0) next_frame_us = now_us;
        if (now_us >= next_frame_us) {
            next_frame_us += interval_us;
            if (now_us > next_frame_us + 200000) next_frame_us = now_us + interval_us;

            std::shared_ptr<FrameHolder> holder;
            { std::lock_guard<std::mutex> lk(q_mu_); if (!frame_q_.empty()) { holder = frame_q_.front(); frame_q_.pop(); } }
            if (holder) {
                bool is_key = H264AccessUnitIsKey(holder->data, holder->size);
                uint32_t ts = static_cast<uint32_t>((NowUs() - start_time_us_) * 90 / 1000);

                std::vector<std::shared_ptr<PeerContext>> snapshot;
                { std::lock_guard<std::mutex> lk(peers_mu_); snapshot = peers_; }

                for (auto& peer : snapshot) {
                    if (!peer->pc || !peer->connected.load(std::memory_order_acquire)) continue;
                    ExternalH264Encoder* enc = peer->registry ? peer->registry->active : nullptr;
                    if (!enc) continue;
                    if (peer->need_keyframe) {
                        if (!is_key) continue;
                        peer->need_keyframe = false;
                        ump_info("[webrtc] viewer #%d keyframe found, start sending\n", peer->id);
                    }
                    enc->InjectH264Frame(holder->data, holder->size, is_key, ts);
                }
                if (++sent_cnt % 50 == 0) {
                    int n = 0;
                    for (auto& p : snapshot) if (p->connected.load(std::memory_order_acquire)) ++n;
                    ump_info("[webrtc] sent #%d size=%zu viewers=%d\n", sent_cnt, holder->size, n);
                }
            }
        }
        usleep(1000);
    }
    ump_info("[webrtc] send thread exited\n");
}

} // namespace wt

// =====================================================================
//  Public factory
// =====================================================================
std::unique_ptr<WebrtcTransport> WebrtcTransport::Create() {
    return std::make_unique<wt::WebrtcTransportImpl>();
}

} // namespace ecovacs
