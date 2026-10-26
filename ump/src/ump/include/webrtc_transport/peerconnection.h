#ifndef WEBRTC_TRANSPORT_PEERCONNECTION_H_
#define WEBRTC_TRANSPORT_PEERCONNECTION_H_

#include "webrtc_transport/video.h"
#include "webrtc_transport/audio.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "api/audio_codecs/audio_encoder_factory.h"
#include "api/create_peerconnection_factory.h"
#include "api/jsep.h"
#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"
#include "api/set_remote_description_observer_interface.h"
#include "rtc_base/thread.h"

namespace ecovacs {
namespace wt {

using webrtc::scoped_refptr;

// =====================================================================
//  PeerConnection observer
// =====================================================================
class PushingObserver : public webrtc::PeerConnectionObserver {
public:
    std::function<void(bool)> on_connection_edge;
    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState s) override;
    void OnDataChannel(scoped_refptr<webrtc::DataChannelInterface>) override {}
    void OnIceCandidate(const webrtc::IceCandidate*) override {}
    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState s) override;
    bool WaitIceComplete(std::chrono::milliseconds timeout, const std::atomic<bool>& quit);
    void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState s) override;
private:
    std::mutex ice_mu_;
    std::condition_variable ice_cv_;
    bool ice_complete_ = false;
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
    void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override { CompleteOp(std::move(error)); }
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
    void SetAudioReceiver(IWebrtcAudioReceiver* r) override;  // TODO: not implemented
    bool Started() const override;

    int32_t Start() override;
    int32_t Stop() override;
    void SendH264Frame(const uint8_t* data, size_t size, bool keyframe) override;
    void SendG711Frame(const uint8_t* data, size_t size) override;

private:
    struct FrameHolder {
        uint8_t* data = nullptr;
        size_t size = 0;
        bool keyframe = false;
        void RelRef() { delete[] data; data = nullptr; }
        ~FrameHolder() { delete[] data; }
    };

    struct PeerContext {
        int id = 0;
        std::string peer_id;
        scoped_refptr<webrtc::PeerConnectionFactoryInterface> owned_factory;
        scoped_refptr<webrtc::PeerConnectionInterface> pc;
        scoped_refptr<webrtc::VideoTrackInterface> track;
        scoped_refptr<webrtc::AudioTrackInterface> audio_track;
        std::unique_ptr<PushingObserver> observer;
        std::shared_ptr<ExternalH264Encoder::Registry> registry;
        std::shared_ptr<ExternalG711Encoder::Registry> audio_registry;
        std::atomic<bool> connected{false};
        bool need_keyframe = true;
    };

    bool CreateFactory();
    void DestroyFactory();
    std::shared_ptr<PeerContext> CreatePeer(const std::string& peer_id);
    std::string HandleOffer(std::shared_ptr<PeerContext> peer, const std::string& offer_sdp);
    void RemoveDeadPeers();
    void RunSignaling();
    void SendLoop();

    WebrtcTransportConfig config_;
    IWebrtcVideoEncoder* video_encoder_ = nullptr;
    IWebrtcViewerListener* viewer_listener_ = nullptr;
    IWebrtcAudioReceiver* audio_receiver_ = nullptr;  // TODO: not wired to libwebrtc yet
    std::atomic<bool> started_{false}, quit_{false}, sig_quit_{false};
    std::thread send_thread_, mailbox_thread_;
    std::unique_ptr<webrtc::Thread> network_thread_, signaling_thread_;
    scoped_refptr<webrtc::VideoTrackSourceInterface> track_source_;
    std::shared_ptr<BitrateAggregator> aggregator_;
    std::atomic<int> next_viewer_id_{0};
    uint64_t start_time_us_ = 0;
    std::mutex q_mu_;
    std::queue<std::shared_ptr<FrameHolder>> frame_q_;
    std::mutex peers_mu_;
    std::vector<std::shared_ptr<PeerContext>> peers_;
};

} // namespace wt
} // namespace ecovacs

#endif // WEBRTC_TRANSPORT_PEERCONNECTION_H_
