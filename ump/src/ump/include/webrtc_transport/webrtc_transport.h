#ifndef WEBRTC_TRANSPORT_H_
#define WEBRTC_TRANSPORT_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

// WebRTC transport wrapper (official trimmed libwebrtc stack).
//
// Encapsulates PeerConnection / SDP / ICE / WebSocket signaling / H264
// frame injection so upper layers (video push) only deal with a clean
// interface.  This header deliberately includes NO WebRTC headers: the
// .cpp is compiled with special flags (C++20, -fno-rtti) required by
// libwebrtc.a.
//
// Design (inspired by AWS KVS WebRTC SDK master/viewer model):
//   - The device acts as MASTER (callee): it waits for incoming offers
//     from viewers (phone / browser) and replies with answers.
//   - Each viewer gets its own PeerConnection + H264 encoder instance,
//     so multiple viewers can watch simultaneously.
//   - A single encoded H264 frame is fanned out to every connected
//     viewer's encoder via SendH264Frame().
namespace ecovacs
{
    // Bandwidth estimate callback target. The hardware encoder implements
    // this so WebRTC's GCC can drive the real bitrate.  With multiple
    // viewers the callback reports the minimum across all viewers so the
    // encoder never exceeds any single viewer's available bandwidth.
    class IWebrtcVideoEncoder
    {
    public:
        virtual ~IWebrtcVideoEncoder() = default;
        virtual void OnRatesChanged(uint32_t bitrate_bps, uint32_t framerate_fps) = 0;
    };

    // Viewer lifecycle events. Upper layers may use these to show
    // connection status or start/stop the encoder pipeline.
    class IWebrtcViewerListener
    {
    public:
        virtual ~IWebrtcViewerListener() = default;
        virtual void OnViewerConnected(int viewer_id) = 0;
        virtual void OnViewerDisconnected(int viewer_id) = 0;
    };

    // Audio frame receiver (browser -> device).
    //
    // When enable_audio_recv is true, libwebrtc decodes the incoming audio
    // RTP from the browser and delivers the PCM samples here.  The upper
    // layer (e.g. audio player / AEC reference) implements this interface
    // to consume the decoded audio.
    //
    // TODO: not implemented yet.  The call path will be:
    //   libwebrtc audio receive track -> OnData -> AudioSinkInterface
    //   -> deliver to this callback (decoded PCM, typically 48kHz or 16kHz).
    class IWebrtcAudioReceiver
    {
    public:
        virtual ~IWebrtcAudioReceiver() = default;
        // Decoded PCM audio from a viewer.
        //   viewer_id  : which viewer sent the audio
        //   pcm        : signed 16-bit PCM samples, interleaved if stereo
        //   samples    : number of samples per channel
        //   sample_rate: sample rate in Hz (e.g. 48000, 16000)
        //   channels   : number of channels (1 = mono, 2 = stereo)
        virtual void OnAudioFrame(int viewer_id,
                                  const int16_t* pcm,
                                  size_t samples,
                                  int sample_rate,
                                  int channels) = 0;
    };

    struct WebrtcTransportConfig
    {
        // WebSocket signaling: ws://host:port/ws?room=name
        std::string signaling_url = "ws://10.10.103.194:8080/ws?room=room1";
        std::string stun_url;
        std::string turn_url;
        std::string turn_username;
        std::string turn_password;
        int video_fps = 15;
        // Maximum number of simultaneous viewers.  Each viewer costs one
        // PeerConnection + encoder, so keep this modest on embedded.
        int max_viewers = 4;

        // ---- Audio ----
        // Device -> browser: when true, an audio track is added to every
        // PeerConnection and pre-encoded G.711 (PCMA) frames can be sent
        // via SendG711Frame().
        bool enable_audio = false;

        // Browser -> device (two-way call). When true, the transport
        // accepts incoming audio from viewers and delivers decoded PCM
        // to the IWebrtcAudioReceiver set via SetAudioReceiver().
        // TODO: not implemented yet, interface reserved.
        bool enable_audio_recv = false;
    };

    class WebrtcTransport
    {
    public:
        static std::unique_ptr<WebrtcTransport> Create();
        virtual ~WebrtcTransport() = default;

        virtual int32_t Init(const WebrtcTransportConfig& config) = 0;
        virtual int32_t Start() = 0;
        virtual int32_t Stop() = 0;
        virtual bool Started() const = 0;

        // Bind the hardware encoder for GCC rate-control callbacks.
        virtual void SetVideoEncoder(IWebrtcVideoEncoder* encoder) = 0;

        // Optional: viewer connect / disconnect notifications.
        virtual void SetViewerListener(IWebrtcViewerListener* listener) = 0;

        // Optional: receive decoded PCM audio from viewers (browser -> device).
        // Only active when config.enable_audio_recv == true.
        // TODO: not implemented yet, interface reserved.
        virtual void SetAudioReceiver(IWebrtcAudioReceiver* receiver) = 0;

        // Inject a raw Annex-B H264 access unit. The frame is fanned out
        // to every connected viewer's RTP packetizer.
        virtual void SendH264Frame(const uint8_t* data, size_t size, bool keyframe) = 0;

        // Inject a pre-encoded G.711 A-law (PCMA) frame (20ms @ 8kHz mono
        // = 160 bytes).  Fanned out to every connected viewer.  Only
        // active when config.enable_audio == true.
        virtual void SendG711Frame(const uint8_t* data, size_t size) = 0;
    };

} // namespace ecovacs

#endif // WEBRTC_TRANSPORT_H_
