#include "webrtc_transport/audio.h"

#include <log.h>

#include <cstring>
#include <chrono>

namespace ecovacs {
namespace wt {

// =====================================================================
//  ExternalAudioEncoder
//  Audio encode is pull-based: libwebrtc calls EncodeImpl() every
//  10ms (or per packet) with PCM input.  We ignore that input and
//  return a pre-encoded audio frame from our queue.
//  Current codec: G.711 PCMA (8kHz mono, 10ms per packet).
// =====================================================================
ExternalAudioEncoder::ExternalAudioEncoder(std::shared_ptr<Registry> r, int payload_type)
    : registry_(r), payload_type_(payload_type) {}
ExternalAudioEncoder::~ExternalAudioEncoder() {
    if (registry_ && registry_->active == this) registry_->active = nullptr;
}

int ExternalAudioEncoder::SampleRateHz() const { return 8000; }
size_t ExternalAudioEncoder::NumChannels() const { return 1; }
size_t ExternalAudioEncoder::Num10MsFramesInNextPacket() const { return 1; } // 10ms per packet
size_t ExternalAudioEncoder::Max10MsFramesInAPacket() const { return 1; }
int ExternalAudioEncoder::GetTargetBitrate() const { return 64000; }

void ExternalAudioEncoder::Reset() {
    std::lock_guard<std::mutex> lk(q_mu_);
    while (!frame_q_.empty()) frame_q_.pop();
    last_rtp_ts_ = 0;
}

std::optional<std::pair<webrtc::TimeDelta, webrtc::TimeDelta>>
ExternalAudioEncoder::GetFrameLengthRange() const {
    return std::make_pair(webrtc::TimeDelta::Millis(10), webrtc::TimeDelta::Millis(10));
}

void ExternalAudioEncoder::FeedAudioFrame(const uint8_t* data, size_t size) {
    if (!data || size == 0) return;
    std::lock_guard<std::mutex> lk(q_mu_);
    if (frame_q_.size() > 200) {
        frame_q_.pop(); // drop oldest to bound memory
    }
    frame_q_.emplace(data, data + size);
}

ExternalAudioEncoder::EncodedInfo
ExternalAudioEncoder::EncodeImpl(uint32_t rtp_timestamp,
                                std::span<const int16_t> audio,
                                webrtc::Buffer* encoded) {
    EncodedInfo info;
    info.encoded_bytes = 0;
    (void)audio;
    std::vector<uint8_t> frame;
    {
        std::lock_guard<std::mutex> lk(q_mu_);
        if (frame_q_.empty()) {
            // No pre-encoded frame available: emit silence (G.711 A-law silence = 0xD5 * 80)
            encoded->SetSize(80);
            std::memset(encoded->data(), 0xD5, 80);
            info.encoded_bytes = 80;
            info.encoded_timestamp = rtp_timestamp;
            info.payload_type = payload_type_;
            last_rtp_ts_ = rtp_timestamp;
            static uint32_t sil_cnt = 0;
            if (++sil_cnt % 100 == 0) {
                ump_info("[webrtc-audio] encoder: silence emitted (q empty) #%u\n", sil_cnt);
            }
            return info;
        }
        frame = std::move(frame_q_.front());
        frame_q_.pop();
    }
    encoded->SetSize(frame.size());
    std::memcpy(encoded->data(), frame.data(), frame.size());
    info.encoded_bytes = frame.size();
    info.encoded_timestamp = rtp_timestamp;
    info.payload_type = payload_type_;
    last_rtp_ts_ = rtp_timestamp;
    static uint32_t real_cnt = 0;
    if (++real_cnt % 100 == 0) {
        ump_info("[webrtc-audio] encoder: real frame sent #%u size=%zu\n", real_cnt, frame.size());
    }
    return info;
}

// =====================================================================
//  ExternalAudioEncoderFactory
// =====================================================================
ExternalAudioEncoderFactory::ExternalAudioEncoderFactory(
    std::shared_ptr<ExternalAudioEncoder::Registry> r) : registry_(std::move(r)) {}
//返回支持的编码列表，libwebrtc 据此生成 SDP 中的 m=audio 行。
std::vector<webrtc::AudioCodecSpec> ExternalAudioEncoderFactory::GetSupportedEncoders() {
    webrtc::AudioCodecSpec spec{
        webrtc::SdpAudioFormat("PCMA", 8000, 1),
        webrtc::AudioCodecInfo(8000, 1, 64000, 64000, 64000)
    };
    spec.info.allow_comfort_noise = false;
    spec.info.supports_network_adaption = false;
    return {spec};
}

std::optional<webrtc::AudioCodecInfo>
ExternalAudioEncoderFactory::QueryAudioEncoder(const webrtc::SdpAudioFormat& f) {
    if (f.name != "PCMA") return std::nullopt;
    webrtc::AudioCodecInfo info(f.clockrate_hz, f.num_channels, 64000, 64000, 64000);
    info.allow_comfort_noise = false;
    info.supports_network_adaption = false;
    return info;
}

std::unique_ptr<webrtc::AudioEncoder> ExternalAudioEncoderFactory::Create(
    const webrtc::Environment&, const webrtc::SdpAudioFormat& f, Options o) {
    ump_info("[webrtc] external audio factory create %s/%d/%d pt=%d\n",
             f.name.c_str(), f.clockrate_hz, (int)f.num_channels, o.payload_type);
    auto enc = std::make_unique<ExternalAudioEncoder>(registry_, o.payload_type);
    if (registry_) registry_->active = enc.get();
    return enc;
}

// =====================================================================
//  FakeAudioDeviceModule
//  Produces silent 10ms PCM blocks at 8kHz so the audio encode
//  pipeline runs.  The ExternalAudioEncoder ignores this silence.
// =====================================================================
FakeAudioDeviceModule::FakeAudioDeviceModule() = default;
FakeAudioDeviceModule::~FakeAudioDeviceModule() {
    StopRecording();
}

int32_t FakeAudioDeviceModule::RegisterAudioCallback(webrtc::AudioTransport* cb) {
    audio_cb_ = cb;
    return 0;
}

int32_t FakeAudioDeviceModule::Init() { initialized_ = true; return 0; }
int32_t FakeAudioDeviceModule::Terminate() {
    StopRecording();
    initialized_ = false;
    return 0;
}

int32_t FakeAudioDeviceModule::StartRecording() {
    if (capture_) return 0;
    capture_ = true;
    cap_thread_ = std::thread(&FakeAudioDeviceModule::CaptureThread, this);
    return 0;
}

int32_t FakeAudioDeviceModule::StopRecording() {
    capture_ = false;
    if (cap_thread_.joinable()) cap_thread_.join();
    return 0;
}

void FakeAudioDeviceModule::CaptureThread() {
    // 10ms @ 8kHz mono 16-bit = 160 bytes
    static constexpr size_t kSamples = 80;
    int16_t silence[kSamples];
    std::memset(silence, 0, sizeof(silence));
    uint32_t mic_level = 0;
    auto next = std::chrono::steady_clock::now();
    while (capture_) {
        if (audio_cb_) {
            audio_cb_->RecordedDataIsAvailable(
                silence, kSamples, 2 /*bytesPerSample*/, 1 /*nChannels*/,
                8000 /*samplesPerSec*/, 0 /*totalDelayMS*/, 0 /*clockDrift*/,
                mic_level, false /*keyPressed*/, mic_level);
        }
        next += std::chrono::milliseconds(10);
        std::this_thread::sleep_until(next);
    }
}

} // namespace wt
} // namespace ecovacs
