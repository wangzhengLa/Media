#ifndef WEBRTC_TRANSPORT_H_
#define WEBRTC_TRANSPORT_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

// WebRTC transport wrapper (official trimmed libwebrtc stack).
//
// Encapsulates PeerConnection / SDP / ICE / WebSocket signaling / video
// frame injection so upper layers (video push) only deal with a clean
// interface.  This header deliberately includes NO WebRTC headers: the
// .cpp is compiled with special flags (C++20, -fno-rtti) required by
// libwebrtc.a.
//
// Design (inspired by AWS KVS WebRTC SDK master/viewer model):
//   - The device acts as MASTER (callee): it waits for incoming offers
//     from viewers (phone / browser) and replies with answers.
//   - Each viewer gets its own PeerConnection + video encoder instance,
//     so multiple viewers can watch simultaneously.
//   - A single encoded video frame is fanned out to every connected
//     viewer's encoder via SendVideoFrame().
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
    //   peer_id: signaling-layer identifier of the viewer (matches the
    //            "peer_id" field in the offer / bye messages).
    class IWebrtcViewerListener
    {
    public:
        virtual ~IWebrtcViewerListener() = default;
        virtual void OnViewerConnected(const std::string& peer_id) = 0;
        virtual void OnViewerDisconnected(const std::string& peer_id) = 0;
    };

    // Audio frame receiver (browser -> device).
    //
    // libwebrtc decodes the incoming audio RTP from the browser and
    // delivers the PCM samples here.  The upper layer (e.g. audio player
    // / AEC reference) implements this interface to consume the decoded
    // audio.
    //
    // TODO: not implemented yet.  The call path will be:
    //   libwebrtc audio receive track -> OnData -> AudioSinkInterface
    //   -> deliver to this callback (decoded PCM, typically 48kHz or 16kHz).
    class IWebrtcAudioReceiver
    {
    public:
        virtual ~IWebrtcAudioReceiver() = default;
        // Decoded PCM audio from a viewer.
        //   peer_id    : which viewer sent the audio (signaling peer_id)
        //   pcm        : signed 16-bit PCM samples, interleaved if stereo
        //   samples    : number of samples per channel
        //   sample_rate: sample rate in Hz (e.g. 48000, 16000)
        //   channels   : number of channels (1 = mono, 2 = stereo)
        virtual void OnAudioFrame(const std::string& peer_id,
                                  const int16_t* pcm,
                                  size_t samples,
                                  int sample_rate,
                                  int channels) = 0;
    };

    // Video frame receiver (browser -> device).
    //
    // On embedded the incoming video is delivered as H.264 encoded frames
    // (NOT decoded) so the upper layer can feed them into the hardware
    // decoder (cedarv / mpp).  libwebrtc's internal software decoder is
    // bypassed via the encoded-frame callback.
    //
    // TODO: not implemented yet.  The call path will be:
    //   libwebrtc video receive stream -> encoded frame callback
    //   -> webrtc::EncodedImage (H.264 Annex-B NALs)
    //   -> deliver to this callback.
    class IWebrtcVideoReceiver
    {
    public:
        virtual ~IWebrtcVideoReceiver() = default;
        // Encoded H.264 frame from a viewer.
        //   peer_id     : which viewer sent the video (signaling peer_id)
        //   h264_data   : H.264 bitstream in Annex-B format (with start
        //                 codes); may contain multiple NAL units
        //   size        : byte length of h264_data
        //   is_keyframe : true if this frame is an IDR / keyframe
        //   timestamp_us: frame timestamp in microseconds
        virtual void OnVideoEncodedFrame(const std::string& peer_id,
                                         const uint8_t* h264_data,
                                         size_t size,
                                         bool is_keyframe,
                                         int64_t timestamp_us) = 0;
    };

    struct WebrtcTransportConfig
    {
        // WebSocket signaling: ws://host:port/ws?room=name
        std::string signaling_url = "ws://10.10.103.194:8080/ws?room=room1";
        std::string stun_url;
        std::string turn_url;
        std::string turn_username;
        std::string turn_password;
        // Maximum number of simultaneous viewers.  Each viewer costs one
        // PeerConnection + encoder, so keep this modest on embedded.
        int max_viewers = 4;
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
        // TODO: not implemented yet, interface reserved.
        virtual void SetAudioReceiver(IWebrtcAudioReceiver* receiver) = 0;

        // Optional: receive encoded H.264 video from viewers (browser -> device).
        // TODO: not implemented yet, interface reserved.
        virtual void SetVideoReceiver(IWebrtcVideoReceiver* receiver) = 0;

        // Inject an encoded video frame. The frame is fanned out to every
        // connected viewer's RTP packetizer.  Current codec: H.264 Annex-B.
        virtual void SendVideoFrame(const uint8_t* data, size_t size, bool keyframe) = 0;

        // Inject a pre-encoded audio frame.  Fanned out to every connected
        // viewer.  Current codec: G.711 PCMA (10ms @ 8kHz mono = 80 bytes).
        virtual void SendAudioFrame(const uint8_t* data, size_t size) = 0;
    };

} // namespace ecovacs

#endif // WEBRTC_TRANSPORT_H_
