#ifndef API_VIDEO_WEBRTC_PUSH_H_
#define API_VIDEO_WEBRTC_PUSH_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "queue.h"
#include "video_common_defines.h"
#include "api_video_encode.h"
#include "webrtc_transport.h"
#include "audio_capture.h"
#include "resampler.h"
#include "g711_encoder.h"
#include "audio_processor.h"

// ringbuffer.h defines a `min` macro that breaks C++ STL headers, so we
// only forward-declare the type here and include the real header in the .cpp
// (after all STL includes) followed by #undef min/max.
struct ringbuffer;

// WebRTC push wrapper backed by ump's WebrtcTransport component.
// The heavy WebRTC stack (PeerConnection / SDP / ICE / signaling / RTP
// packetizer) lives in libwebrtc_transport.so; this class only bridges
// the hardware encoder and feeds H264 access units.
//
// Audio (device -> browser): ALSA capture @16kHz -> resample to 8kHz ->
// G.711 A-law encode -> transport_->SendG711Frame().  Browser -> device
// audio is not implemented yet.
namespace ecovacs
{
    struct WebrtcConfig {
        std::string signaling_url = "ws://172.20.10.12:8080/ws?room=room1";
        std::string stun_url;
        std::string turn_url;
        std::string turn_username;
        std::string turn_password;
        int video_fps = 15;
        // Maximum simultaneous viewers (phone / browser). Each viewer costs
        // one PeerConnection + H264 encoder instance.
        int max_viewers = 4;
        // Enable audio send (device -> browser).
        bool enable_audio = true;
    };

    class VideoWebrtcPush : public video_common::EncodedImageSinkInterface,
                            public IWebrtcVideoEncoder,
                            public IWebrtcViewerListener
    {
    public:
        VideoWebrtcPush();
        ~VideoWebrtcPush() override;

        static std::unique_ptr<VideoWebrtcPush> Create();

        int32_t Init();
        int32_t UnInit();
        int32_t StartPush();
        int32_t StopPush();
        bool PushStarted() const { return push_started_.load(std::memory_order_acquire); }

        void SetConfig(const WebrtcConfig& config) { config_ = config; }
        const WebrtcConfig& GetConfig() const { return config_; }

        void SetVideoEncode(std::shared_ptr<VideoEncode> encode);

        // Audio noise reduction level (0=off, 1=Low, 2=Moderate, 3=High, 4=VeryHigh).
        // Must be called before StartPush(); takes effect in AudioInit().
        void SetAudioNoiseLevel(int32_t level) { audio_noise_level_ = level; }
        int32_t GetAudioNoiseLevel() const { return audio_noise_level_; }

        void OnFrame(
            video_common::EncodedImageBuffer *encoded_image,
            const video_common::CodecSpecificInfo *codec_specific_info) override;

        // IWebrtcVideoEncoder: forward GCC rate control to hardware encoder.
        void OnRatesChanged(uint32_t bitrate_bps, uint32_t framerate_fps) override;

        // IWebrtcViewerListener: log viewer connect / disconnect.
        void OnViewerConnected(int viewer_id) override;
        void OnViewerDisconnected(int viewer_id) override;

    private:
        struct TH264Buffer {
            video_common::EncodedImageBuffer *h264_frame = nullptr;
        };

        void EnqueueFrame(video_common::EncodedImageBuffer *encoded_image,
                          const video_common::CodecSpecificInfo *codec_specific_info);
        TH264Buffer* DequeueLatestFrame();

        // ---- Audio pipeline (device -> browser) ----
        void AudioCaptureCallback(const int16_t* data, size_t bytes,
                                  int sample_rate, int channels);
        bool AudioPushProcess();
        int32_t AudioInit();
        void AudioUninit();
        int32_t AudioStart();
        void AudioStop();

        TIntelliBufHndl h264_buf_hndl_ = {};
        TH264Buffer h264_buf_[30];

        std::mutex api_lock_;
        std::atomic<bool> push_started_{false};
        std::atomic<bool> quit_{false};
        std::thread send_thread_;

        WebrtcConfig config_;
        std::shared_ptr<VideoEncode> video_encode_;
        std::unique_ptr<WebrtcTransport> transport_;

        // Audio members
        AlsaRecorder audio_recorder_;
        AudioResampler audio_resampler_;
        G711Encoder g711_encoder_{G711Encoder::ALAW};
        struct ringbuffer* audio_ring_buf_ = nullptr;
        AudioProcessor* audio_processor_ = nullptr;   // 降噪处理器
        int32_t audio_noise_level_ = 0;               // 0=off, 1-4=降噪等级
        std::thread audio_thread_;
        std::mutex audio_mutex_;
        std::atomic<bool> audio_quit_{false};
        std::atomic<bool> audio_running_{false};
    };

} // namespace ecovacs

#endif // API_VIDEO_WEBRTC_PUSH_H_
