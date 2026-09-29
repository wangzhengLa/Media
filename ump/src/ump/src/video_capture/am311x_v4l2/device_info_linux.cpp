/**
 * @file    device_info_linux.cpp
 * @brief   AM311x 平台的 VideoCaptureImpl::CreateDeviceInfo 实现
 *
 * 每个平台都有同名的 CreateDeviceInfo，链接时只有当前平台的被选入。
 */

#include "device_info_am311x.h"
#include "video_capture.h"
#include "video_capture_defines.h"
#include "video_capture_impl.h"

namespace ecovacs
{
    namespace videocapturemodule
    {

        VideoCaptureModule::DeviceInfo *VideoCaptureImpl::CreateDeviceInfo()
        {
            return new videocapturemodule::DeviceInfoAm311x();
        }

    } // namespace videocapturemodule

} // namespace ecovacs
