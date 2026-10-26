#ifndef API_MEDIA_ENGINE_H_
#define API_MEDIA_ENGINE_H_

#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include "api_media_engine_config.h"
#include "api_video_capture.h"
#include "api_video_encode.h"
#include "api_video_capture_broadcaster.h"
#include "api_video_encode_broadcaster.h"
#include "api_video_push.h"
#if defined(USE_LIVE555_RTSP)
#include "api_video_live555_push.h"
#else
#include "api_video_rtsp_push.h"
#endif
#if defined(ENABLE_LIBPEER)
#include "api_video_libpeer_push.h"
#endif
#if defined(ENABLE_WEBRTC)
#include "api_video_webrtc_push.h"
#endif
#include "api_jpeg_upload.h"
#include "api_video_ai_process.h"
#include "api_raw_video_save.h"
#include "api_jpeg_publish.h"
#include "api_message_dispatcher.h"

namespace ecovacs
{
    // 视频采集元件
    class VideoCaptureElement
    {
    public:
        VideoCaptureElement() = default;
        static std::unique_ptr<VideoCaptureElement> Create();
        ~VideoCaptureElement() = default;

        VideoCaptureElement(const VideoCaptureElement &) = delete;
        VideoCaptureElement &operator=(const VideoCaptureElement &) = delete;

    public:
        ConfigModule::CaptureCapability device_capture_capability_;
        std::shared_ptr<VideoCapture> video_capture_;
        std::shared_ptr<VideoCaptureBroadcaster> video_capture_broadcaster_;
#ifdef ENABLE_ISP
        int32_t cam_id_;
        std::string isp_path_;
#endif
    };

    // 视频JPEG抓拍元件
    class VideoPhotographElement
    {
    public:
        VideoPhotographElement() = default;
        static std::unique_ptr<VideoPhotographElement> Create();
        ~VideoPhotographElement() = default;

        VideoPhotographElement(const VideoPhotographElement &) = delete;
        VideoPhotographElement &operator=(const VideoPhotographElement &) = delete;

    public:
        std::shared_ptr<VideoEncode> video_jpeg_encode_;
        std::shared_ptr<VideoEncodeBroadcaster> video_encode_broadcaster_;
    };

    // JPEG图片上传服务器
    class JpegPicUploadElement
    {
    public:
        JpegPicUploadElement() = default;
        static std::unique_ptr<JpegPicUploadElement> Create();
        ~JpegPicUploadElement() = default;

        JpegPicUploadElement(const JpegPicUploadElement &) = delete;
        JpegPicUploadElement &operator=(const JpegPicUploadElement &) = delete;

    public:
        std::shared_ptr<JpegUpload> jpeg_upload_;
    };

    // 裸视频保存元件
    class RawVideoSaveElement
    {
    public:
        RawVideoSaveElement() = default;
        static std::unique_ptr<RawVideoSaveElement> Create();
        ~RawVideoSaveElement() = default;

        RawVideoSaveElement(const RawVideoSaveElement &) = delete;
        RawVideoSaveElement &operator=(const RawVideoSaveElement &) = delete;

    public:
        std::shared_ptr<RawVideoSave> raw_video_save_;
    };

    // 视频H264编码元件
    class VideoH264EncodeElement
    {
    public:
        VideoH264EncodeElement() = default;
        static std::unique_ptr<VideoH264EncodeElement> Create();
        ~VideoH264EncodeElement() = default;

        VideoH264EncodeElement(const VideoH264EncodeElement &) = delete;
        VideoH264EncodeElement &operator=(const VideoH264EncodeElement &) = delete;

    public:
        std::shared_ptr<VideoEncode> video_h264_encode_;
        std::shared_ptr<VideoEncodeBroadcaster> video_encode_broadcaster_;
    };

    // 视频推流元件
    class VideoPushElement
    {
    public:
        VideoPushElement() = default;
        static std::unique_ptr<VideoPushElement> Create();
        ~VideoPushElement() = default;

        VideoPushElement(const VideoPushElement &) = delete;
        VideoPushElement &operator=(const VideoPushElement &) = delete;

    public:
        std::shared_ptr<VideoPush> video_push_;
    };

     // 视频推流rtsp元件
    class VideoRtspPushElement
    {
    public:
        VideoRtspPushElement() = default;
        static std::unique_ptr<VideoRtspPushElement> Create();
        ~VideoRtspPushElement() = default;

        VideoRtspPushElement(const VideoRtspPushElement &) = delete;
        VideoRtspPushElement &operator=(const VideoRtspPushElement &) = delete;

    public:
#if defined(USE_LIVE555_RTSP)
        std::shared_ptr<VideoLive555Push> video_push_;
#else
        std::shared_ptr<VideoRtspPush> video_push_;
#endif
    };

#if defined(ENABLE_LIBPEER)
    // 视频推流libpeer元件
    class VideoLibpeerPushElement
    {
    public:
        VideoLibpeerPushElement() = default;
        static std::unique_ptr<VideoLibpeerPushElement> Create();
        ~VideoLibpeerPushElement() = default;

        VideoLibpeerPushElement(const VideoLibpeerPushElement &) = delete;
        VideoLibpeerPushElement &operator=(const VideoLibpeerPushElement &) = delete;

    public:
        std::shared_ptr<VideoLibpeerPush> video_push_;
    };
#endif

#if defined(ENABLE_WEBRTC)
    // 视频推流官方裁剪栈 webrtc 元件
    class VideoWebrtcPushElement
    {
    public:
        VideoWebrtcPushElement() = default;
        static std::unique_ptr<VideoWebrtcPushElement> Create();
        ~VideoWebrtcPushElement() = default;

        VideoWebrtcPushElement(const VideoWebrtcPushElement &) = delete;
        VideoWebrtcPushElement &operator=(const VideoWebrtcPushElement &) = delete;

    public:
        std::shared_ptr<VideoWebrtcPush> video_push_;
    };
#endif

    // 视频AI处理元件
    class VideoAiProcessElement
    {
    public:
        VideoAiProcessElement() = default;
        static std::unique_ptr<VideoAiProcessElement> Create();
        ~VideoAiProcessElement() = default;

        VideoAiProcessElement(const VideoAiProcessElement &) = delete;
        VideoAiProcessElement &operator=(const VideoAiProcessElement &) = delete;

    public:
        std::shared_ptr<VideoAI> video_ai_;
    };

    // 视频MP4录像元件
    class VideoMp4RecordElement
    {
    };

    // JPEG图片发布元件
    class JpegPicPublishElement
    {
    public:
        JpegPicPublishElement() = default;
        static std::unique_ptr<JpegPicPublishElement> Create();
        ~JpegPicPublishElement() = default;

        JpegPicPublishElement(const JpegPicPublishElement &) = delete;
        JpegPicPublishElement &operator=(const JpegPicPublishElement &) = delete;

    public:
        std::shared_ptr<JpegPublish> jpeg_publish_;
    };

    enum class GrapStatus : uint8_t
    {
        IDEL,
        START,
        FINISH,
    };

    // 媒体调度引擎
    class MediaEngine
    {
    public:
        static MediaEngine *MediaEngineInstance();

        int32_t ReadConfig(const char *cfgFile, ConfigModule::MediaEngineConfig *config);
        int32_t Init(const char *cfgFile);
        int32_t UnInit();
        int32_t StartPush();
        int32_t StopPush();
        int32_t StartRtspPush();
        int32_t StopRtspPush();
#if defined(ENABLE_LIBPEER)
        int32_t StartLibpeerPush();
        int32_t StopLibpeerPush();
#endif
#if defined(ENABLE_WEBRTC)
        int32_t StartWebrtcPush();
        int32_t StopWebrtcPush();
#endif
        int32_t StartAI();
        int32_t StopAI();
        int32_t StartRecord();
        int32_t StopRecord();
        int32_t StartTakePhotograph(video_common::VideoJpegGrap grap_config);
        int32_t StopTakePhotograph();
        int32_t StartSaveRawVideo();
        int32_t StopSaveRawVideo();
        int32_t StartJpegPublish();
        int32_t StopJpegPublish();
        int32_t StartQrIdentify();
        int32_t StopQrIdentify();
        std::string GetBusEventMsg();
        bool EventProcess();

        ~MediaEngine() {};

    private:
        MediaEngine() {};
        MediaEngine(const MediaEngine &) = delete;
        MediaEngine &operator=(const MediaEngine &) = delete;
        MediaEngine(const MediaEngine &&) = delete;
        MediaEngine &operator=(const MediaEngine &&) = delete;
        VideoCaptureElement *FindCaptureElem(std::string src_name);

    public:
        std::thread event_thread_;
        std::mutex api_lock_;
        std::atomic<bool> quit_{false};
        GrapStatus grap_status_{GrapStatus::IDEL};
        ConfigModule::MediaEngineConfig media_engine_cfg_;
        std::vector<std::unique_ptr<VideoCaptureElement>> video_capture_sources_;

        // 抓拍相关
        std::unique_ptr<VideoPhotographElement> video_photograph_element_;
        std::unique_ptr<JpegPicUploadElement> jpeg_pic_upload_element_;

        // JPEG 发布相关（独立）
        std::unique_ptr<VideoPhotographElement> video_jpeg_publish_element_;
        std::unique_ptr<JpegPicPublishElement> jpeg_pic_publish_element_;

        // 推流相关
        std::unique_ptr<VideoH264EncodeElement> video_encode_element_;
        std::unique_ptr<VideoPushElement> video_push_element_;
        std::unique_ptr<VideoRtspPushElement> video_rtsp_push_element_;
#if defined(ENABLE_LIBPEER)
    std::unique_ptr<VideoLibpeerPushElement> video_libpeer_push_element_;
#endif
#if defined(ENABLE_WEBRTC)
    std::unique_ptr<VideoWebrtcPushElement> video_webrtc_push_element_;
#endif

        // AI 相关
        std::unique_ptr<VideoAiProcessElement> video_ai_process_element_;

        // 裸视频保存相关
        std::unique_ptr<RawVideoSaveElement> video_raw_video_save_element_;
    };

} // namespace ecovacs

#endif // API_MEDIA_ENGINE_H_
