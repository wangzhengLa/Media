#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include <new>
#include <string>

#include "video_capture_x3.h"
#include "video_capture.h"

namespace ecovacs
{
    namespace videocapturemodule
    {
        VideoCaptureModule* VideoCaptureImpl::Create(const char *deviceUniqueId)
        {
            auto implementation = new VideoCaptureModuleX3();
            if (implementation->Init(deviceUniqueId) != 0)
                return nullptr;

            return implementation;
        }

    } // namespace videocapturemodule

} // namespace ecovacs
