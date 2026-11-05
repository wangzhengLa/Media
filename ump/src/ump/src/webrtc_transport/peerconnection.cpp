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
//  NullVideoDecoderFactory — no real decoding, we only consume encoded
//  frames via AddEncodedSink. A non-null factory is required so that
//  SetRemoteDescription does not crash when the offer carries a video
//  m-line (libwebrtc dereferences the factory during SDP negotiation).
//  We advertise H264 support so SDP negotiation succeeds; the decoder
//  itself is never created because encoded frames are consumed raw.
// =====================================================================
class NullVideoDecoderFactory : public webrtc::VideoDecoderFactory {
public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        // Match what the browser offers (H264 baseline/constrained baseline).
        return {
            webrtc::SdpVideoFormat("H264", {
                {"level-asymmetry-allowed", "1"},
                {"packetization-mode", "1"},
                {"profile-level-id", "42e01f"},
            }),
        };
    }
    std::unique_ptr<webrtc::VideoDecoder> Create(
        const webrtc::Environment& /*env*/,
        const webrtc::SdpVideoFormat& /*format*/) override {
        return nullptr;
    }
};

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
    bool connected = (s == PCState::kConnected);
    bool down = (s == PCState::kFailed || s == PCState::kClosed);
    if (connected && !connected_) {
        connected_ = true;
        if (on_connection_edge) on_connection_edge(true);
    } else if (down) {
        // Always notify on terminal states, even if we never reached
        // kConnected. Otherwise the audio source is never removed from
        // the mixer and Mix() dereferences a freed sink.
        connected_ = false;
        if (on_connection_edge) on_connection_edge(false);
    }
}

// =====================================================================
//  Receive-side sinks (browser -> device)
// =====================================================================

// Delivers decoded PCM from a remote audio track to the mixer.  Acts both
// as an AudioTrackSinkInterface (receives PCM from libwebrtc) and as an
// AudioMixer::Source (feeds the mixer).  PCM is buffered so the mixer can
// pull 10ms frames on its own cadence.
class RemoteAudioSink : public webrtc::AudioTrackSinkInterface,
                        public webrtc::AudioMixer::Source {
public:
    static constexpr int kSampleRate = 48000;  // match audio module outSampleRate
    static constexpr int kChannels = 1;
    static constexpr int kFrameMs = 10;
    static constexpr int kFrameSamples = kSampleRate * kFrameMs / 1000;

    explicit RemoteAudioSink(std::string peer_id)
        : peer_id_(std::move(peer_id)) {
        buf_.resize(kFrameSamples * 4, 0);  // 40ms buffer
    }

    // AudioTrackSinkInterface: called by libwebrtc with decoded PCM.
    void OnData(const void* audio_data, int bits_per_sample,
                int sample_rate, size_t number_of_channels,
                size_t number_of_frames,
                std::optional<int64_t> /*absolute_capture_timestamp_ms*/,
                const webrtc::RtpPacketInfos& /*packet_infos*/) override {
        if (!audio_data || bits_per_sample != 16 || sample_rate <= 0) return;
        const int16_t* src = static_cast<const int16_t*>(audio_data);
        size_t target_frames =
            (size_t)((double)number_of_frames * kSampleRate / sample_rate);
        if (target_frames == 0) return;

        std::lock_guard<std::mutex> lk(mu_);
        // Append after existing data; shift down if the buffer would overflow.
        if (write_pos_ + target_frames > buf_.size()) {
            size_t keep = write_pos_;
            if (keep > buf_.size() - target_frames) keep = buf_.size() - target_frames;
            if (keep > 0)
                memmove(buf_.data(), buf_.data() + (write_pos_ - keep),
                        keep * sizeof(int16_t));
            write_pos_ = keep;
        }
        for (size_t i = 0; i < target_frames; i++) {
            // Nearest-neighbour index into the source (original rate).
            size_t src_idx = (size_t)((double)i * sample_rate / kSampleRate);
            if (src_idx >= number_of_frames) src_idx = number_of_frames - 1;
            int32_t s = 0;
            for (size_t c = 0; c < number_of_channels; c++)
                s += src[src_idx * number_of_channels + c];
            buf_[write_pos_ + i] = (int16_t)(s / (int)number_of_channels);
        }
        write_pos_ += target_frames;
    }

    // AudioMixer::Source: pull 10ms of mixed PCM.
    AudioFrameInfo GetAudioFrameWithInfo(int sample_rate_hz,
                                         webrtc::AudioFrame* frame) override {
        frame->sample_rate_hz_ = kSampleRate;
        frame->num_channels_ = kChannels;
        frame->samples_per_channel_ = kFrameSamples;
        std::lock_guard<std::mutex> lk(mu_);
        AudioFrameInfo info;
        if (write_pos_ < kFrameSamples) {
            frame->Mute();
            info = AudioFrameInfo::kMuted;
        } else {
            memcpy(frame->mutable_data(), buf_.data(),
                   kFrameSamples * sizeof(int16_t));
            size_t remain = write_pos_ - kFrameSamples;
            if (remain > 0)
                memmove(buf_.data(), buf_.data() + kFrameSamples,
                        remain * sizeof(int16_t));
            write_pos_ = remain;
            info = AudioFrameInfo::kNormal;
        }
        return info;
    }

    int Ssrc() const override { return ssrc_; }
    int PreferredSampleRate() const override { return kSampleRate; }

    void SetSsrc(uint32_t ssrc) { ssrc_ = ssrc; }
    const std::string& peer_id() const { return peer_id_; }

private:
    std::string peer_id_;
    uint32_t ssrc_ = 0;
    std::mutex mu_;
    std::vector<int16_t> buf_;
    size_t write_pos_ = 0;
};

// Delivers encoded H.264 frames from a remote video track to
// IWebrtcVideoReceiver.  Implements VideoSinkInterface<RecordableEncodedFrame>
// so we get the compressed bitstream (not decoded frames).
class RemoteVideoEncodedSink
    : public webrtc::VideoSinkInterface<webrtc::RecordableEncodedFrame> {
public:
    RemoteVideoEncodedSink(std::string peer_id, IWebrtcVideoReceiver* receiver)
        : peer_id_(std::move(peer_id)), receiver_(receiver) {}

    void OnFrame(const webrtc::RecordableEncodedFrame& frame) override {
        auto buf = frame.encoded_buffer();
        if (!receiver_ || !buf) return;
        const uint8_t* data = buf->data();
        size_t size = buf->size();
        if (!data || size == 0) return;
        receiver_->OnVideoEncodedFrame(peer_id_, data, size,
            frame.is_key_frame(), frame.render_time().us());
    }

private:
    std::string peer_id_;
    IWebrtcVideoReceiver* receiver_;
};

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
    if (rate_controller_) rate_controller_->SetVideoEncoder(e);
}

void WebrtcTransportImpl::SetViewerListener(IWebrtcViewerListener* l) { viewer_listener_ = l; }

void WebrtcTransportImpl::SetAudioReceiver(IWebrtcAudioReceiver* r) {
    audio_receiver_ = r;
}

void WebrtcTransportImpl::SetVideoReceiver(IWebrtcVideoReceiver* r) {
    video_receiver_ = r;
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
    mix_thread_ = std::thread([this] { MixThread(); });
    return 0;
}

int32_t WebrtcTransportImpl::Stop() {
    bool expected = true;
    if (!started_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) return 0;
    quit_.store(true); sig_quit_.store(true);
    if (mix_thread_.joinable()) mix_thread_.join();
    if (mailbox_thread_.joinable()) mailbox_thread_.join();
    DestroyFactory();
    return 0;
}

// MixThread: pulls 10ms frames from AudioMixer and delivers mixed PCM to
// the audio receiver (which forwards it to the audio module for playback).
void WebrtcTransportImpl::MixThread() {
    ump_info("[webrtc] mix thread started\n");
    using namespace std::chrono;
    auto next = steady_clock::now();
    while (!quit_.load(std::memory_order_acquire)) {
        webrtc::AudioFrame frame;
        if (audio_mixer_)
            audio_mixer_->Mix(RemoteAudioSink::kChannels, &frame);
        if (audio_receiver_ && frame.samples_per_channel_ > 0) {
            const int16_t* pcm = reinterpret_cast<const int16_t*>(frame.data());
            audio_receiver_->OnAudioFrame("mixed", pcm,
                frame.samples_per_channel_,
                frame.sample_rate_hz_,
                frame.num_channels_);
        }
        next += milliseconds(RemoteAudioSink::kFrameMs);
        std::this_thread::sleep_until(next);
    }
    ump_info("[webrtc] mix thread exited\n");
}

void WebrtcTransportImpl::SendVideoFrame(const uint8_t* data, size_t size, bool keyframe) {
    if (!started_.load(std::memory_order_acquire) || !data || size == 0) return;
    std::vector<std::shared_ptr<PeerContext>> snapshot;
    { std::lock_guard<std::mutex> lk(peers_mu_); snapshot = peers_; }
    for (auto& peer : snapshot) {
        if (!peer->pc || !peer->connected.load(std::memory_order_acquire)) continue;
        if (peer->need_keyframe) {
            if (!keyframe) continue;
            peer->need_keyframe = false;
            ump_info("[webrtc] viewer (peer_id=%s) keyframe found, start sending\n",
                     peer->peer_id.c_str());
        }
        ExternalVideoEncoder* enc = peer->video_registry ? peer->video_registry->active : nullptr;
        if (enc) enc->InjectVideoFrame(data, size, keyframe);
    }
}

void WebrtcTransportImpl::SendAudioFrame(const uint8_t* data, size_t size) {
    if (!started_.load(std::memory_order_acquire) || !data || size == 0) return;

    // G.711 PCMA @ 8kHz mono, 10ms per packet = 80 bytes.
    // ExternalAudioEncoder declares GetFrameLengthRange()=10ms and
    // Num10MsFramesInNextPacket()=1, so libwebrtc expects exactly 80 bytes
    // per RTP packet (timestamp +80). Any other size will cause the receiver
    // to misinterpret frame duration -> glitches / speed changes.
    static constexpr size_t kExpectedG711FrameSize = 80;
    if (size != kExpectedG711FrameSize) {
        static uint32_t bad_cnt = 0;
        if (++bad_cnt % 50 == 0) {
            ump_warn("[webrtc] audio frame size %zu != expected %zu (G.711 10ms@8kHz) #%u\n",
                     size, kExpectedG711FrameSize, bad_cnt);
        }
    }

    std::vector<std::shared_ptr<PeerContext>> snapshot;
    { std::lock_guard<std::mutex> lk(peers_mu_); snapshot = peers_; }
    for (auto& peer : snapshot) {
        if (!peer->pc || !peer->connected.load(std::memory_order_acquire)) continue;
        ExternalAudioEncoder* enc = peer->audio_registry ? peer->audio_registry->active : nullptr;
        if (enc) enc->FeedAudioFrame(data, size);
    }
}

bool WebrtcTransportImpl::CreateFactory() {
    network_thread_ = webrtc::Thread::CreateWithSocketServer();
    signaling_thread_ = webrtc::Thread::Create();
    network_thread_->Start(); signaling_thread_->Start();
    track_source_ = webrtc::make_ref_counted<ExternalVideoTrackSource>(
        config_.video_width, config_.video_height, config_.video_fps);
    rate_controller_ = std::make_shared<VideoRateController>();
    if (video_encoder_) rate_controller_->SetVideoEncoder(video_encoder_);
    // Mixer for receive-side audio (browser -> device).
    audio_mixer_ = webrtc::AudioMixerImpl::Create();
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
    // Unregister all sinks/sources before destroying PeerContexts and the
    // mixer.  DetachPeer is idempotent (atomic flags).
    for (auto& p : snapshot) {
        DetachPeer(p);
    }

    // Close callbacks (if any) have already run. Now drop all references
    // under the lock. It is harmless if a callback already nulled p->pc.
    {
        std::lock_guard<std::mutex> lk(peers_mu_);
        for (auto& p : peers_) {
            p->video_track = nullptr;
            p->audio_track = nullptr;
            p->remote_audio_track = nullptr;
            p->remote_video_source = nullptr;
            p->pc = nullptr;
            p->owned_factory = nullptr;
        }
        peers_.clear();
    }
    track_source_ = nullptr;
    rate_controller_ = nullptr;
    audio_mixer_ = nullptr;
    if (network_thread_) { network_thread_->Stop(); network_thread_.reset(); }
    if (signaling_thread_) { signaling_thread_->Stop(); signaling_thread_.reset(); }
}

std::shared_ptr<WebrtcTransportImpl::PeerContext> WebrtcTransportImpl::CreatePeer(const std::string& peer_id) {
    auto peer = std::make_shared<PeerContext>();
    peer->peer_id = peer_id;
    peer->video_registry = std::make_shared<ExternalVideoEncoder::Registry>();
    peer->video_registry->video_encoder = video_encoder_;
    peer->video_registry->rate_controller = rate_controller_;
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
            // Do NOT call RemoveSource / RemoveSink here.  This callback may
            // fire multiple times (kFailed then kClosed during Close()), and
            // RemoveSource on a non-present source is UB.  Actual unregistration
            // is done by RemoveDeadPeers -> DetachPeer, guarded by atomic flags.
        }
    };
    peer->observer->on_ice_candidate = [this, weak](const webrtc::IceCandidate* c) {
        auto p = weak.lock();
        if (!p) return;
        SendIceCandidate(p->peer_id, c);
    };
    // Receive-side: bind remote audio/video tracks to our sinks.
    peer->observer->on_track = [this, weak](scoped_refptr<webrtc::RtpTransceiverInterface> tr) {
        auto p = weak.lock();
        if (!p) return;
        if (!tr) return;
        auto track = tr->receiver()->track();
        if (!track) return;
        if (track->kind() == "audio") {
            auto at = static_cast<webrtc::AudioTrackInterface*>(track.get());
            p->remote_audio_track = at;
            p->remote_audio_sink = std::make_unique<RemoteAudioSink>(p->peer_id);
            at->AddSink(p->remote_audio_sink.get());
            p->audio_sink_added.store(true, std::memory_order_release);
            if (audio_mixer_) {
                audio_mixer_->AddSource(p->remote_audio_sink.get());
                p->audio_source_added.store(true, std::memory_order_release);
            }
            ump_info("[webrtc] remote audio track bound for peer=%s\n",
                     p->peer_id.c_str());
        } else if (track->kind() == "video") {
            auto vt = static_cast<webrtc::VideoTrackInterface*>(track.get());
            p->remote_video_encoded_sink = std::make_unique<RemoteVideoEncodedSink>(
                p->peer_id, video_receiver_);
            auto* src = vt->GetSource();
            if (src) {
                p->remote_video_source = src;
                src->AddEncodedSink(p->remote_video_encoded_sink.get());
                p->video_sink_added.store(true, std::memory_order_release);
            }
            ump_info("[webrtc] remote video track bound for peer=%s\n",
                     p->peer_id.c_str());
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
    // Builtin video decoder factory is required for receiving browser video.
    // A nullptr here crashes inside SetRemoteDescription when the offer
    // contains a sendrecv/sendonly video m-line. We only consume encoded
    // frames via AddEncodedSink, so a null (no-op) factory is sufficient.
    auto video_dec_factory = std::make_unique<NullVideoDecoderFactory>();
    peer->owned_factory = webrtc::CreatePeerConnectionFactory(
        network_thread_.get(), signaling_thread_.get(),
        adm,
        audio_enc_factory,
        audio_dec_factory,
        std::move(enc_factory),
        std::move(video_dec_factory),
        nullptr, nullptr);
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

void WebrtcTransportImpl::DetachPeer(const std::shared_ptr<PeerContext>& p) {
    if (!p) return;
    // Audio mixer source — only remove if AddSource succeeded, and only once.
    if (p->audio_source_added.exchange(false, std::memory_order_acq_rel)) {
        if (audio_mixer_ && p->remote_audio_sink)
            audio_mixer_->RemoveSource(p->remote_audio_sink.get());
    }
    // Audio sink from the remote audio track.
    if (p->audio_sink_added.exchange(false, std::memory_order_acq_rel)) {
        if (p->remote_audio_track && p->remote_audio_sink)
            p->remote_audio_track->RemoveSink(p->remote_audio_sink.get());
    }
    // Video encoded sink from the remote video track source.
    if (p->video_sink_added.exchange(false, std::memory_order_acq_rel)) {
        if (p->remote_video_source && p->remote_video_encoded_sink)
            p->remote_video_source->RemoveEncodedSink(p->remote_video_encoded_sink.get());
    }
}

void WebrtcTransportImpl::RemoveDeadPeers() {
    // 1. Collect dead peers.  Two cases:
    //    a) p->pc still set but !connected — need Close() + detach + erase.
    //    b) p->pc already null (set by bye / HandleOffer failure) — just
    //       detach + erase (Close already done).
    std::vector<std::shared_ptr<PeerContext>> to_detach;
    std::vector<scoped_refptr<webrtc::PeerConnectionInterface>> to_close;
    {
        std::lock_guard<std::mutex> lk(peers_mu_);
        for (auto& p : peers_) {
            if (!p->pc) {
                // Case (b): already closed, just detach+erase.
                to_detach.push_back(p);
            } else if (!p->connected.load(std::memory_order_acquire)) {
                // Case (a): still alive but disconnected.
                to_close.push_back(p->pc);
                to_detach.push_back(p);
                p->pc = nullptr;
            }
        }
    }

    // 2. Close() the PeerConnections OUTSIDE the lock.
    for (auto& pc : to_close) {
        if (pc) pc->Close();
    }
    to_close.clear();

    // 3. Detach sinks/sources.  Also runs outside the lock.
    for (auto& p : to_detach) {
        DetachPeer(p);
    }

    // 4. Erase detached peers.
    {
        std::lock_guard<std::mutex> lk(peers_mu_);
        peers_.erase(
            std::remove_if(peers_.begin(), peers_.end(),
                [](const std::shared_ptr<PeerContext>& p) { return p->pc == nullptr; }),
            peers_.end());
    }
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
