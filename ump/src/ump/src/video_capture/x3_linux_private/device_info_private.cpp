#include "device_info_x3.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>
#include "video_capture.h"
#include "video_capture_defines.h"
#include "video_capture_impl.h"
#include <com/amazonaws/kinesis/video/webrtcclient/log.h>

namespace ecovacs
{
    namespace videocapturemodule
    {
        DeviceInfoX3::DeviceInfoX3() : DeviceInfoImpl() {}

        int32_t DeviceInfoX3::Init()
        {
            return 0;
        }

        DeviceInfoX3::~DeviceInfoX3() {}

        uint32_t DeviceInfoX3::NumberOfDevices()
        {
            uint32_t count = 1;
            return count;
        }

        int32_t DeviceInfoX3::GetDeviceName(uint32_t deviceNumber,
                                              char *deviceNameUTF8,
                                              uint32_t deviceNameLength,
                                              char *deviceUniqueIdUTF8,
                                              uint32_t deviceUniqueIdUTF8Length,
                                              char * /*productUniqueIdUTF8*/,
                                              uint32_t /*productUniqueIdUTF8Length*/)
        {
            // Travel through /dev/video [0-63]
            uint32_t count = 0;
            char device[20];
            int fd = -1;
            bool found = false;

            char cameraName[64];
            memset(deviceNameUTF8, 0, deviceNameLength);
            memcpy(cameraName, "TEST_X3_VIDEO", sizeof("TEST_X3_VIDEO"));
            if (deviceNameLength > strlen(cameraName))
            {
                memcpy(deviceNameUTF8, cameraName, strlen(cameraName));
            }
            else
            {
                info("buffer passed is too small\r\n");
                return -1;
            }

            memset(deviceUniqueIdUTF8, 0, deviceUniqueIdUTF8Length);
            memcpy(deviceUniqueIdUTF8,"TEST_X3_VIDEO", sizeof("TEST_X3_VIDEO"));

            return 0;
        }

        int32_t DeviceInfoX3::CreateCapabilityMap(const char *deviceUniqueIdUTF8)
        {
            char device[32];
            bool found = false;

            const int32_t deviceUniqueIdUTF8Length = strlen(deviceUniqueIdUTF8);
            if (deviceUniqueIdUTF8Length >= kVideoCaptureUniqueNameLength)
            {
                error("Device name too long\r\n");
                return -1;
            }

            info("CreateCapabilityMap called for device[%s]\r\n", deviceUniqueIdUTF8);

            _captureCapabilities.clear();


            // Store the new used device name
            _lastUsedDeviceNameLength = deviceUniqueIdUTF8Length;
            _lastUsedDeviceName = reinterpret_cast<char *>(realloc(_lastUsedDeviceName, _lastUsedDeviceNameLength + 1));
            memcpy(_lastUsedDeviceName, deviceUniqueIdUTF8, _lastUsedDeviceNameLength + 1);

            info("CreateCapabilityMap[%d]\r\n", _captureCapabilities.size());

            return 0;
        }

        bool DeviceInfoX3::IsDeviceNameMatches(const char *name,
                                                 const char *deviceUniqueIdUTF8)
        {
            if (strncmp(deviceUniqueIdUTF8, name, strlen(name)) == 0)
                return true;
            return false;
        }

        int32_t DeviceInfoX3::FillCapabilities(int fd)
        {
            info("CreateCapabilityMap[%d]\r\n", _captureCapabilities.size());

            return _captureCapabilities.size();
        }

    } // namespace videocapturemodule

} // namespace ecovacs
