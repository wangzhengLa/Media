#ifndef API_VIDEO_CAPTURE_H_
#define API_VIDEO_CAPTURE_H_

#include <memory>
#include <mutex>
#include <string>
#include "video_common_defines.h"
#include "video_capture.h"

namespace ecovacs
{
    class VideoCapture
    {
    public:
        VideoCapture();  // 构造函数改为 public
        ~VideoCapture();

        static std::unique_ptr<VideoCapture> Create(size_t width, size_t height, size_t target_fps,
                                                    size_t capture_device_index, int32_t csi_id = -1,
                                                    const std::string &stream_type = "fr",
                                                    const std::string &image_type = "YUYV");

        int32_t StartCapture();
        int32_t StopCapture();
        bool CaptureStarted();

        void RegisterCaptureDataCallback(video_common::VideoSinkInterface *dataCallback);
        void DeRegisterCaptureDataCallback();
        void Destroy();

    private:
        bool Init(size_t width, size_t height, size_t target_fps, size_t capture_device_index,
                  int32_t csi_id = -1, const std::string &stream_type = "fr",
                  const std::string &image_type = "YUYV");

        std::unique_ptr<VideoCaptureModule> vcm_;
        VideoCaptureCapability capability_;
        std::string stream_type_;
        std::string image_type_;
        mutable std::mutex mutex_;
    };

} // namespace ecovacs

#endif // API_VIDEO_CAPTURE_H_