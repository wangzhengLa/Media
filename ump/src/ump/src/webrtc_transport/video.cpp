#include "webrtc_transport/video.h"

#include <log.h>

#include <cstring>
#include <chrono>
#include <thread>

#include "api/video_codecs/sdp_video_format.h"
#include "api/video/i420_buffer.h"
#include "modules/video_coding/codecs/h264/include/h264_globals.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/time_utils.h"

namespace ecovacs {
namespace wt {

// =====================================================================
//  BitrateAggregator
// =====================================================================
void BitrateAggregator::SetVideoEncoder(IWebrtcVideoEncoder* e) {
    std::lock_guard<std::mutex> lk(mu_);
    video_encoder_ = e;
}

void BitrateAggregator::Update(ExternalH264Encoder* enc, uint32_t bitrate_bps, uint32_t fps) {
    uint32_t min_br = 0;
    uint32_t min_fps = 0;
    IWebrtcVideoEncoder* ve = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        bitrates_[enc] = bitrate_bps;
        ve = video_encoder_;
        for (auto& [e, br] : bitrates_) {
            (void)e;
            if (br == 0) continue;  // skip viewers with no valid estimate yet
            if (min_br == 0 || br < min_br) min_br = br;
        }
        min_fps = fps;
        if (min_br == last_br_ && min_fps == last_fps_) return;
        last_br_ = min_br;
        last_fps_ = min_fps;
    }
    // Only forward a valid (non-zero) bitrate. Passing 0/-1 down to the
    // hardware encoder results in an overflowed/negative bitrate value
    // (e.g. cedarc "bitrate[-296] is not right").
    if (ve && min_br != 0) ve->OnRatesChanged((int32_t)min_br, (int32_t)min_fps);
}

void BitrateAggregator::Remove(ExternalH264Encoder* enc) {
    uint32_t min_br = 0;
    uint32_t min_fps = 0;
    IWebrtcVideoEncoder* ve = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        bitrates_.erase(enc);
        ve = video_encoder_;
        for (auto& [e, br] : bitrates_) {
            (void)e;
            if (br == 0) continue;
            if (min_br == 0 || br < min_br) min_br = br;
        }
        min_fps = 15;
        if (min_br == last_br_ && min_fps == last_fps_) return;
        last_br_ = min_br;
        last_fps_ = min_fps;
    }
    if (ve && min_br != 0) ve->OnRatesChanged((int32_t)min_br, (int32_t)min_fps);
}

// =====================================================================
//  ExternalH264Encoder
// =====================================================================
ExternalH264Encoder::ExternalH264Encoder(std::shared_ptr<Registry> r) : registry_(r) {}
ExternalH264Encoder::~ExternalH264Encoder() {
    if (registry_ && registry_->active == this) registry_->active = nullptr;
    if (registry_ && registry_->aggregator) registry_->aggregator->Remove(this);
}

int ExternalH264Encoder::InitEncode(const webrtc::VideoCodec* cs, const Settings&) {
    if (!cs) return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
    width_ = cs->width;
    height_ = cs->height;
    if (registry_) registry_->active = this;
    ump_info("[webrtc] external H264 encoder InitEncode %ux%u\n", width_, height_);
    return WEBRTC_VIDEO_CODEC_OK;
}

int32_t ExternalH264Encoder::RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* cb) {
    callback_ = cb;
    ump_info("[webrtc] external H264 callback registered\n");
    return WEBRTC_VIDEO_CODEC_OK;
}

int32_t ExternalH264Encoder::Release() {
    callback_ = nullptr;
    if (registry_ && registry_->active == this) registry_->active = nullptr;
    if (registry_ && registry_->aggregator) registry_->aggregator->Remove(this);
    ump_info("[webrtc] external H264 release\n");
    return WEBRTC_VIDEO_CODEC_OK;
}

int32_t ExternalH264Encoder::Encode(const webrtc::VideoFrame&,
                                    const std::vector<webrtc::VideoFrameType>*) {
    return WEBRTC_VIDEO_CODEC_OK;
}

void ExternalH264Encoder::SetRates(const RateControlParameters& p) {
    last_br_ = p.bitrate.get_sum_bps();
    last_fps_ = p.framerate_fps;
    //ump_debug("[webrtc] ext h264 setrates %u bps / %u fps\n", last_br_, last_fps_);
    if (registry_ && registry_->aggregator) registry_->aggregator->Update(this, last_br_, last_fps_);
}

ExternalH264Encoder::EncoderInfo ExternalH264Encoder::GetEncoderInfo() const {
    EncoderInfo info;
    info.supports_native_handle = false;
    info.implementation_name = "ExternalH264";
    info.has_trusted_rate_controller = true;
    info.is_hardware_accelerated = true;
    info.supports_simulcast = false;
    return info;
}

void ExternalH264Encoder::InjectH264Frame(const uint8_t* d, size_t s, bool key, uint32_t ts) {
    if (!callback_) return;
    webrtc::EncodedImage img;
    img.SetEncodedData(webrtc::EncodedImageBuffer::Create(d, s));
    img._encodedWidth = width_ ? width_ : 1280;
    img._encodedHeight = height_ ? height_ : 720;
    img.SetRtpTimestamp(ts);
    img._frameType = key ? webrtc::VideoFrameType::kVideoFrameKey
                         : webrtc::VideoFrameType::kVideoFrameDelta;
    img.capture_time_ms_ = webrtc::TimeMillis();
    img.content_type_ = webrtc::VideoContentType::UNSPECIFIED;
    webrtc::CodecSpecificInfo info;
    info.codecType = webrtc::kVideoCodecH264;
    webrtc::CodecSpecificInfoH264& h264 = info.codecSpecific.H264;
    h264.packetization_mode = webrtc::H264PacketizationMode::NonInterleaved;
    callback_->OnEncodedImage(img, &info);
}

// =====================================================================
//  ExternalH264EncoderFactory
// =====================================================================
ExternalH264EncoderFactory::ExternalH264EncoderFactory(
    std::shared_ptr<ExternalH264Encoder::Registry> r) : registry_(r) {}

std::vector<webrtc::SdpVideoFormat> ExternalH264EncoderFactory::GetSupportedFormats() const {
    webrtc::SdpVideoFormat fmt("H264");
    fmt.parameters["level-asymmetry-allowed"] = "1";
    fmt.parameters["packetization-mode"] = "1";
    fmt.parameters["profile-level-id"] = "42e01f";
    return {fmt};
}

std::unique_ptr<webrtc::VideoEncoder> ExternalH264EncoderFactory::Create(
    const webrtc::Environment&, const webrtc::SdpVideoFormat& f) {
    ump_info("[webrtc] external H264 encoder Create %s\n", f.name.c_str());
    return std::make_unique<ExternalH264Encoder>(registry_);
}

// =====================================================================
//  DummyVideoSource / ExternalVideoTrackSource
// =====================================================================
void DummyVideoSource::AddOrUpdateSink(webrtc::VideoSinkInterface<webrtc::VideoFrame>* s,
                                       const webrtc::VideoSinkWants&) {
    std::lock_guard<std::mutex> lk(mu_);
    sink_ = s;
}

void DummyVideoSource::RemoveSink(webrtc::VideoSinkInterface<webrtc::VideoFrame>* s) {
    std::lock_guard<std::mutex> lk(mu_);
    if (sink_ == s) sink_ = nullptr;
}

void DummyVideoSource::PushFrame(const webrtc::VideoFrame& f) {
    std::lock_guard<std::mutex> lk(mu_);
    if (sink_) sink_->OnFrame(f);
}

ExternalVideoTrackSource::ExternalVideoTrackSource()
    : webrtc::VideoTrackSource(false) {
    auto buf = webrtc::I420Buffer::Create(kW, kH);
    buf->InitializeData();
    black_ = webrtc::VideoFrame::Builder()
                 .set_video_frame_buffer(buf)
                 .set_timestamp_rtp(0)
                 .set_timestamp_us(0)
                 .build();
    feeder_ = std::thread([this]() {
        while (!quit_) {
            source_.PushFrame(*black_);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
}

ExternalVideoTrackSource::~ExternalVideoTrackSource() {
    quit_ = true;
    if (feeder_.joinable()) feeder_.join();
}

webrtc::VideoSourceInterface<webrtc::VideoFrame>* ExternalVideoTrackSource::source() {
    return &source_;
}

} // namespace wt
} // namespace ecovacs
