#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include <new>
#include <string>

#include "video_capture_v4l2.h"
#include "video_capture.h"

namespace ecovacs
{
    namespace videocapturemodule
    {
        VideoCaptureModule *VideoCaptureImpl::Create(const char *deviceUniqueId)
        {
            auto implementation = new VideoCaptureModuleV4L2();
            if (implementation->Init(deviceUniqueId) != 0)
                return nullptr;

            return implementation;
        }

        VideoCaptureModule *VideoCaptureImpl::Create(int32_t deviceIndex)
        {
            auto implementation = new VideoCaptureModuleV4L2();
            if (implementation->Init(deviceIndex) != 0)
                return nullptr;

            return implementation;
        }

    } // namespace videocapturemodule

} // namespace ecovacs
