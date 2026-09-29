#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>
#include "device_info_x3.h"
#include "video_capture.h"
#include "video_capture_defines.h"
#include "video_capture_impl.h"

namespace ecovacs
{
    namespace videocapturemodule
    {
        VideoCaptureModule::DeviceInfo *VideoCaptureImpl::CreateDeviceInfo()
        {
            return new videocapturemodule::DeviceInfoX3();
        }

    } // namespace videocapturemodule
    
} // namespace ecovacs
