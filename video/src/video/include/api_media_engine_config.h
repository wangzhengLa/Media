#ifndef API_MEDIA_ENGINE_CONFIG_H_
#define API_MEDIA_ENGINE_CONFIG_H_

#include <string>
#include <vector>

namespace ecovacs
{
    namespace ConfigModule
    {
        struct CaptureCapability
        {
            int32_t width;
            int32_t height;
            int32_t fps;
            int32_t device_index;
            int32_t csi_id;           // CSI 编号：-1=自动，0/1/2=指定 CSI
            std::string camera_type;
            std::string stream_property;
            std::string stream_type;  // "fr"=全分辨率主路, "ds"=降采样子路（默认 "fr"）
            std::string image_type;   // "YUYV"/"RGB"/"NV12"（默认 "YUYV"）
#ifdef ENABLE_ISP
            std::string isp_path; // "mainpath" 或 "selfpath"
            int32_t cam_id;       // 摄像头ID，用于 ISP API
            std::string iq_file;  // IQ 文件路径
#endif
        };

        struct CaptureConfig
        {
            int32_t device_number_;
            std::vector<CaptureCapability> capture_capabilitys_;
        };

        struct EncoderConfig
        {
            int32_t width;
            int32_t height;
            int32_t crop_x;
            int32_t crop_y;

            int32_t min_qp;
            int32_t init_qp;
            int32_t max_qp;

            int32_t min_bps;
            int32_t init_bps;
            int32_t max_bps;

            int32_t max_fps;
            int32_t gop;
        };

        struct PusherConfig
        {
            int32_t audio_noise_level;  // 音频降噪等级，取值 0=关闭，1=低，2=中，3=高，4=很高
            std::string audio_device = "default";  // ALSA 采集设备
            int32_t audio_capture_channels = 8;    // 采集声道数（AEC 需要多声道取参考）
            int32_t audio_mic_channel = 0;         // 麦克风所在声道 index（0-based，ch1）
            int32_t audio_ref_channel = 5;         // 参考声道 index（0-based，ch6）
            int32_t aec_enabled = 0;               // 总开关：0=关闭 AEC（沿用单声道透传），1=开启
        };

        struct AiDispatcherConfig
        {
        };

        struct Mp4RecoderConfig
        {
        };

        struct MediaEngineConfig
        {
            char unix_socket_path[128];

            CaptureConfig capture_cfg_;

            EncoderConfig encoder_cfg_;

            PusherConfig pusher_cfg_;

            AiDispatcherConfig ai_dispatcher_cfg_;

            Mp4RecoderConfig mp4_recoder_cfg_;
        };

    } // namespace ConfigModule

} // namespace ecovacs

#endif // API_MEDIA_ENGINE_CONFIG_H_