#ifndef VIDEO_CAPTURE_FACTORY_H_
#define VIDEO_CAPTURE_FACTORY_H_

#include "video_capture.h"
#include "video_capture_defines.h"

namespace ecovacs
{
    class VideoCaptureFactory
    {
    public:
        // Create a video capture module object
        // id - unique identifier of this video capture module object.
        // deviceUniqueIdUTF8 - name of the device.
        //                      Available names can be found by using GetDeviceName
        static VideoCaptureModule *Create(const char *deviceUniqueIdUTF8);

        static VideoCaptureModule *Create(int32_t deviceIndex);

#ifdef AM311X_PLATFORM
        static VideoCaptureModule *Create(int32_t deviceIndex, int32_t csi_id,
                                          const char *stream_type, const char *image_type);
#endif

        static VideoCaptureModule::DeviceInfo *CreateDeviceInfo();

    private:
        ~VideoCaptureFactory();
    };

} // namespace ecovacs

#endif // VIDEO_CAPTURE_FACTORY_H_
