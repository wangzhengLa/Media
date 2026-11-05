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
//  VideoRateController
// =====================================================================
void VideoRateController::SetVideoEncoder(IWebrtcVideoEncoder* e) {
    std::lock_guard<std::mutex> lk(mu_);
    video_encoder_ = e;
}

void VideoRateController::Update(ExternalVideoEncoder* enc, uint32_t bps, uint32_t fps) {
    uint32_t min_br = 0;
    uint32_t min_fps = 0;
    IWebrtcVideoEncoder* ve = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        rates_[enc] = {bps, fps};
        ve = video_encoder_;
        for (auto& [e, r] : rates_) {
            (void)e;
            if (r.bps == 0) continue;  // skip viewers with no valid estimate yet
            if (min_br == 0 || r.bps < min_br) min_br = r.bps;
            if (r.fps != 0 && (min_fps == 0 || r.fps < min_fps)) min_fps = r.fps;
        }
        if (min_br == last_br_ && min_fps == last_fps_) return;
        last_br_ = min_br;
        last_fps_ = min_fps;
    }
    // Only forward a valid (non-zero) bitrate. Passing 0/-1 down to the
    // hardware encoder results in an overflowed/negative bitrate value
    // (e.g. cedarc "bitrate[-296] is not right").
    if (ve && min_br != 0) ve->OnRatesChanged((int32_t)min_br, (int32_t)min_fps);
}

void VideoRateController::Remove(ExternalVideoEncoder* enc) {
    uint32_t min_br = 0;
    uint32_t min_fps = 0;
    IWebrtcVideoEncoder* ve = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        rates_.erase(enc);
        ve = video_encoder_;
        for (auto& [e, r] : rates_) {
            (void)e;
            if (r.bps == 0) continue;
            if (min_br == 0 || r.bps < min_br) min_br = r.bps;
            if (r.fps != 0 && (min_fps == 0 || r.fps < min_fps)) min_fps = r.fps;
        }
        if (min_br == last_br_ && min_fps == last_fps_) return;
        last_br_ = min_br;
        last_fps_ = min_fps;
    }
    if (ve && min_br != 0) ve->OnRatesChanged((int32_t)min_br, (int32_t)min_fps);
}

// =====================================================================
//  ExternalVideoEncoder
// =====================================================================
ExternalVideoEncoder::ExternalVideoEncoder(std::shared_ptr<Registry> r) : registry_(r) {}
ExternalVideoEncoder::~ExternalVideoEncoder() {
    if (registry_ && registry_->active == this) registry_->active = nullptr;
    if (registry_ && registry_->rate_controller) registry_->rate_controller->Remove(this);
}

int ExternalVideoEncoder::InitEncode(const webrtc::VideoCodec* cs, const Settings&) {
    if (!cs) return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
    width_ = cs->width;
    height_ = cs->height;
    if (registry_) registry_->active = this;
    ump_info("[webrtc] external video encoder InitEncode %ux%u\n", width_, height_);
    return WEBRTC_VIDEO_CODEC_OK;
}

int32_t ExternalVideoEncoder::RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* cb) {
    callback_ = cb;
    ump_info("[webrtc] external video callback registered\n");
    return WEBRTC_VIDEO_CODEC_OK;
}

int32_t ExternalVideoEncoder::Release() {
    callback_ = nullptr;
    if (registry_ && registry_->active == this) registry_->active = nullptr;
    if (registry_ && registry_->rate_controller) registry_->rate_controller->Remove(this);
    ump_info("[webrtc] external video release\n");
    return WEBRTC_VIDEO_CODEC_OK;
}

int32_t ExternalVideoEncoder::Encode(const webrtc::VideoFrame&,
                                    const std::vector<webrtc::VideoFrameType>* frame_types) {
    // External encoder: Encode() is a no-op for actual encoding (frames are
    // injected via InjectVideoFrame).  However libwebrtc calls Encode() with
    // frame_types[0] == kVideoFrameKey when a remote viewer sends PLI or FIR
    // (picture loss / full intra request).  We detect that and ask the shared
    // hardware encoder to produce an IDR immediately so the viewer can
    // recover from packet loss without waiting for the next periodic IDR.
    //
    // Throttling is NOT done here: the hardware encoder is shared by all
    // viewers, so per-instance throttling cannot coalesce requests from
    // multiple viewers.  The shared VideoWebrtcPush::OnKeyFrameRequested() does
    // the global 500ms coalescing instead (one IDR is broadcast to everyone).
    if (frame_types && !frame_types->empty() &&
        (*frame_types)[0] == webrtc::VideoFrameType::kVideoFrameKey) {
        IWebrtcVideoEncoder* ve = registry_ ? registry_->video_encoder : nullptr;
        if (ve) {
            ump_info("[webrtc] keyframe requested (PLI/FIR)\n");
            ve->OnKeyFrameRequested();
        }
    }
    return WEBRTC_VIDEO_CODEC_OK;
}

void ExternalVideoEncoder::SetRates(const RateControlParameters& p) {
    last_br_ = p.bitrate.get_sum_bps();
    last_fps_ = p.framerate_fps;
    //ump_debug("[webrtc] ext video setrates %u bps / %u fps\n", last_br_, last_fps_);
    if (registry_ && registry_->rate_controller) registry_->rate_controller->Update(this, last_br_, last_fps_);
}

ExternalVideoEncoder::EncoderInfo ExternalVideoEncoder::GetEncoderInfo() const {
    EncoderInfo info;
    info.supports_native_handle = false;
    info.implementation_name = "ExternalVideo";
    info.has_trusted_rate_controller = true;
    info.is_hardware_accelerated = true;
    info.supports_simulcast = false;
    return info;
}

void ExternalVideoEncoder::InjectVideoFrame(const uint8_t* d, size_t s, bool key) {
    if (!callback_) return;
    if (first_frame_us_ == 0) {
        first_frame_us_ = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    uint64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    uint32_t ts = static_cast<uint32_t>((now_us - first_frame_us_) * 90 / 1000);
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
//  ExternalVideoEncoderFactory
// =====================================================================
ExternalVideoEncoderFactory::ExternalVideoEncoderFactory(
    std::shared_ptr<ExternalVideoEncoder::Registry> r) : registry_(r) {}

std::vector<webrtc::SdpVideoFormat> ExternalVideoEncoderFactory::GetSupportedFormats() const {
    webrtc::SdpVideoFormat fmt("H264");
    fmt.parameters["level-asymmetry-allowed"] = "1";
    fmt.parameters["packetization-mode"] = "1";
    fmt.parameters["profile-level-id"] = "42e01f";
    return {fmt};
}

std::unique_ptr<webrtc::VideoEncoder> ExternalVideoEncoderFactory::Create(
    const webrtc::Environment&, const webrtc::SdpVideoFormat& f) {
    ump_info("[webrtc] external video encoder Create %s\n", f.name.c_str());
    return std::make_unique<ExternalVideoEncoder>(registry_);
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

ExternalVideoTrackSource::ExternalVideoTrackSource(uint32_t width, uint32_t height, uint32_t fps)
    : webrtc::VideoTrackSource(false), width_(width), height_(height), fps_(fps) {
    auto buf = webrtc::I420Buffer::Create(width_, height_);
    buf->InitializeData();
    black_ = webrtc::VideoFrame::Builder()
                 .set_video_frame_buffer(buf)
                 .set_timestamp_rtp(0)
                 .set_timestamp_us(0)
                 .build();
    uint32_t interval_ms = fps_ > 0 ? 1000u / fps_ : 100u;
    feeder_ = std::thread([this, interval_ms]() {
        while (!quit_) {
            source_.PushFrame(*black_);
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
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
