#include "api_video_webrtc_push.h"

#include <log.h>
#include "osp.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <vector>

// ringbuffer.h defines a `min`/`max` macro via GCC statement expressions that
// corrupts C++ STL headers.  Include it after all STL headers and immediately
// undefine the macros so later code (and <algorithm>) keeps working.
#include "ringbuffer.h"
#ifdef min
#  undef min
#endif
#ifdef max
#  undef max
#endif

namespace ecovacs
{
    // ---- Audio pipeline constants (mirror RTSP push) ----
    static constexpr int AUDIO_RING_BUFFER_SIZE = 65536;   // 64KB, power of 2
    static constexpr int AUDIO_INPUT_RATE = 16000;          // ALSA capture rate
    static constexpr int AUDIO_OUTPUT_RATE = 8000;          // G.711 output rate
    static constexpr int AUDIO_INPUT_CHANNELS = 1;          // capture mono
    static constexpr int AUDIO_FRAME_MS = 10;               // 10ms per G.711 frame
    static constexpr const char* AUDIO_DEVICE = "plughw:1,0";

    static uint64_t NowUs() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Keyframe detection for Annex-B H264 (SPS=7 / IDR=5).
    static bool H264IsKeyframe(const uint8_t* data, size_t size) {
        size_t i = 0;
        while (i + 2 < size) {
            if (data[i] == 0 && data[i + 1] == 0) {
                size_t sl = 0;
                if (data[i + 2] == 1) sl = 3;
                else if (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1) sl = 4;
                if (sl) {
                    uint8_t t = data[i + sl] & 0x1F;
                    if (t == 5 || t == 7) return true;
                    i += sl + 1; continue;
                }
            }
            ++i;
        }
        return false;
    }

    VideoWebrtcPush::VideoWebrtcPush() = default;
    VideoWebrtcPush::~VideoWebrtcPush() { UnInit(); }

    std::unique_ptr<VideoWebrtcPush> VideoWebrtcPush::Create() {
        auto obj = std::unique_ptr<VideoWebrtcPush>(new VideoWebrtcPush());
        if (0 != obj->Init()) {
            ump_error("[webrtc] Failed to create VideoWebrtcPush\n");
            return nullptr;
        }
        return obj;
    }

    void VideoWebrtcPush::SetVideoEncode(std::shared_ptr<VideoEncode> encode) {
        video_encode_ = encode;
    }

    int32_t VideoWebrtcPush::Init() {
        if (IPC_INTELLI_SOK != IntelliBufCreate(&h264_buf_hndl_, 30)) {
            ump_error("[webrtc] IntelliBufCreate failed\n");
            return -1;
        }
        for (int i = 0; i < 30; ++i) {
            if (IPC_INTELLI_SOK != IntelliPutBuffer(&h264_buf_hndl_, &h264_buf_[i], IPC_INTELLI_EMPTY_BUFFER_FLAG)) {
                ump_error("[webrtc] IntelliPutBuffer failed for idx %d\n", i);
                IntelliBufDelete(&h264_buf_hndl_);
                return -1;
            }
        }
        ump_info("[webrtc] VideoWebrtcPush Init done\n");
        return 0;
    }

    int32_t VideoWebrtcPush::UnInit() {
        StopPush();
        AudioUninit();
        if (h264_buf_hndl_.emptyQue.pu64Queue != nullptr) {
            IntelliBufDelete(&h264_buf_hndl_);
            h264_buf_hndl_ = {};
        }
        return 0;
    }

    int32_t VideoWebrtcPush::StartPush() {
        bool expected = false;
        if (!push_started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            ump_info("[webrtc] push already running\n");
            return 0;
        }
        quit_.store(false, std::memory_order_release);

        WebrtcTransportConfig tcfg;
        tcfg.signaling_url = config_.signaling_url;
        tcfg.stun_url = config_.stun_url;
        tcfg.turn_url = config_.turn_url;
        tcfg.turn_username = config_.turn_username;
        tcfg.turn_password = config_.turn_password;
        tcfg.max_viewers = config_.max_viewers;

        transport_ = WebrtcTransport::Create();
        if (!transport_) {
            ump_error("[webrtc] WebrtcTransport::Create failed\n");
            push_started_.store(false);
            return -1;
        }
        transport_->Init(tcfg);
        transport_->SetVideoEncoder(this);
        transport_->SetViewerListener(this);
        if (transport_->Start() != 0) {
            ump_error("[webrtc] transport Start failed\n");
            transport_.reset();
            push_started_.store(false);
            return -1;
        }

        // Start audio capture + encode pipeline (device -> browser).
        if (AudioStart() != 0) {
            ump_warn("[webrtc] audio start failed, running video-only\n");
        }

        // Drain thread: pull H264 frames from the queue and feed the transport.
        send_thread_ = std::thread([this] {
            ump_info("[webrtc] drain thread started\n");
            int drain_cnt = 0;
            while (!quit_.load(std::memory_order_acquire)) {
                TH264Buffer* f = DequeueLatestFrame();
                if (f && f->h264_frame) {
                    transport_->SendVideoFrame(
                        f->h264_frame->data(), f->h264_frame->size(),
                        H264IsKeyframe(f->h264_frame->data(), f->h264_frame->size()));
                    f->h264_frame->RelRef();
                    f->h264_frame = nullptr;
                    IntelliPutBuffer(&h264_buf_hndl_, f, IPC_INTELLI_EMPTY_BUFFER_FLAG);
                    if (++drain_cnt % 30 == 0)
                        ump_info("[webrtc] drain #%d, empty=%u full=%u\n",
                                 drain_cnt,
                                 IntelliBufGetEmptyCount(&h264_buf_hndl_),
                                 IntelliBufGetFullCount(&h264_buf_hndl_));
                } else {
                    usleep(1000);
                }
            }
            ump_info("[webrtc] drain thread exited\n");
        });

        ump_info("[webrtc] StartPush done\n");
        return 0;
    }

    int32_t VideoWebrtcPush::StopPush() {
        bool expected = true;
        if (!push_started_.compare_exchange_strong(expected, false, std::memory_order_acq_rel))
            return 0;
        quit_.store(true, std::memory_order_release);
        AudioStop();
        if (send_thread_.joinable()) { send_thread_.join(); ump_info("[webrtc] drain thread joined\n"); }
        if (transport_) { transport_->Stop(); transport_.reset(); }
        ump_info("[webrtc] StopPush done\n");
        return 0;
    }

    void VideoWebrtcPush::OnFrame(video_common::EncodedImageBuffer* img,
                                  const video_common::CodecSpecificInfo* info) {
        if (!img || !info) return;
        if (!push_started_.load(std::memory_order_acquire)) return;
        if (info->codecType != video_common::kVideoCodecH264) return;
        EnqueueFrame(img, info);
    }

    void VideoWebrtcPush::EnqueueFrame(video_common::EncodedImageBuffer* img,
                                       const video_common::CodecSpecificInfo* info) {
        TH264Buffer* f = (TH264Buffer*)IntelliGetBuffer(&h264_buf_hndl_, IPC_INTELLI_EMPTY_BUFFER_FLAG, IPC_INTELLI_TIMEOUT_NONE);
        if (!f) {
            TH264Buffer* old = (TH264Buffer*)IntelliGetBuffer(&h264_buf_hndl_, IPC_INTELLI_FULL_BUFFER_FLAG, IPC_INTELLI_TIMEOUT_NONE);
            if (old) {
                if (old->h264_frame) old->h264_frame->RelRef();
                IntelliPutBuffer(&h264_buf_hndl_, old, IPC_INTELLI_EMPTY_BUFFER_FLAG);
            }
            f = (TH264Buffer*)IntelliGetBuffer(&h264_buf_hndl_, IPC_INTELLI_EMPTY_BUFFER_FLAG, IPC_INTELLI_TIMEOUT_NONE);
        }
        if (!f) { ump_error("[webrtc] queue full, drop frame\n"); return; }

        // Prefix SPS/PPS in front of an IDR.
        if ((img->data()[4] & 0x1F) == 5 && info->codecSpecific.H264.sps_pps_len > 0) {
            uint32_t h264_size = img->size();
            uint32_t sps_pps_size = info->codecSpecific.H264.sps_pps_len;
            uint8_t* data = (uint8_t*)malloc(h264_size + sps_pps_size);
            if (info->codecSpecific.H264.sps_pps)
                std::memcpy(data, info->codecSpecific.H264.sps_pps, sps_pps_size);
            std::memcpy(data + sps_pps_size, img->data(), h264_size);
            video_common::EncodedImageBuffer* adapter =
                new video_common::EncodedImageBuffer(data, h264_size + sps_pps_size);
            free(data);
            adapter->AddRef();
            f->h264_frame = adapter;
        } else {
            img->AddRef();
            f->h264_frame = img;
        }
        IntelliPutBuffer(&h264_buf_hndl_, f, IPC_INTELLI_FULL_BUFFER_FLAG);
    }

    VideoWebrtcPush::TH264Buffer* VideoWebrtcPush::DequeueLatestFrame() {
        return (TH264Buffer*)IntelliGetBuffer(&h264_buf_hndl_, IPC_INTELLI_FULL_BUFFER_FLAG, IPC_INTELLI_TIMEOUT_NONE);
    }

    void VideoWebrtcPush::OnRatesChanged(uint32_t bitrate_bps, uint32_t framerate_fps) {
        if (!video_encode_) return;
        video_common::RateControlParameters p;
        p.target_bitrate = bitrate_bps;
        p.framerate_fps = framerate_fps;
        video_encode_->SetRates(p);
    }

    void VideoWebrtcPush::OnViewerConnected(const std::string& peer_id) {
        ump_info("[webrtc] viewer (peer_id=%s) connected, requesting keyframe\n",
                 peer_id.c_str());
        // A new viewer needs a keyframe to start decoding. Force the hardware
        // encoder to emit an IDR as soon as possible.
        if (video_encode_) {
            video_encode_->RequestKeyFrame();
        }
    }

    void VideoWebrtcPush::OnViewerDisconnected(const std::string& peer_id) {
        ump_info("[webrtc] viewer (peer_id=%s) disconnected\n", peer_id.c_str());
    }

    // =====================================================================
    //  Audio pipeline (device -> browser)
    //  ALSA @16kHz mono -> ringbuffer -> resample 16k->8k -> G.711 A-law
    //  -> transport_->SendAudioFrame()
    // =====================================================================
    int32_t VideoWebrtcPush::AudioInit() {
        audio_ring_buf_ = ringbuffer_create(AUDIO_RING_BUFFER_SIZE);
        if (!audio_ring_buf_) {
            ump_error("[webrtc-audio] ringbuffer_create failed\n");
            return -1;
        }
        if (!audio_resampler_.Init(AUDIO_INPUT_CHANNELS, AUDIO_INPUT_RATE, AUDIO_OUTPUT_RATE)) {
            ump_error("[webrtc-audio] resampler init failed\n");
            ringbuffer_destroy(audio_ring_buf_);
            audio_ring_buf_ = nullptr;
            return -1;
        }
        // 创建降噪处理器（0=关闭，1=Low 2=Moderate 3=High 4=VeryHigh）
        if (audio_processor_) {
            audio_processor_destroy(audio_processor_);
            audio_processor_ = nullptr;
        }
        if (audio_noise_level_ != 0) {
            audio_processor_ = audio_processor_create(AUDIO_INPUT_RATE, audio_noise_level_);
            if (audio_processor_) {
                ump_info("[webrtc-audio] noise processor ready, level=[%d] (1=Low 2=Moderate 3=High 4=VeryHigh)\n",
                         audio_noise_level_);
            } else {
                ump_warn("[webrtc-audio] audio_processor_create(level=%d) failed, running without noise reduction\n",
                         audio_noise_level_);
            }
        } else {
            ump_info("[webrtc-audio] noise reduction disabled (audio_noise_level=0)\n");
        }
        ump_info("[webrtc-audio] init OK (in=%dHz ch=%d -> out=%dHz)\n",
                 AUDIO_INPUT_RATE, AUDIO_INPUT_CHANNELS, AUDIO_OUTPUT_RATE);
        return 0;
    }

    void VideoWebrtcPush::AudioUninit() {
        if (audio_ring_buf_) {
            ringbuffer_destroy(audio_ring_buf_);
            audio_ring_buf_ = nullptr;
        }
        if (audio_processor_) {
            audio_processor_destroy(audio_processor_);
            audio_processor_ = nullptr;
        }
        audio_resampler_.Reset();
    }

    int32_t VideoWebrtcPush::AudioStart() {
        if (audio_running_.load(std::memory_order_acquire)) return 0;

        if (!audio_ring_buf_) {
            if (AudioInit() != 0) return -1;
        }

        audio_recorder_.RegisterCallback([this](const int16_t* data, size_t bytes,
                                                int sample_rate, int channels) {
            AudioCaptureCallback(data, bytes, sample_rate, channels);
        });

        if (!audio_recorder_.Init(AUDIO_DEVICE, AUDIO_INPUT_RATE, AUDIO_INPUT_CHANNELS,
                                  AUDIO_FRAME_MS, 64)) {
            ump_error("[webrtc-audio] AlsaRecorder init failed: %s\n",
                      audio_recorder_.GetLastError().c_str());
            return -1;
        }
        if (!audio_recorder_.Start()) {
            ump_error("[webrtc-audio] AlsaRecorder start failed: %s\n",
                      audio_recorder_.GetLastError().c_str());
            audio_recorder_.CloseDevice();
            return -1;
        }

        audio_quit_.store(false, std::memory_order_release);
        audio_running_.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> guard(audio_mutex_);
        audio_thread_ = std::thread([this] {
            ump_info("[webrtc-audio] AudioPushProcess thread started\n");
            while (AudioPushProcess()) {}
            ump_info("[webrtc-audio] AudioPushProcess thread exited\n");
        });
        ump_info("[webrtc-audio] started\n");
        return 0;
    }

    void VideoWebrtcPush::AudioStop() {
        if (!audio_running_.load(std::memory_order_acquire)) return;
        audio_quit_.store(true, std::memory_order_release);
        audio_recorder_.Stop();
        {
            std::lock_guard<std::mutex> guard(audio_mutex_);
            if (audio_thread_.joinable()) audio_thread_.join();
        }
        audio_recorder_.CloseDevice();
        audio_running_.store(false, std::memory_order_release);
        if (audio_ring_buf_) ringbuffer_reset(audio_ring_buf_);
        ump_info("[webrtc-audio] stopped\n");
    }

    void VideoWebrtcPush::AudioCaptureCallback(const int16_t* data, size_t bytes,
                                               int sample_rate, int channels) {
        (void)sample_rate;
        if (!audio_ring_buf_ || !audio_running_.load(std::memory_order_acquire)) return;

        size_t total_samples = bytes / sizeof(int16_t);
        size_t mono_samples = total_samples / channels;

        int16_t mono_buf[4096];
        if (mono_samples > sizeof(mono_buf) / sizeof(int16_t)) return;

        if (channels == 1) {
            std::memcpy(mono_buf, data, mono_samples * sizeof(int16_t));
        } else {
            for (size_t i = 0; i < mono_samples; ++i) {
                int32_t sum = 0;
                for (int c = 0; c < channels; ++c)
                    sum += data[i * channels + c];
                mono_buf[i] = (int16_t)(sum / channels);
            }
        }

        unsigned int mono_bytes = mono_samples * sizeof(int16_t);
        ringbuffer_put(audio_ring_buf_, (const char*)mono_buf, mono_bytes);
    }

    bool VideoWebrtcPush::AudioPushProcess() {
        if (audio_quit_.load(std::memory_order_acquire)) return false;

        // 20ms @ 16kHz mono = 320 samples = 640 bytes
        const uint32_t frame_bytes = AUDIO_INPUT_RATE * AUDIO_FRAME_MS * 2 / 1000;

        if (!audio_ring_buf_) return true;

        unsigned int ring_len = ringbuffer_len(audio_ring_buf_);
        if (ring_len < frame_bytes) {
            usleep(1000);
            return true;
        }

        std::vector<int16_t> pcm_in(frame_bytes / sizeof(int16_t));
        unsigned int got = ringbuffer_get(audio_ring_buf_, (char*)pcm_in.data(), frame_bytes);
        if (got < frame_bytes) return true;

        // 降噪处理（10ms 一帧，正好 = AUDIO_INPUT_RATE/100 = 160 samples；AEC 关闭 render_pcm 传 nullptr）
        std::vector<int16_t> denoised;
        const int16_t* proc_src = pcm_in.data();
        if (audio_processor_) {
            denoised.resize(pcm_in.size());
            audio_processor_process(audio_processor_, pcm_in.data(), nullptr, denoised.data());
            proc_src = denoised.data();
        }

        // Resample 16kHz -> 8kHz
        std::vector<int16_t> pcm_out;
        size_t output_samples = audio_resampler_.Process(proc_src, pcm_in.size(), pcm_out);
        if (output_samples == 0) {
            ump_warn("[webrtc-audio] resample produced 0 samples\n");
            return true;
        }

        // G.711 A-law encode
        std::vector<uint8_t> g711_data = g711_encoder_.Encode(pcm_out.data(), output_samples);
        if (g711_data.empty()) {
            ump_warn("[webrtc-audio] g711 encode empty\n");
            return true;
        }

        // Fan-out to all connected viewers via the transport.
        if (transport_) transport_->SendAudioFrame(g711_data.data(), g711_data.size());

        static uint32_t audio_frame_cnt = 0;
        if (++audio_frame_cnt % 50 == 0) {
            ump_info("[webrtc-audio] frame #%u ring=%u pcm_in=%zu pcm_out=%zu g711=%zu\n",
                     audio_frame_cnt, ring_len, pcm_in.size(), output_samples, g711_data.size());
        }
        return true;
    }

} // namespace ecovacs
