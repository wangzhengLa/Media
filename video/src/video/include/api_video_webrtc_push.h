#ifndef API_VIDEO_WEBRTC_PUSH_H_
#define API_VIDEO_WEBRTC_PUSH_H_

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

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
// G.711 A-law encode -> transport_->SendAudioFrame().  Browser -> device
// audio is not implemented yet.
namespace ecovacs
{
    struct WebrtcConfig {
        std::string signaling_url = "ws://10.10.103.190:8080/ws?room=room1";
        std::string stun_url;
        std::string turn_url;
        std::string turn_username;
        std::string turn_password;
        // Maximum simultaneous viewers (phone / browser). Each viewer costs
        // one PeerConnection + H264 encoder instance.
        int max_viewers = 4;
    };

    class VideoWebrtcPush : public video_common::EncodedImageSinkInterface,
                            public IWebrtcVideoEncoder,
                            public IWebrtcViewerListener,
                            public IWebrtcAudioReceiver,
                            public IWebrtcVideoReceiver
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

        // AEC (acoustic echo cancellation) toggle.  When enabled, the ALSA
        // speaker-loopback channel (audio_ref_channel_) is used as the
        // reference signal so that echo picked up by the microphone is
        // removed before encoding.  Must be called before StartPush().
        void SetAecEnabled(bool en) { aec_enabled_ = en; }
        bool GetAecEnabled() const { return aec_enabled_; }

        // ALSA capture device and channel layout (same config as RTSP push).
        // The capture device exposes multiple channels; one carries the
        // microphone and another carries the speaker loopback (AEC ref).
        void SetAudioDevice(const std::string& dev) { audio_device_ = dev; }
        void SetAudioCaptureChannels(int32_t ch) { audio_capture_channels_ = ch; }
        void SetAudioMicChannel(int32_t ch) { audio_mic_channel_ = ch; }
        void SetAudioRefChannel(int32_t ch) { audio_ref_channel_ = ch; }

        void OnFrame(
            video_common::EncodedImageBuffer *encoded_image,
            const video_common::CodecSpecificInfo *codec_specific_info) override;

        // IWebrtcVideoEncoder: forward GCC rate control to hardware encoder.
        void OnRatesChanged(uint32_t bitrate_bps, uint32_t framerate_fps) override;
        // IWebrtcVideoEncoder: PLI/FIR from a viewer triggers an IDR request.
        void OnKeyFrameRequested() override;

        // IWebrtcViewerListener: log viewer connect / disconnect.
        void OnViewerConnected(const std::string& peer_id) override;
        void OnViewerDisconnected(const std::string& peer_id) override;

        // IWebrtcAudioReceiver: remote (browser -> device) audio frame.
        // Mixes all viewers' PCM and pushes the result to the audio module.
        void OnAudioFrame(const std::string& peer_id,
                          const int16_t* pcm, size_t samples,
                          int sample_rate, int channels) override;

        // IWebrtcVideoReceiver: remote (browser -> device) encoded H.264 frame.
        // Saves the bitstream to a file for offline review.
        void OnVideoEncodedFrame(const std::string& peer_id,
                                 const uint8_t* h264_data, size_t size,
                                 bool is_keyframe, int64_t timestamp_us) override;

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

        // ---- Receive-side (browser -> device) ----
        // Audio: the ump layer already mixed all viewers' PCM via AudioMixer;
        // we forward it to the audio module over TCP for playback.  Network
        // I/O runs in RecvAudioThread() so MixThread is never blocked by a
        // slow/dead audio module.  OnAudioFrame() only enqueues PCM.
        void RecvAudioThread();
        void VideoRecvSaveFrame(const std::string& peer_id,
                                const uint8_t* data, size_t size, bool keyframe);

        TIntelliBufHndl h264_buf_hndl_ = {};
        TH264Buffer h264_buf_[30];

        std::mutex api_lock_;
        std::atomic<bool> push_started_{false};
        std::atomic<bool> quit_{false};
        std::atomic<int64_t> last_keyframe_req_us_{0};  // throttle shared IDR requests
        std::atomic<size_t> avg_frame_size_{0};          // EMA of encoded frame size (bytes) for fps calc
        std::thread send_thread_;

        WebrtcConfig config_;
        std::shared_ptr<VideoEncode> video_encode_;
        std::unique_ptr<WebrtcTransport> transport_;

        // Audio members (device -> browser)
        AlsaRecorder audio_recorder_;
        AudioResampler audio_resampler_;
        G711Encoder g711_encoder_{G711Encoder::ALAW};
        struct ringbuffer* audio_ring_buf_ = nullptr;
        struct ringbuffer* audio_ref_buf_ = nullptr;   // AEC reference (speaker loopback @16kHz)
        AudioProcessor* audio_processor_ = nullptr;   // 降噪 + AEC 处理器
        int32_t audio_noise_level_ = 0;               // 0=off, 1-4=降噪等级
        bool aec_enabled_ = false;                     // AEC 总开关
        std::string audio_device_ = "default";        // ALSA 采集设备
        int32_t audio_capture_channels_ = 8;          // 采集声道数
        int32_t audio_mic_channel_ = 0;               // 麦克风声道 index
        int32_t audio_ref_channel_ = 5;               // 参考声道 index（扬声器回环）
        std::thread audio_thread_;
        std::mutex audio_mutex_;
        std::atomic<bool> audio_quit_{false};
        std::atomic<bool> audio_running_{false};

        // ---- Receive-side members (browser -> device) ----
        // PCM queue between MixThread (producer) and RecvAudioThread
        // (consumer).  Bounded: oldest frame is dropped when full so real
        // time audio never stalls MixThread on a slow audio module.
        struct PcmFrame { std::vector<int16_t> samples; };
        static constexpr size_t kPcmQueueMax = 10;  // ~100ms @ 10ms/frame
        std::queue<PcmFrame> recv_audio_q_;
        std::mutex recv_audio_q_mu_;
        std::condition_variable recv_audio_q_cv_;
        std::thread recv_audio_thread_;
        // TCP socket to the audio module (owned by RecvAudioThread only).
        int recv_audio_sock_ = -1;
        // Video: per-peer file handles for saving H.264 bitstreams.
        std::mutex recv_video_mu_;
        std::unordered_map<std::string, FILE*> recv_video_files_;
    };

} // namespace ecovacs

#endif // API_VIDEO_WEBRTC_PUSH_H_
