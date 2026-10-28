#include <memory>
#include "log.h"
#include "video_capture_factory.h"
#include "api_video_capture.h"

namespace ecovacs
{
    VideoCapture::VideoCapture() = default;

    VideoCapture::~VideoCapture() = default;

    std::unique_ptr<VideoCapture> VideoCapture::Create(size_t width, size_t height, size_t target_fps,
                                                       size_t device_index, int32_t csi_id,
                                                       const std::string &stream_type,
                                                       const std::string &image_type)
    {
        auto vcm_capture = std::make_unique<VideoCapture>();
        vcm_capture->stream_type_ = stream_type;
        vcm_capture->image_type_ = image_type;
        ump_info("VideoCapture::Create: %dx%d@%d csi_id=%d stream_type=%s image_type=%s\n",
                 width, height, target_fps, csi_id, stream_type.c_str(), image_type.c_str());
        if (!vcm_capture->Init(width, height, target_fps, device_index, csi_id, stream_type, image_type))
        {
            ump_error("Failed to create VideoCapture %dx%d@%d", width, height, target_fps);
            return nullptr;
        }
        return vcm_capture;
    }

    int32_t VideoCapture::StartCapture()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!vcm_)
        {
            ump_error("vcm_ not created");
            return -1;
        }
        if (vcm_->CaptureStarted())
        {
            return 0;
        }
        if (0 != vcm_->StartCapture(capability_))
        {
            ump_error("StartCapture failed");
            return -1;
        }
        return 0;
    }

    int32_t VideoCapture::StopCapture()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!vcm_)
        {
            return 0;
        }
        if (!vcm_->CaptureStarted())
        {
            return 0;
        }
        if (0 != vcm_->StopCapture())
        {
            ump_error("StopCapture failed");
            return -1;
        }
        return 0;
    }

    bool VideoCapture::CaptureStarted()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!vcm_)
        {
            return false;
        }
        return vcm_->CaptureStarted();
    }

    void VideoCapture::RegisterCaptureDataCallback(video_common::VideoSinkInterface *dataCallback)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!vcm_ || !dataCallback)
        {
            return;
        }
        vcm_->RegisterCaptureDataCallback(dataCallback);
    }

    void VideoCapture::DeRegisterCaptureDataCallback()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!vcm_)
        {
            return;
        }
        vcm_->DeRegisterCaptureDataCallback();
    }

    bool VideoCapture::Init(size_t width, size_t height, size_t target_fps, size_t device_index,
                            int32_t csi_id, const std::string &stream_type, const std::string &image_type)
    {
    #if defined(RK_PLATFORM) || defined(AM311X_PLATFORM)
        auto device_info = std::unique_ptr<VideoCaptureModule::DeviceInfo>(VideoCaptureFactory::CreateDeviceInfo());

        uint32_t number = device_info->NumberOfDevices();
        if (0 == number)
        {
            ump_error("no video device");
            return false;
        }

        char device_name[256];
        char unique_name[256];
        if (device_info->GetDeviceName(static_cast<uint32_t>(device_index),
                                       device_name, sizeof(device_name),
                                       unique_name, sizeof(unique_name)) != 0)
        {
            ump_error("GetDeviceName failed");
            return false;
        }

        int32_t capNum = device_info->NumberOfCapabilities(unique_name);
        if (0 == capNum)
        {
            ump_error("no capabilities");
            return false;
        }
#endif
        VideoCaptureCapability requestCap;
        requestCap.width = static_cast<int32_t>(width);
        requestCap.height = static_cast<int32_t>(height);
        requestCap.maxFPS = static_cast<int32_t>(target_fps);
#if defined(AM311X_PLATFORM)
        // AM311x ISP FR 主推 YUYV，NV12 的 UV 平面输出不正确（绿屏）
        requestCap.videoType = video_common::VideoType::kYUY2;
#else
        requestCap.videoType = video_common::VideoType::kNV12;
#endif

        VideoCaptureCapability bestCap = requestCap;
#if defined(RK_PLATFORM) || defined(AM311X_PLATFORM)
        int32_t num = device_info->GetBestMatchedCapability(unique_name, requestCap, bestCap);
        if (0 == num)
        {
            ump_error("no matched capability");
            return false;
        }
#endif

        capability_ = bestCap;
        capability_.maxFPS = static_cast<int32_t>(target_fps);

#if defined(AM311X_PLATFORM)
        vcm_ = std::unique_ptr<VideoCaptureModule>(VideoCaptureFactory::Create(device_index, csi_id, stream_type.c_str(), image_type.c_str()));
#else
        (void)csi_id;
        (void)stream_type;
        (void)image_type;
        vcm_ = std::unique_ptr<VideoCaptureModule>(VideoCaptureFactory::Create(device_index));
#endif
        if (!vcm_)
        {
            ump_error("VideoCaptureFactory::Create failed");
            return false;
        }

        return true;
    }

    void VideoCapture::Destroy()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!vcm_)
        {
            return;
        }

        try
        {
            if (vcm_->CaptureStarted())
            {
                vcm_->StopCapture();
            }
            vcm_->DeRegisterCaptureDataCallback();
        }
        catch (...)
        {
            // 忽略异常
        }
        vcm_.reset();
    }

} // namespace ecovacs