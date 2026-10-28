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

#ifdef AM311X_PLATFORM
    VideoCaptureModule *VideoCaptureFactory::Create(int32_t deviceIndex, int32_t csi_id,
                                                    const char *stream_type, const char *image_type)
    {
        return videocapturemodule::VideoCaptureImpl::Create(deviceIndex, csi_id, stream_type, image_type);
    }
#endif

    VideoCaptureModule::DeviceInfo *VideoCaptureFactory::CreateDeviceInfo()
    {
        return videocapturemodule::VideoCaptureImpl::CreateDeviceInfo();
    }

} // namespace ecovacs
