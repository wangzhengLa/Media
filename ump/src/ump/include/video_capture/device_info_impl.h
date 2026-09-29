#ifndef DEVICE_INFO_IMPL_H_
#define DEVICE_INFO_IMPL_H_

#include <stdint.h>
#include <vector>
#include <mutex>
#include "video_common_defines.h"
#include "video_capture.h"
#include "video_capture_defines.h"

namespace ecovacs
{
    namespace videocapturemodule
    {
        class DeviceInfoImpl : public VideoCaptureModule::DeviceInfo
        {
        public:
            DeviceInfoImpl();
            ~DeviceInfoImpl(void) override;
            int32_t NumberOfCapabilities(const char *deviceUniqueIdUTF8) override;
            int32_t GetCapability(const char *deviceUniqueIdUTF8,
                                  uint32_t deviceCapabilityNumber,
                                  VideoCaptureCapability &capability) override;

            int32_t GetBestMatchedCapability(const char *deviceUniqueIdUTF8,
                                             const VideoCaptureCapability &requested,
                                             VideoCaptureCapability &resulting) override;
            int32_t GetOrientation(const char *deviceUniqueIdUTF8,
                                   video_common::VideoRotation &orientation) override;

        protected:
            /* Initialize this object*/
            virtual int32_t Init() = 0;
            /*
             * Fills the member variable _captureCapabilities with capabilities for the
             * given device name.
             */
            virtual int32_t CreateCapabilityMap(const char *deviceUniqueIdUTF8) = 0;

        protected:
            // Data members
            typedef std::vector<VideoCaptureCapability> VideoCaptureCapabilities;
            VideoCaptureCapabilities _captureCapabilities;
            std::mutex _apiLock;
            char *_lastUsedDeviceName;
            uint32_t _lastUsedDeviceNameLength;
        };

    } // namespace videocapturemodule

} // namespace ecovacs

#endif // DEVICE_INFO_IMPL_H_
