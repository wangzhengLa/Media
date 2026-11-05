#ifndef WEBRTC_TRANSPORT_PEERCONNECTION_H_
#define WEBRTC_TRANSPORT_PEERCONNECTION_H_

#include <log.h>
#include "webrtc_transport/video.h"
#include "webrtc_transport/audio.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "api/audio/audio_mixer.h"
#include "api/audio/audio_frame.h"
#include "modules/audio_mixer/audio_mixer_impl.h"
#include "api/audio_codecs/audio_encoder_factory.h"
#include "api/create_peerconnection_factory.h"
#include "api/jsep.h"
#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"
#include "api/set_remote_description_observer_interface.h"
#include "rtc_base/thread.h"

#include <unordered_map>

namespace ecovacs {
class WsSignalingClient;  // forward decl
namespace wt {

// Forward declarations for receive-side sinks (browser -> device).
class RemoteAudioSink;       // receives decoded PCM via AudioSinkInterface
class RemoteVideoEncodedSink; // receives encoded H.264 via EncodedImageCallback

using webrtc::scoped_refptr;

// =====================================================================
//  PeerConnection observer
// =====================================================================
class PushingObserver : public webrtc::PeerConnectionObserver {
public:
    std::function<void(bool)> on_connection_edge;
    std::function<void(const webrtc::IceCandidate*)> on_ice_candidate;
    std::function<void(scoped_refptr<webrtc::RtpTransceiverInterface>)> on_track;
    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState s) override;
    void OnDataChannel(scoped_refptr<webrtc::DataChannelInterface>) override {}
    void OnIceCandidate(const webrtc::IceCandidate* c) override {
        if (on_ice_candidate) on_ice_candidate(c);
    }
    void OnTrack(scoped_refptr<webrtc::RtpTransceiverInterface> t) override {
        if (on_track) on_track(t);
    }
    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState s) override;
    void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState s) override;
    void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState s) override;
private:
    bool connected_ = false;
};

// =====================================================================
//  SDP promise observers (sync wrappers for async SDP operations)
// =====================================================================
class PromiseCreateSdpObserver : public webrtc::CreateSessionDescriptionObserver {
public:
    void OnSuccess(webrtc::SessionDescriptionInterface* d) override;
    void OnFailure(webrtc::RTCError e) override;
    bool Wait(const std::atomic<bool>& quit);
    std::unique_ptr<webrtc::SessionDescriptionInterface> TakeDesc();
    const std::string& error() const { return err_; }
    void AddRef() const override { ++refs_; }
    webrtc::RefCountReleaseStatus Release() const override {
        if (--refs_ == 0) { delete this; return webrtc::RefCountReleaseStatus::kDroppedLastRef; }
        return webrtc::RefCountReleaseStatus::kOtherRefsRemained;
    }
private:
    ~PromiseCreateSdpObserver() override = default;
    std::mutex mu_;
    std::condition_variable cv_;
    bool done_ = false;
    std::string err_;
    std::unique_ptr<webrtc::SessionDescriptionInterface> desc_;
    mutable std::atomic<int> refs_{0};
};

template<typename Iface>
class PromiseSetSdpObserverT : public Iface {
public:
    void CompleteOp(webrtc::RTCError e) {
        std::lock_guard<std::mutex> g(mu_); done_ = true; ok_ = e.ok();
        if (!ok_) err_ = e.message(); cv_.notify_all();
    }
    bool Wait(const std::atomic<bool>& quit) {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait_for(lk, std::chrono::seconds(10), [&] { return done_ || quit.load(std::memory_order_acquire); });
        return ok_;
    }
    const std::string& error() const { return err_; }
    void AddRef() const override { ++refs_; }
    webrtc::RefCountReleaseStatus Release() const override {
        if (--refs_ == 0) { delete this; return webrtc::RefCountReleaseStatus::kDroppedLastRef; }
        return webrtc::RefCountReleaseStatus::kOtherRefsRemained;
    }
protected:
    ~PromiseSetSdpObserverT() override = default;
private:
    std::mutex mu_;
    std::condition_variable cv_;
    bool done_ = false, ok_ = false;
    std::string err_;
    mutable std::atomic<int> refs_{0};
};

class PromiseSetLocalObserver
    : public PromiseSetSdpObserverT<webrtc::SetLocalDescriptionObserverInterface> {
public:
    void OnSetLocalDescriptionComplete(webrtc::RTCError error) override { CompleteOp(std::move(error)); }
};

class PromiseSetRemoteObserver
    : public PromiseSetSdpObserverT<webrtc::SetRemoteDescriptionObserverInterface> {
public:
    void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
        ump_info("[webrtc] SetRemoteDescription complete, ok=%d err=%s\n",
                 error.ok() ? 1 : 0, error.message());
        CompleteOp(std::move(error));
    }
};

// =====================================================================
//  WebrtcTransportImpl — multi-viewer master (callee) transport
// =====================================================================
class WebrtcTransportImpl : public WebrtcTransport {
public:
    WebrtcTransportImpl() = default;
    ~WebrtcTransportImpl() override;

    int32_t Init(const WebrtcTransportConfig& c) override;
    void SetVideoEncoder(IWebrtcVideoEncoder* e) override;
    void SetViewerListener(IWebrtcViewerListener* l) override;
    void SetAudioReceiver(IWebrtcAudioReceiver* r) override;  // receive-side audio (browser -> device)
    void SetVideoReceiver(IWebrtcVideoReceiver* r) override;  // receive-side video (browser -> device)
    bool Started() const override;

    int32_t Start() override;
    int32_t Stop() override;
    void SendVideoFrame(const uint8_t* data, size_t size, bool keyframe) override;
    void SendAudioFrame(const uint8_t* data, size_t size) override;

private:
    struct PeerContext {
        std::string peer_id;
        scoped_refptr<webrtc::PeerConnectionFactoryInterface> owned_factory;
        scoped_refptr<webrtc::PeerConnectionInterface> pc;
        scoped_refptr<webrtc::VideoTrackInterface> video_track;
        scoped_refptr<webrtc::AudioTrackInterface> audio_track;
        std::unique_ptr<PushingObserver> observer;
        std::shared_ptr<ExternalVideoEncoder::Registry> video_registry;
        std::shared_ptr<ExternalAudioEncoder::Registry> audio_registry;
        // Receive-side sinks (browser -> device).  Created when OnTrack
        // fires for remote audio/video tracks.
        // Audio sink doubles as an AudioMixer::Source so all viewers'
        // voices get mixed before being delivered to the audio receiver.
        std::unique_ptr<RemoteAudioSink> remote_audio_sink;
        std::unique_ptr<RemoteVideoEncodedSink> remote_video_encoded_sink;
        scoped_refptr<webrtc::AudioTrackInterface> remote_audio_track;
        scoped_refptr<webrtc::VideoTrackSourceInterface> remote_video_source;
        std::atomic<bool> connected{false};
        std::atomic<bool> need_keyframe{true};
        // Guards RemoveSource: only call AudioMixer::RemoveSource once
        // per source, and only if AddSource succeeded.
        std::atomic<bool> audio_source_added{false};
        std::atomic<bool> video_sink_added{false};
        std::atomic<bool> audio_sink_added{false};
    };

    bool CreateFactory();
    void DestroyFactory();
    std::shared_ptr<PeerContext> CreatePeer(const std::string& peer_id);
    std::string HandleOffer(std::shared_ptr<PeerContext> peer, const std::string& offer_sdp);
    void RemoveDeadPeers();
    void RunSignaling();
    // Unregister all sinks/sources for a peer.  Safe to call multiple times:
    // each removal is guarded by an atomic flag so it happens exactly once.
    // Must be called WITHOUT holding peers_mu_ because it invokes libwebrtc
    // APIs (RemoveSink / RemoveSource / RemoveEncodedSink).
    void DetachPeer(const std::shared_ptr<PeerContext>& p);
    // Called from PushingObserver::OnIceCandidate (signaling thread) to push
    // a trickle-ICE candidate out over the signaling WebSocket.
    void SendIceCandidate(const std::string& peer_id,
                          const webrtc::IceCandidate* candidate);

    WebrtcTransportConfig config_;
    IWebrtcVideoEncoder* video_encoder_ = nullptr;
    IWebrtcViewerListener* viewer_listener_ = nullptr;
    IWebrtcAudioReceiver* audio_receiver_ = nullptr;
    IWebrtcVideoReceiver* video_receiver_ = nullptr;
    std::atomic<bool> started_{false}, quit_{false}, sig_quit_{false};
    std::thread mailbox_thread_;
    std::unique_ptr<webrtc::Thread> network_thread_, signaling_thread_;
    scoped_refptr<webrtc::VideoTrackSourceInterface> track_source_;
    std::shared_ptr<VideoRateController> rate_controller_;
    std::mutex peers_mu_;
    std::vector<std::shared_ptr<PeerContext>> peers_;
    // Signaling WebSocket handle, owned by RunSignaling's loop.  Set under
    // ws_mu_ so SendIceCandidate (called from libwebrtc's signaling thread)
    // can safely send without racing a reconnect.
    std::mutex ws_mu_;
    WsSignalingClient* ws_ = nullptr;

    // ---- Receive-side audio mixer (browser -> device) ----
    // Mixes PCM from all remote audio tracks and delivers the mixed stream
    // to audio_receiver_->OnAudioFrame().  Owns one AudioMixer::Source per
    // viewer (RemoteAudioSink).
    scoped_refptr<webrtc::AudioMixer> audio_mixer_;
    std::thread mix_thread_;
    void MixThread();
};

} // namespace wt
} // namespace ecovacs

#endif // WEBRTC_TRANSPORT_PEERCONNECTION_H_
