#include "video_capture_factory.h"
#include "video_capture_impl.h"

namespace ecovacs
{
    VideoCaptureModule *VideoCaptureFactory::Create(const char *deviceUniqueIdUTF8)
    {
        return videocapturemodule::VideoCaptureImpl::Create(deviceUniqueIdUTF8);
    }

    VideoCaptureModule *VideoCaptureFactory::Create(int32_t deviceIndex)
    {
        return videocapturemodule::VideoCaptureImpl::Create(deviceIndex);
    }

    VideoCaptureModule::DeviceInfo *VideoCaptureFactory::CreateDeviceInfo()
    {
        return videocapturemodule::VideoCaptureImpl::CreateDeviceInfo();
    }

} // namespace ecovacs
