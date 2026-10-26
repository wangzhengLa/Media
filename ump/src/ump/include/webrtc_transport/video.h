#ifndef WEBRTC_TRANSPORT_VIDEO_H_
#define WEBRTC_TRANSPORT_VIDEO_H_

#include "webrtc_transport/webrtc_transport.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "api/scoped_refptr.h"
#include "api/video/encoded_image.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "api/video/video_source_interface.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "pc/video_track_source.h"

namespace ecovacs {
namespace wt {

// =====================================================================
//  BitrateAggregator: picks the min GCC target across all viewers so
//  the shared hardware encoder never exceeds the bottleneck link.
// =====================================================================
class BitrateAggregator {
public:
    void SetVideoEncoder(IWebrtcVideoEncoder* e);
    void Update(class ExternalH264Encoder* enc, uint32_t bitrate_bps, uint32_t fps);
    void Remove(class ExternalH264Encoder* enc);
private:
    std::mutex mu_;
    std::unordered_map<ExternalH264Encoder*, uint32_t> bitrates_;
    IWebrtcVideoEncoder* video_encoder_ = nullptr;
    uint32_t last_br_ = 0, last_fps_ = 0;
};

// =====================================================================
//  External H264 encoder: injects pre-encoded H264 frames into
//  libwebrtc's RTP packetizer (bypasses libwebrtc's own encoding).
// =====================================================================
class ExternalH264Encoder : public webrtc::VideoEncoder {
public:
    struct Registry {
        ExternalH264Encoder* active = nullptr;
        IWebrtcVideoEncoder* video_encoder = nullptr;
        std::shared_ptr<BitrateAggregator> aggregator;
    };
    explicit ExternalH264Encoder(std::shared_ptr<Registry> r);
    ~ExternalH264Encoder() override;

    int InitEncode(const webrtc::VideoCodec* cs, const Settings&) override;
    int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* cb) override;
    int32_t Release() override;
    int32_t Encode(const webrtc::VideoFrame&, const std::vector<webrtc::VideoFrameType>*) override;
    void SetRates(const RateControlParameters& p) override;
    EncoderInfo GetEncoderInfo() const override;

    // Called by the send loop to inject a pre-encoded H264 access unit.
    void InjectH264Frame(const uint8_t* d, size_t s, bool key, uint32_t ts);

private:
    std::shared_ptr<Registry> registry_;
    webrtc::EncodedImageCallback* callback_ = nullptr;
    uint32_t width_ = 0, height_ = 0, last_br_ = 0, last_fps_ = 0;
};

class ExternalH264EncoderFactory : public webrtc::VideoEncoderFactory {
public:
    explicit ExternalH264EncoderFactory(std::shared_ptr<ExternalH264Encoder::Registry> r);
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override;
    std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment&, const webrtc::SdpVideoFormat& f) override;
private:
    std::shared_ptr<ExternalH264Encoder::Registry> registry_;
};

// ---- Dummy video source (keeps the video track alive) ----
class DummyVideoSource : public webrtc::VideoSourceInterface<webrtc::VideoFrame> {
public:
    void AddOrUpdateSink(webrtc::VideoSinkInterface<webrtc::VideoFrame>* s, const webrtc::VideoSinkWants&) override;
    void RemoveSink(webrtc::VideoSinkInterface<webrtc::VideoFrame>* s) override;
    void PushFrame(const webrtc::VideoFrame& f);
private:
    std::mutex mu_;
    webrtc::VideoSinkInterface<webrtc::VideoFrame>* sink_ = nullptr;
};

class ExternalVideoTrackSource : public webrtc::VideoTrackSource {
public:
    static constexpr int kW = 640, kH = 480;
    ExternalVideoTrackSource();
    ~ExternalVideoTrackSource() override;
protected:
    webrtc::VideoSourceInterface<webrtc::VideoFrame>* source() override;
private:
    DummyVideoSource source_;
    std::optional<webrtc::VideoFrame> black_;
    std::atomic<bool> quit_{false};
    std::thread feeder_;
};

} // namespace wt
} // namespace ecovacs

#endif // WEBRTC_TRANSPORT_VIDEO_H_
