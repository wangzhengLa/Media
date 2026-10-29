#include "webrtc_transport/peerconnection.h"

#include <log.h>
#include "osp.h"
#include "ws_signaling_client.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "api/audio_codecs/builtin_audio_decoder_factory.h"

namespace ecovacs {
namespace wt {

using webrtc::scoped_refptr;

// ---- Helpers (file-local) ----
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
}

void PushingObserver::OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState s) {
    ump_info("[webrtc] ICE connection state -> %d\n", static_cast<int>(s));
}

void PushingObserver::OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState s) {
    ump_info("[webrtc] peer connection state -> %d\n", static_cast<int>(s));
    using PCState = webrtc::PeerConnectionInterface::PeerConnectionState;
    // kDisconnected is transient — ICE may recover on its own.
    // Only kFailed / kClosed are terminal.
    bool connected = (s == PCState::kConnected);
    bool down = (s == PCState::kFailed || s == PCState::kClosed);
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

void WebrtcTransportImpl::SetVideoReceiver(IWebrtcVideoReceiver* r) {
    video_receiver_ = r;
    ump_info("[webrtc] SetVideoReceiver called (video_recv not implemented yet, reserved)\n");
}

bool WebrtcTransportImpl::Started() const { return started_.load(std::memory_order_acquire); }

int32_t WebrtcTransportImpl::Start() {
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        ump_info("[webrtc] transport already running\n"); return 0;
    }
    quit_.store(false, std::memory_order_release);
    ump_info("[webrtc] Start (master), max_viewers=%d, signaling=%s\n",
             config_.max_viewers, config_.signaling_url.c_str());
    if (!CreateFactory()) { started_.store(false); return -1; }
    sig_quit_.store(false);
    mailbox_thread_ = std::thread([this] { RunSignaling(); });
    ump_info("[webrtc] Start done\n");
    return 0;
}

int32_t WebrtcTransportImpl::Stop() {
    bool expected = true;
    if (!started_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) return 0;
    quit_.store(true); sig_quit_.store(true);
    if (mailbox_thread_.joinable()) { mailbox_thread_.join(); ump_info("[webrtc] signaling thread joined\n"); }
    DestroyFactory();
    ump_info("[webrtc] Stop done\n");
    return 0;
}

void WebrtcTransportImpl::SendVideoFrame(const uint8_t* data, size_t size, bool keyframe) {
    if (!started_.load(std::memory_order_acquire) || !data || size == 0) return;
    std::vector<std::shared_ptr<PeerContext>> snapshot;
    { std::lock_guard<std::mutex> lk(peers_mu_); snapshot = peers_; }
    int fed = 0;
    for (auto& peer : snapshot) {
        if (!peer->pc || !peer->connected.load(std::memory_order_acquire)) continue;
        if (peer->need_keyframe) {
            if (!keyframe) continue;
            peer->need_keyframe = false;
            ump_info("[webrtc] viewer (peer_id=%s) keyframe found, start sending\n",
                     peer->peer_id.c_str());
        }
        ExternalVideoEncoder* enc = peer->video_registry ? peer->video_registry->active : nullptr;
        if (enc) { enc->InjectVideoFrame(data, size, keyframe); ++fed; }
    }
    static uint32_t vid_cnt = 0;
    if (++vid_cnt % 50 == 0) {
        ump_info("[webrtc] video #%u size=%zu key=%d fed_peers=%d total_peers=%zu\n",
                 vid_cnt, size, keyframe ? 1 : 0, fed, snapshot.size());
    }
}

void WebrtcTransportImpl::SendAudioFrame(const uint8_t* data, size_t size) {
    if (!started_.load(std::memory_order_acquire) || !data || size == 0) return;
    std::vector<std::shared_ptr<PeerContext>> snapshot;
    { std::lock_guard<std::mutex> lk(peers_mu_); snapshot = peers_; }
    int fed = 0;
    for (auto& peer : snapshot) {
        if (!peer->pc || !peer->connected.load(std::memory_order_acquire)) continue;
        ExternalAudioEncoder* enc = peer->audio_registry ? peer->audio_registry->active : nullptr;
        if (enc) { enc->FeedAudioFrame(data, size); ++fed; }
    }
    static uint32_t aud_cnt = 0;
    if (++aud_cnt % 50 == 0) {
        ump_info("[webrtc] audio #%u size=%zu fed_peers=%d total_peers=%zu\n",
                 aud_cnt, size, fed, snapshot.size());
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
    // Snapshot peers and call pc->Close() WITHOUT holding peers_mu_.
    // Close() runs synchronously on the signaling thread and fires
    // OnConnectionChange(kClosed), whose callback (on_connection_edge) also
    // takes peers_mu_. Holding the lock here would deadlock: Stop thread
    // waits in Close() for the signaling thread, while the signaling thread
    // waits for peers_mu_ held by the Stop thread.
    std::vector<std::shared_ptr<PeerContext>> snapshot;
    {
        std::lock_guard<std::mutex> lk(peers_mu_);
        snapshot = peers_;
    }
    for (auto& p : snapshot) {
        if (p->pc) p->pc->Close();
    }

    // Close callbacks (if any) have already run. Now drop all references
    // under the lock. It is harmless if a callback already nulled p->pc.
    {
        std::lock_guard<std::mutex> lk(peers_mu_);
        for (auto& p : peers_) {
            p->video_track = nullptr;
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
    peer->video_registry = std::make_shared<ExternalVideoEncoder::Registry>();
    peer->video_registry->video_encoder = video_encoder_;
    peer->video_registry->aggregator = aggregator_;
    peer->audio_registry = std::make_shared<ExternalAudioEncoder::Registry>();

    peer->observer = std::make_unique<PushingObserver>();
    std::weak_ptr<PeerContext> weak = peer;
    peer->observer->on_connection_edge = [this, weak](bool c) {
        auto p = weak.lock();
        if (!p) return;
        p->connected.store(c, std::memory_order_release);
        if (c) {
            p->need_keyframe = true;
            ump_info("[webrtc] viewer (peer_id=%s) connected\n", p->peer_id.c_str());
            if (viewer_listener_) viewer_listener_->OnViewerConnected(p->peer_id);
        } else {
            ump_info("[webrtc] viewer (peer_id=%s) disconnected\n", p->peer_id.c_str());
            if (viewer_listener_) viewer_listener_->OnViewerDisconnected(p->peer_id);
            // Do NOT release p->pc here. This callback runs inside the
            // PeerConnection's own state-change / Close() call stack; dropping
            // the last reference here would destroy the PC mid-call and cause
            // a use-after-free. Actual teardown is done by RunSignaling's
            // periodic cleanup (for ICE failures) or DestroyFactory (stop).
        }
    };
    peer->observer->on_ice_candidate = [this, weak](const webrtc::IceCandidate* c) {
        auto p = weak.lock();
        if (!p) return;
        SendIceCandidate(p->peer_id, c);
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

    auto enc_factory = std::make_unique<ExternalVideoEncoderFactory>(peer->video_registry);
    scoped_refptr<webrtc::AudioEncoderFactory> audio_enc_factory =
        scoped_refptr<webrtc::AudioEncoderFactory>(
            new ExternalAudioEncoderFactory(peer->audio_registry));
    scoped_refptr<webrtc::AudioDeviceModule> adm(
        new FakeAudioDeviceModule());
    // libwebrtc validates the recv codec list against the offer during
    // SetLocalDescription, so use the builtin decoder factory.
    scoped_refptr<webrtc::AudioDecoderFactory> audio_dec_factory =
        webrtc::CreateBuiltinAudioDecoderFactory();
    peer->owned_factory = webrtc::CreatePeerConnectionFactory(
        network_thread_.get(), signaling_thread_.get(),
        adm,
        audio_enc_factory,
        audio_dec_factory,
        std::move(enc_factory),
        nullptr, nullptr, nullptr);
    if (!peer->owned_factory) { ump_error("[webrtc] per-peer factory failed\n"); return nullptr; }

    peer->video_track = peer->owned_factory->CreateVideoTrack(track_source_, "video0");
    if (!peer->video_track) { ump_error("[webrtc] CreateVideoTrack failed\n"); return nullptr; }

    peer->audio_track = peer->owned_factory->CreateAudioTrack("audio0", nullptr);
    if (!peer->audio_track) { ump_error("[webrtc] CreateAudioTrack failed\n"); return nullptr; }

    bool pc_ok = signaling_thread_->BlockingCall([&] {
        auto r = peer->owned_factory->CreatePeerConnectionOrError(
            rtc, webrtc::PeerConnectionDependencies(peer->observer.get()));
        if (!r.ok()) return false;
        peer->pc = r.MoveValue();
        auto add = peer->pc->AddTrack(peer->video_track, {"stream0"});
        if (!add.ok()) return false;
        auto add_audio = peer->pc->AddTrack(peer->audio_track, {"stream0"});
        if (!add_audio.ok()) {
            ump_error("[webrtc] AddTrack(audio) failed: %s\n",
                      add_audio.error().message());
            return false;
        }
        return true;
    });
    if (!pc_ok) { ump_error("[webrtc] CreatePeerConnection/AddTrack failed\n"); return nullptr; }

    ump_info("[webrtc] viewer peer created, peer_id=%s\n", peer_id.c_str());
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
    // Trickle ICE: send the answer immediately; candidates are forwarded
    // separately via OnIceCandidate -> SendIceCandidate.
    std::string sdp;
    bool ok = signaling_thread_->BlockingCall([&] {
        const webrtc::SessionDescriptionInterface* local = peer->pc->local_description();
        if (!local) return false;
        return local->ToString(&sdp) && !sdp.empty();
    });
    if (!ok) { ump_error("[webrtc] serialize answer SDP failed\n"); return {}; }
    ump_info("[webrtc] answer ready (trickle), sdp_len=%zu\n", sdp.size());
    return sdp;
}

void WebrtcTransportImpl::SendIceCandidate(const std::string& peer_id,
                                           const webrtc::IceCandidate* candidate) {
    std::string frame;
    if (!candidate) {
        // End-of-candidates marker.
        frame = "{\"type\":\"candidate\",\"candidate\":\"\"";
    } else {
        std::string sdp;
        if (!candidate->ToString(&sdp)) return;
        frame = "{\"type\":\"candidate\",\"candidate\":\"" + JsonEscape(sdp) + "\"";
        frame += ",\"sdpMid\":\"" + JsonEscape(candidate->sdp_mid()) + "\"";
        frame += ",\"sdpMLineIndex\":" + std::to_string(candidate->sdp_mline_index());
    }
    if (!peer_id.empty()) frame += ",\"peer_id\":\"" + JsonEscape(peer_id) + "\"";
    frame += "}";
    std::lock_guard<std::mutex> lk(ws_mu_);
    if (ws_) ws_->SendText(frame);
    ump_info("[webrtc] ICE candidate sent (peer_id=%s)%s\n",
             peer_id.c_str(), candidate ? "" : " (end-of-candidates)");
}

void WebrtcTransportImpl::RemoveDeadPeers() {
    // Close peers whose connection went down (ICE failure, etc.). The observer
    // callback only marks connected=false; we must not destroy the PC from
    // inside that callback. Collect the PCs under the lock, release them into
    // local refs, then Close() outside the lock so the call stack never holds
    // peers_mu_ while invoking PC callbacks.
    std::vector<scoped_refptr<webrtc::PeerConnectionInterface>> to_close;
    {
        std::lock_guard<std::mutex> lk(peers_mu_);
        for (auto& p : peers_) {
            if (p->pc && !p->connected.load(std::memory_order_acquire)) {
                to_close.push_back(p->pc);
                p->pc = nullptr;
            }
        }
    }
    for (auto& pc : to_close) {
        if (pc) pc->Close();
    }

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
        {
            std::lock_guard<std::mutex> lk(ws_mu_);
            ws_ = &ws;
        }

        std::string msg;
        bool joined = false;
        while (!sig_quit_.load(std::memory_order_acquire)) {
            using Ev = WsSignalingClient::Event;
            Ev ev = ws.RecvText(10000, sig_quit_, msg);
            if (ev == Ev::kQuit) {
                std::lock_guard<std::mutex> lk(ws_mu_);
                ws_ = nullptr;
                return;
            }
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
                    scoped_refptr<webrtc::PeerConnectionInterface> to_close;
                    {
                        std::lock_guard<std::mutex> lk(peers_mu_);
                        to_close = peer->pc;
                        peer->pc = nullptr;
                    }
                    if (to_close) to_close->Close();
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
                    ump_info("[webrtc] answer sent to viewer (peer_id=%s), sdp_len=%zu\n",
                             peer->peer_id.c_str(), answer_sdp.size());
                }
            } else if (type == "bye") {
                std::string bye_peer;
                JsonGetString(msg, "peer_id", bye_peer);
                scoped_refptr<webrtc::PeerConnectionInterface> to_close;
                {
                    std::lock_guard<std::mutex> lk(peers_mu_);
                    for (auto& p : peers_) {
                        if (p->peer_id == bye_peer && p->pc) {
                            ump_info("[webrtc] viewer (peer_id=%s) left, closing\n",
                                     bye_peer.c_str());
                            p->connected.store(false, std::memory_order_release);
                            to_close = p->pc;
                            p->pc = nullptr;
                        }
                    }
                }
                if (to_close) to_close->Close();
            } else if (type == "candidate") {
                // Trickle ICE: apply a remote (browser) candidate to the
                // matching peer's PeerConnection.
                std::string cand, mid, peer_id;
                JsonGetString(msg, "candidate", cand);
                JsonGetString(msg, "sdpMid", mid);
                JsonGetString(msg, "peer_id", peer_id);
                int mline = 0;
                {
                    const std::string key = "\"sdpMLineIndex\"";
                    size_t pos = msg.find(key);
                    if (pos != std::string::npos) {
                        pos += key.size();
                        while (pos < msg.size() &&
                               (msg[pos] == ' ' || msg[pos] == ':'))
                            ++pos;
                        mline = std::atoi(msg.c_str() + pos);
                    }
                }
                if (cand.empty()) continue;  // end-of-candidates, no-op
                std::lock_guard<std::mutex> lk(peers_mu_);
                for (auto& p : peers_) {
                    if (p->peer_id == peer_id && p->pc) {
                        webrtc::SdpParseError error;
                        auto ic = webrtc::CreateIceCandidate(mid, mline, cand, &error);
                        if (ic) {
                            p->pc->AddIceCandidate(std::move(ic));
                            ump_info("[webrtc] ICE candidate added (peer_id=%s, mline=%d)\n",
                                     peer_id.c_str(), mline);
                        } else {
                            ump_warn("[webrtc] parse ICE candidate failed: %s\n",
                                     error.description.c_str());
                        }
                    }
                }
            }
        }
        {
            std::lock_guard<std::mutex> lk(ws_mu_);
            ws_ = nullptr;
        }
        RemoveDeadPeers();
        SleepInterruptible(std::chrono::seconds(1), sig_quit_);
    }
}

} // namespace wt

// =====================================================================
//  Public factory
// =====================================================================
std::unique_ptr<WebrtcTransport> WebrtcTransport::Create() {
    return std::make_unique<wt::WebrtcTransportImpl>();
}

} // namespace ecovacs
