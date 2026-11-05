#include "api_video_webrtc_push.h"

#include <log.h>
#include "osp.h"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
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
        if (video_encode_) {
            tcfg.video_width = (uint32_t)video_encode_->GetWidth();
            tcfg.video_height = (uint32_t)video_encode_->GetHeight();
            tcfg.video_fps = video_encode_->GetMaxFramerate();
        }

        transport_ = WebrtcTransport::Create();
        if (!transport_) {
            ump_error("[webrtc] WebrtcTransport::Create failed\n");
            push_started_.store(false);
            return -1;
        }
        transport_->Init(tcfg);
        transport_->SetVideoEncoder(this);
        transport_->SetViewerListener(this);
        // Receive-side: bind audio/video receivers for browser -> device media.
        transport_->SetAudioReceiver(this);
        transport_->SetVideoReceiver(this);
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
            while (!quit_.load(std::memory_order_acquire)) {
                TH264Buffer* f = DequeueLatestFrame();
                if (f && f->h264_frame) {
                    transport_->SendVideoFrame(
                        f->h264_frame->data(), f->h264_frame->size(),
                        H264IsKeyframe(f->h264_frame->data(), f->h264_frame->size()));
                    f->h264_frame->RelRef();
                    f->h264_frame = nullptr;
                    IntelliPutBuffer(&h264_buf_hndl_, f, IPC_INTELLI_EMPTY_BUFFER_FLAG);
                } else {
                    usleep(1000);
                }
            }
        });

        // Receive-side audio thread: forwards mixed PCM to the audio module.
        recv_audio_thread_ = std::thread(&VideoWebrtcPush::RecvAudioThread, this);

        return 0;
    }

    int32_t VideoWebrtcPush::StopPush() {
        bool expected = true;
        if (!push_started_.compare_exchange_strong(expected, false, std::memory_order_acq_rel))
            return 0;
        quit_.store(true, std::memory_order_release);
        AudioStop();
        if (send_thread_.joinable()) send_thread_.join();
        recv_audio_q_cv_.notify_all();  // wake RecvAudioThread so it sees quit_
        if (recv_audio_thread_.joinable()) recv_audio_thread_.join();
        if (transport_) { transport_->Stop(); transport_.reset(); }

        // Cleanup receive-side resources.  recv_audio_sock_ is owned by
        // RecvAudioThread and closed when it exits, so we only clear the
        // PCM queue here.
        {
            std::lock_guard<std::mutex> lk(recv_audio_q_mu_);
            std::queue<PcmFrame> empty;
            std::swap(recv_audio_q_, empty);
        }
        {
            std::lock_guard<std::mutex> vlk(recv_video_mu_);
            for (auto& [pid, f] : recv_video_files_) { if (f) fclose(f); }
            recv_video_files_.clear();
        }

        return 0;
    }

    void VideoWebrtcPush::OnFrame(video_common::EncodedImageBuffer* img,
                                  const video_common::CodecSpecificInfo* info) {
        if (!img || !info) return;
        if (!push_started_.load(std::memory_order_acquire)) return;
        if (info->codecType != video_common::kVideoCodecH264) return;

        // Track average encoded frame size (EMA) of P-frames only so that
        // large IDR frames don't skew the estimate and over-penalize fps.
        if (info->codecSpecific.H264.idr_frame == false) {
            size_t cur = avg_frame_size_.load(std::memory_order_relaxed);
            size_t next = (cur == 0) ? img->size()
                                     : (cur * 9 + img->size()) / 10;
            avg_frame_size_.store(next, std::memory_order_relaxed);
        }

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
        // framerate_fps from libwebrtc is the observed source (black-frame)
        // rate, not a network-adaptive suggestion.  Derive a sustainable fps
        // from the target bitrate and the measured average encoded-frame size.
        uint32_t max_fps = video_encode_->GetMaxFramerate();
        if (max_fps == 0) max_fps = 25;
        uint32_t target_fps = max_fps;
        size_t avg_size = avg_frame_size_.load(std::memory_order_relaxed);
        if (avg_size > 0) {
            uint32_t calc = (uint32_t)(bitrate_bps / (avg_size * 8));
            target_fps = std::min(calc, max_fps);
            if (target_fps < 5) target_fps = 5; // floor, keep video playable
        }
        video_common::RateControlParameters p;
        p.target_bitrate = bitrate_bps;
        p.framerate_fps = target_fps;
        video_encode_->SetRates(p);
    }

    void VideoWebrtcPush::OnKeyFrameRequested() {
        if (!video_encode_) return;
        // The hardware H.264 encoder is shared by all viewers.  When several
        // viewers send PLI/FIR at the same time (e.g. after a shared network
        // blip) we must not hammer it with back-to-back IDR requests.  A
        // single IDR is broadcast to every viewer via SendVideoFrame, so one
        // request every 500ms is enough to let everyone recover.
        int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        int64_t last = last_keyframe_req_us_.load(std::memory_order_relaxed);
        static constexpr int64_t kMinIntervalUs = 500 * 1000;
        if (now_us - last < kMinIntervalUs) return;
        if (!last_keyframe_req_us_.compare_exchange_strong(last, now_us,
                std::memory_order_relaxed)) return;
        ump_info("[webrtc] OnKeyFrameRequested -> hardware encoder\n");
        video_encode_->RequestKeyFrame();
    }

    void VideoWebrtcPush::OnViewerConnected(const std::string& peer_id) {
        ump_info("[webrtc] viewer (peer_id=%s) connected, requesting keyframe\n",
                 peer_id.c_str());
        // A new viewer needs a keyframe to start decoding.  Go through the
        // throttled OnKeyFrameRequested() entry so rapid viewer joins (and
        // PLI/FIR from existing viewers) share the same 500ms coalescing
        // window and cannot overwhelm the shared hardware encoder.
        this->OnKeyFrameRequested();
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
        // AEC reference ringbuffer: holds the speaker-loopback channel
        // captured from ALSA (same frame as mic, hardware-aligned).
        audio_ref_buf_ = ringbuffer_create(AUDIO_RING_BUFFER_SIZE);
        if (!audio_ref_buf_) {
            ump_error("[webrtc-audio] ref ringbuffer_create failed\n");
            ringbuffer_destroy(audio_ring_buf_);
            audio_ring_buf_ = nullptr;
            return -1;
        }
        if (!audio_resampler_.Init(AUDIO_INPUT_CHANNELS, AUDIO_INPUT_RATE, AUDIO_OUTPUT_RATE)) {
            ump_error("[webrtc-audio] resampler init failed\n");
            ringbuffer_destroy(audio_ring_buf_);
            audio_ring_buf_ = nullptr;
            ringbuffer_destroy(audio_ref_buf_);
            audio_ref_buf_ = nullptr;
            return -1;
        }
        // 创建音频处理器（AEC + 降噪）。
        // - noise_level > 0: 开启降噪（1=Low ... 4=VeryHigh）
        // - aec_enabled_: 开启 AEC（即使 noise_level=0，AEC 仍工作）
        // audio_processor_create(sample_rate, noise_level<=0) = 纯 AEC 模式（关 AGC+NS）
        if (audio_processor_) {
            audio_processor_destroy(audio_processor_);
            audio_processor_ = nullptr;
        }
        if (aec_enabled_ || audio_noise_level_ != 0) {
            audio_processor_ = audio_processor_create(AUDIO_INPUT_RATE, audio_noise_level_);
            if (audio_processor_) {
                ump_info("[webrtc-audio] processor ready: aec=%s, noise_level=%d\n",
                         aec_enabled_ ? "on" : "off", audio_noise_level_);
            } else {
                ump_warn("[webrtc-audio] audio_processor_create failed (aec=%s, level=%d), running raw\n",
                         aec_enabled_ ? "on" : "off", audio_noise_level_);
            }
        } else {
            ump_info("[webrtc-audio] AEC and noise reduction both disabled\n");
        }
        ump_info("[webrtc-audio] init OK (in=%dHz ch=%d -> out=%dHz, aec=%s)\n",
                 AUDIO_INPUT_RATE, AUDIO_INPUT_CHANNELS, AUDIO_OUTPUT_RATE,
                 aec_enabled_ ? "on" : "off");
        return 0;
    }

    void VideoWebrtcPush::AudioUninit() {
        if (audio_ring_buf_) {
            ringbuffer_destroy(audio_ring_buf_);
            audio_ring_buf_ = nullptr;
        }
        if (audio_ref_buf_) {
            ringbuffer_destroy(audio_ref_buf_);
            audio_ref_buf_ = nullptr;
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

        if (!audio_recorder_.Init(audio_device_.c_str(), AUDIO_INPUT_RATE,
                                  audio_capture_channels_, AUDIO_FRAME_MS, 64)) {
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
            while (AudioPushProcess()) {}
        });
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
        if (audio_ref_buf_) ringbuffer_reset(audio_ref_buf_);
    }

    void VideoWebrtcPush::AudioCaptureCallback(const int16_t* data, size_t bytes,
                                               int sample_rate, int channels) {
        (void)sample_rate;
        if (!audio_ring_buf_ || !audio_running_.load(std::memory_order_acquire)) return;

        if (channels <= 0) return;
        size_t total_samples = bytes / sizeof(int16_t);
        size_t mono_samples = total_samples / channels;

        int16_t mic_buf[4096];  // 麦克风声道 10ms @16kHz = 160 samples
        int16_t ref_buf[4096];  // 参考声道 10ms @16kHz = 160 samples
        if (mono_samples > sizeof(mic_buf) / sizeof(int16_t)) return;

        // 声道边界校验：mic/ref index 必须在有效范围内
        int mic_ch = (channels > audio_mic_channel_) ? audio_mic_channel_ : 0;
        int ref_ch = (channels > audio_ref_channel_) ? audio_ref_channel_ : (channels - 1);

        if (channels == 1) {
            // 单声道兜底：整帧即 mic，参考为空（AEC 无效，等同透传）
            std::memcpy(mic_buf, data, mono_samples * sizeof(int16_t));
            if (audio_ref_buf_)
                ringbuffer_reset(audio_ref_buf_);
        } else {
            // 多声道：抽取指定 index 的 mic 与 ref 声道（同一份 data，天然对齐）
            for (size_t i = 0; i < mono_samples; ++i) {
                mic_buf[i] = data[i * channels + mic_ch];
                ref_buf[i] = data[i * channels + ref_ch];
            }
        }

        // 存 mic 到主 ringbuffer，ref 到参考 ringbuffer（扬声器回环，AEC 用）
        unsigned int mono_bytes = mono_samples * sizeof(int16_t);
        ringbuffer_put(audio_ring_buf_, (const char*)mic_buf, mono_bytes);
        if (channels > 1 && audio_ref_buf_) {
            ringbuffer_put(audio_ref_buf_, (const char*)ref_buf, mono_bytes);
        }
    }

    bool VideoWebrtcPush::AudioPushProcess() {
        if (audio_quit_.load(std::memory_order_acquire)) return false;

        // 10ms @ 16kHz mono = 160 samples = 320 bytes
        const uint32_t frame_bytes = AUDIO_INPUT_RATE * AUDIO_FRAME_MS * 2 / 1000;

        if (!audio_ring_buf_) return true;

        unsigned int ring_len = ringbuffer_len(audio_ring_buf_);
        unsigned int ref_ring_len = audio_ref_buf_ ? ringbuffer_len(audio_ref_buf_) : 0;
        // AEC 开启时，mic 与 ref 必须同时足帧（保持严格对齐）；不足则等下一拍
        if (ring_len < frame_bytes || (aec_enabled_ && ref_ring_len < frame_bytes)) {
            usleep(1000);
            return true;
        }

        // 读 mic 帧
        std::vector<int16_t> pcm_in(frame_bytes / sizeof(int16_t));
        unsigned int got = ringbuffer_get(audio_ring_buf_, (char*)pcm_in.data(), frame_bytes);
        if (got < frame_bytes) return true;

        // AEC 开启时读对应 ref 帧
        std::vector<int16_t> pcm_ref;
        const int16_t* ref_ptr = nullptr;
        if (aec_enabled_ && audio_ref_buf_) {
            pcm_ref.assign(frame_bytes / sizeof(int16_t), 0);
            if (ringbuffer_get(audio_ref_buf_, (char*)pcm_ref.data(), frame_bytes) < frame_bytes) {
                return true;  // ref 不足，丢弃该帧保持对齐
            }
            ref_ptr = pcm_ref.data();
        }

        std::vector<int16_t> denoised;
        const int16_t* proc_src = pcm_in.data();
        if (audio_processor_) {
            denoised.resize(pcm_in.size());
            audio_processor_process(audio_processor_, pcm_in.data(), ref_ptr, denoised.data());
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

        return true;
    }

    // =====================================================================
    //  Receive-side (browser -> device)
    //  Audio: ump already mixed all viewers' PCM via AudioMixer; we enqueue
    //         it and RecvAudioThread() forwards it to the audio module over TCP.
    //  Video: save H.264 bitstream to per-peer file.
    // =====================================================================

    void VideoWebrtcPush::OnAudioFrame(const std::string& peer_id,
                                       const int16_t* pcm, size_t samples,
                                       int sample_rate, int channels) {
        if (!pcm || samples == 0) return;
        (void)peer_id;
        // The header sent to the audio module hardcodes 48kHz mono.  Warn
        // (throttled) if the ump layer ever changes the format so the
        // mismatch is visible instead of silently mis-playing.
        if (sample_rate != 48000 || channels != 1) {
            static std::atomic<uint32_t> warn_cnt{0};
            if (warn_cnt.fetch_add(1, std::memory_order_relaxed) % 500 == 0)
                ump_warn("[webrtc-audio] unexpected PCM format rate=%d ch=%d (expected 48000/1)\n",
                         sample_rate, channels);
        }
        // The ump layer already mixed all viewers' audio via AudioMixer.
        // We only enqueue the mixed PCM here; the actual TCP send to the
        // audio module happens in RecvAudioThread() so this callback
        // (invoked from ump's MixThread every 10ms) can never block on
        // network I/O.
        PcmFrame frame;
        frame.samples.assign(pcm, pcm + samples);
        std::lock_guard<std::mutex> lk(recv_audio_q_mu_);
        if (recv_audio_q_.size() >= kPcmQueueMax)
            recv_audio_q_.pop();  // drop oldest to stay real-time
        recv_audio_q_.push(std::move(frame));
        recv_audio_q_cv_.notify_one();
    }

    void VideoWebrtcPush::RecvAudioThread() {
        static constexpr const char* AUDIO_MODULE_HOST = "127.0.0.1";
        static constexpr int AUDIO_MODULE_PORT = 8888;
        static constexpr int kConnectTimeoutMs = 500;
        static constexpr int kIoTimeoutMs = 500;       // per-call poll / recv timeout
        static constexpr int kSendDeadlineMs = 1000;   // hard cap for one send_all()
        static constexpr int kReconnectBackoffMs = 300;
        const char* kHeader = "{\"format\":\"pcm\",\"audio_type\":1,\"rate\":48000,"
                              "\"bits_depth\":16,\"channel\":1,\"endian\":0,\"sign\":1,"
                              "\"float\":0,\"cli_type\":\"call\"}\n";
        const size_t kHeaderLen = strlen(kHeader);
        using clock = std::chrono::steady_clock;

        // Send all bytes; returns false on hard failure (connection broken or
        // deadline exceeded).  Uses MSG_DONTWAIT per call so SO_SNDTIMEO is
        // irrelevant; EAGAIN waits via poll(POLLOUT) up to the deadline.
        auto send_all = [](int fd, const void* buf, size_t len) -> bool {
            const uint8_t* p = static_cast<const uint8_t*>(buf);
            const auto deadline = clock::now() + std::chrono::milliseconds(kSendDeadlineMs);
            size_t off = 0;
            while (off < len) {
                ssize_t n = send(fd, p + off, len - off, MSG_NOSIGNAL | MSG_DONTWAIT);
                if (n > 0) { off += (size_t)n; continue; }
                if (n < 0 && errno == EINTR) continue;
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    auto now = clock::now();
                    if (now >= deadline) return false;
                    int remain = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                                     deadline - now).count();
                    struct pollfd pfd{fd, POLLOUT, 0};
                    if (poll(&pfd, 1, remain) <= 0) return false;
                    if (!(pfd.revents & POLLOUT)) return false;  // POLLERR / POLLHUP
                    continue;
                }
                return false;  // EPIPE / ECONNRESET / other hard error
            }
            return true;
        };

        // connect() with timeout via non-blocking socket + poll(POLLOUT).
        auto connect_module = [&]() -> int {
            int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
            if (fd < 0) return -1;
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(AUDIO_MODULE_PORT);
            inet_pton(AF_INET, AUDIO_MODULE_HOST, &addr.sin_addr);
            int rc = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
            if (rc < 0 && errno != EINPROGRESS) { close(fd); return -1; }
            if (rc < 0) {
                struct pollfd pfd{fd, POLLOUT, 0};
                if (poll(&pfd, 1, kConnectTimeoutMs) <= 0) { close(fd); return -1; }
                int err = 0; socklen_t elen = sizeof(err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err) {
                    close(fd); return -1;
                }
            }
            // Switch back to blocking so SO_RCVTIMEO limits the ack recv().
            // send_all() still uses MSG_DONTWAIT per call, so send never blocks.
            int fl = fcntl(fd, F_GETFL, 0);
            if (fl < 0 || fcntl(fd, F_SETFL, fl & ~O_NONBLOCK) < 0) {
                close(fd); return -1;
            }
            struct timeval tv{0, kIoTimeoutMs * 1000};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            // Send header (loop for partial writes).
            if (!send_all(fd, kHeader, kHeaderLen)) { close(fd); return -1; }

            // Wait for the audio module's {"ret":"ok"} ack before sending PCM.
            // Loop-read because a single recv() may return a partial JSON.
            // Stop as soon as we see "}" so a {"ret":"fail"} response does
            // not block the full SO_RCVTIMEO waiting for more data.
            char ack[128] = {0};
            size_t got = 0;
            bool ok = false;
            while (got < sizeof(ack) - 1) {
                ssize_t rn = recv(fd, ack + got, sizeof(ack) - 1 - got, 0);
                if (rn <= 0) break;
                got += (size_t)rn;
                ack[got] = '\0';
                if (strstr(ack, "\"ret\":\"ok\"")) { ok = true; break; }
                if (strchr(ack, '}')) break;  // complete (failed) response, no more data
            }
            if (!ok) {
                ump_warn("[webrtc-audio] audio module ack unexpected: %s\n", ack);
                close(fd); return -1;
            }
            ump_info("[webrtc-audio] audio module connected, ack ok\n");
            return fd;
        };

        // Drop all but the latest queued frame.  Used after a send stall so
        // we don't dump ~100ms of stale PCM at once when the link recovers.
        auto drain_to_latest = [this]() {
            std::lock_guard<std::mutex> lk(recv_audio_q_mu_);
            while (recv_audio_q_.size() > 1) recv_audio_q_.pop();
        };

        bool reconnect_backoff = false;
        while (!quit_.load(std::memory_order_acquire)) {
            PcmFrame frame;
            {
                std::unique_lock<std::mutex> lk(recv_audio_q_mu_);
                recv_audio_q_cv_.wait_for(lk, std::chrono::milliseconds(100), [this] {
                    return quit_.load(std::memory_order_acquire) || !recv_audio_q_.empty();
                });
                if (quit_.load(std::memory_order_acquire)) break;
                if (recv_audio_q_.empty()) continue;
                frame = std::move(recv_audio_q_.front());
                recv_audio_q_.pop();
            }
            if (frame.samples.empty()) continue;

            if (reconnect_backoff) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kReconnectBackoffMs));
                reconnect_backoff = false;
            }
            if (quit_.load(std::memory_order_acquire)) break;

            if (recv_audio_sock_ < 0) {
                recv_audio_sock_ = connect_module();
                if (recv_audio_sock_ < 0) {
                    // Connect failed — drop stale frames so we don't dump
                    // ~100ms of old PCM in a burst once the link recovers.
                    reconnect_backoff = true;
                    drain_to_latest();
                    continue;
                }
            }

            const size_t bytes = frame.samples.size() * sizeof(int16_t);
            const uint8_t* p = reinterpret_cast<const uint8_t*>(frame.samples.data());
            if (!send_all(recv_audio_sock_, p, bytes)) {
                close(recv_audio_sock_);
                recv_audio_sock_ = -1;
                reconnect_backoff = true;
                drain_to_latest();
            }
        }
        if (recv_audio_sock_ >= 0) {
            close(recv_audio_sock_);
            recv_audio_sock_ = -1;
        }
    }

    void VideoWebrtcPush::VideoRecvSaveFrame(const std::string& peer_id,
                                             const uint8_t* data, size_t size,
                                             bool keyframe) {
        std::lock_guard<std::mutex> lk(recv_video_mu_);
        FILE*& f = recv_video_files_[peer_id];
        if (!f) {
            std::string path = std::string("/tmp/webrtc_recv_") + peer_id + ".h264";
            f = fopen(path.c_str(), "wb");
            if (f) ump_info("[webrtc-recv] video save -> %s\n", path.c_str());
        }
        if (f) {
            fwrite(data, 1, size, f);
            fflush(f);
        }
    }

    void VideoWebrtcPush::OnVideoEncodedFrame(const std::string& peer_id,
                                              const uint8_t* h264_data, size_t size,
                                              bool is_keyframe, int64_t /*timestamp_us*/) {
        if (!h264_data || size == 0) return;
        VideoRecvSaveFrame(peer_id, h264_data, size, is_keyframe);
    }

} // namespace ecovacs
