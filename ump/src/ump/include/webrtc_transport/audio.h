#ifndef WEBRTC_TRANSPORT_AUDIO_H_
#define WEBRTC_TRANSPORT_AUDIO_H_

#include "webrtc_transport/webrtc_transport.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "api/audio/audio_device.h"
#include "api/audio/audio_device_defines.h"
#include "api/audio_codecs/audio_decoder.h"
#include "api/audio_codecs/audio_decoder_factory.h"
#include "api/audio_codecs/audio_encoder.h"
#include "api/audio_codecs/audio_encoder_factory.h"
#include "api/audio_codecs/audio_format.h"
#include "api/environment/environment.h"
#include "api/ref_count.h"
#include "api/scoped_refptr.h"
#include "rtc_base/buffer.h"

namespace ecovacs {
namespace wt {

// =====================================================================
//  External G711 encoder: injects pre-encoded G.711 (PCMA) frames into
//  libwebrtc's audio RTP pipeline.  Encode() ignores the input PCM and
//  returns a frame from the internal queue (fed by SendG711Frame).
// =====================================================================
class ExternalG711Encoder : public webrtc::AudioEncoder {
public:
    struct Registry {
        ExternalG711Encoder* active = nullptr;
    };
    explicit ExternalG711Encoder(std::shared_ptr<Registry> r, int payload_type);
    ~ExternalG711Encoder() override;

    // webrtc::AudioEncoder interface
    int SampleRateHz() const override;
    size_t NumChannels() const override;
    size_t Num10MsFramesInNextPacket() const override;
    size_t Max10MsFramesInAPacket() const override;
    int GetTargetBitrate() const override;
    void Reset() override;
    std::optional<std::pair<webrtc::TimeDelta, webrtc::TimeDelta>> GetFrameLengthRange() const override;

    // Called by the send loop to inject a pre-encoded G.711 frame.
    void FeedG711Frame(const uint8_t* data, size_t size);

protected:
    EncodedInfo EncodeImpl(uint32_t rtp_timestamp,
                           std::span<const int16_t> audio,
                           webrtc::Buffer* encoded) override;

private:
    std::shared_ptr<Registry> registry_;
    std::mutex q_mu_;
    std::queue<std::vector<uint8_t>> frame_q_;
    uint32_t last_rtp_ts_ = 0;
    int payload_type_ = 0;
};

class ExternalG711EncoderFactory : public webrtc::AudioEncoderFactory {
public:
    explicit ExternalG711EncoderFactory(std::shared_ptr<ExternalG711Encoder::Registry> r);
    std::vector<webrtc::AudioCodecSpec> GetSupportedEncoders() override;
    std::optional<webrtc::AudioCodecInfo> QueryAudioEncoder(const webrtc::SdpAudioFormat&) override;
    std::unique_ptr<webrtc::AudioEncoder> Create(
        const webrtc::Environment&, const webrtc::SdpAudioFormat&,
        Options) override;
    void AddRef() const override { ++ref_count_; }
    webrtc::RefCountReleaseStatus Release() const override {
        if (--ref_count_ == 0) { delete this; return webrtc::RefCountReleaseStatus::kDroppedLastRef; }
        return webrtc::RefCountReleaseStatus::kOtherRefsRemained;
    }
private:
    std::shared_ptr<ExternalG711Encoder::Registry> registry_;
    mutable std::atomic<int> ref_count_{0};
};

// =====================================================================
//  Fake ADM: produces silent 10ms PCM blocks at 8kHz to drive the
//  audio encode pipeline.  The real G.711 payload is injected via
//  ExternalG711Encoder (which ignores this silence).
// =====================================================================
class FakeAudioDeviceModule : public webrtc::AudioDeviceModule {
public:
    FakeAudioDeviceModule();
    ~FakeAudioDeviceModule() override;

    int32_t ActiveAudioLayer(AudioLayer* layer) const override { *layer = kDummyAudio; return 0; }
    int32_t RegisterAudioCallback(webrtc::AudioTransport* cb) override;
    int32_t Init() override;
    int32_t Terminate() override;
    bool Initialized() const override { return initialized_; }

    int16_t PlayoutDevices() override { return 1; }
    int16_t RecordingDevices() override { return 1; }
    int32_t PlayoutDeviceName(uint16_t, char*, char*) override { return 0; }
    int32_t RecordingDeviceName(uint16_t, char*, char*) override { return 0; }
    int32_t SetPlayoutDevice(uint16_t) override { return 0; }
    int32_t SetPlayoutDevice(WindowsDeviceType) override { return 0; }
    int32_t SetRecordingDevice(uint16_t) override { return 0; }
    int32_t SetRecordingDevice(WindowsDeviceType) override { return 0; }

    int32_t PlayoutIsAvailable(bool* available) override { *available = true; return 0; }
    int32_t InitPlayout() override { playout_init_ = true; return 0; }
    bool PlayoutIsInitialized() const override { return playout_init_; }
    int32_t RecordingIsAvailable(bool* available) override { *available = true; return 0; }
    int32_t InitRecording() override { rec_init_ = true; return 0; }
    bool RecordingIsInitialized() const override { return rec_init_; }

    int32_t StartPlayout() override { playing_ = true; return 0; }
    int32_t StopPlayout() override { playing_ = false; return 0; }
    bool Playing() const override { return playing_; }
    int32_t StartRecording() override;
    int32_t StopRecording() override;
    bool Recording() const override { return recording_; }

    int32_t InitSpeaker() override { return 0; }
    bool SpeakerIsInitialized() const override { return true; }
    int32_t InitMicrophone() override { return 0; }
    bool MicrophoneIsInitialized() const override { return true; }

    int32_t SpeakerVolumeIsAvailable(bool* a) override { *a = false; return 0; }
    int32_t SetSpeakerVolume(uint32_t) override { return -1; }
    int32_t SpeakerVolume(uint32_t* v) const override { *v = 0; return 0; }
    int32_t MaxSpeakerVolume(uint32_t* v) const override { *v = 0; return 0; }
    int32_t MinSpeakerVolume(uint32_t* v) const override { *v = 0; return 0; }
    int32_t MicrophoneVolumeIsAvailable(bool* a) override { *a = false; return 0; }
    int32_t SetMicrophoneVolume(uint32_t) override { return -1; }
    int32_t MicrophoneVolume(uint32_t* v) const override { *v = 0; return 0; }
    int32_t MaxMicrophoneVolume(uint32_t* v) const override { *v = 0; return 0; }
    int32_t MinMicrophoneVolume(uint32_t* v) const override { *v = 0; return 0; }
    int32_t SpeakerMuteIsAvailable(bool* a) override { *a = false; return 0; }
    int32_t SetSpeakerMute(bool) override { return -1; }
    int32_t SpeakerMute(bool* e) const override { *e = false; return 0; }
    int32_t MicrophoneMuteIsAvailable(bool* a) override { *a = false; return 0; }
    int32_t SetMicrophoneMute(bool) override { return -1; }
    int32_t MicrophoneMute(bool* e) const override { *e = false; return 0; }
    int32_t StereoPlayoutIsAvailable(bool* a) const override { *a = false; return 0; }
    int32_t SetStereoPlayout(bool) override { return -1; }
    int32_t StereoPlayout(bool* e) const override { *e = false; return 0; }
    int32_t StereoRecordingIsAvailable(bool* a) const override { *a = false; return 0; }
    int32_t SetStereoRecording(bool) override { return -1; }
    int32_t StereoRecording(bool* e) const override { *e = false; return 0; }
    int32_t PlayoutDelay(uint16_t* d) const override { *d = 0; return 0; }

    bool BuiltInAECIsAvailable() const override { return false; }
    bool BuiltInAGCIsAvailable() const override { return false; }
    bool BuiltInNSIsAvailable() const override { return false; }
    int32_t EnableBuiltInAEC(bool) override { return 0; }
    int32_t EnableBuiltInAGC(bool) override { return 0; }
    int32_t EnableBuiltInNS(bool) override { return 0; }

    void AddRef() const override { ++ref_count_; }
    webrtc::RefCountReleaseStatus Release() const override {
        if (--ref_count_ == 0) { delete this; return webrtc::RefCountReleaseStatus::kDroppedLastRef; }
        return webrtc::RefCountReleaseStatus::kOtherRefsRemained;
    }

private:
    void CaptureThread();
    webrtc::AudioTransport* audio_cb_ = nullptr;
    std::thread cap_thread_;
    std::atomic<bool> recording_{false};
    bool initialized_ = false;
    bool playout_init_ = false;
    bool rec_init_ = false;
    bool playing_ = false;
    mutable std::atomic<int> ref_count_{0};
};

// =====================================================================
//  Empty audio encoder/decoder factories: used when audio is disabled
//  or when we don't yet support a given direction.
// =====================================================================
class EmptyAudioEncoderFactory : public webrtc::AudioEncoderFactory {
public:
    std::vector<webrtc::AudioCodecSpec> GetSupportedEncoders() override { return {}; }
    std::optional<webrtc::AudioCodecInfo> QueryAudioEncoder(const webrtc::SdpAudioFormat&) override { return std::nullopt; }
    std::unique_ptr<webrtc::AudioEncoder> Create(
        const webrtc::Environment&, const webrtc::SdpAudioFormat&,
        Options) override { return nullptr; }
    void AddRef() const override { ++ref_count_; }
    webrtc::RefCountReleaseStatus Release() const override {
        if (--ref_count_ == 0) { delete this; return webrtc::RefCountReleaseStatus::kDroppedLastRef; }
        return webrtc::RefCountReleaseStatus::kOtherRefsRemained;
    }
private:
    mutable std::atomic<int> ref_count_{0};
};

class EmptyAudioDecoderFactory : public webrtc::AudioDecoderFactory {
public:
    std::vector<webrtc::AudioCodecSpec> GetSupportedDecoders() override { return {}; }
    bool IsSupportedDecoder(const webrtc::SdpAudioFormat&) override { return false; }
    std::unique_ptr<webrtc::AudioDecoder> Create(
        const webrtc::Environment&, const webrtc::SdpAudioFormat&,
        std::optional<webrtc::AudioCodecPairId>) override { return nullptr; }
    void AddRef() const override { ++ref_count_; }
    webrtc::RefCountReleaseStatus Release() const override {
        if (--ref_count_ == 0) { delete this; return webrtc::RefCountReleaseStatus::kDroppedLastRef; }
        return webrtc::RefCountReleaseStatus::kOtherRefsRemained;
    }
private:
    mutable std::atomic<int> ref_count_{0};
};

} // namespace wt
} // namespace ecovacs

#endif // WEBRTC_TRANSPORT_AUDIO_H_
