#ifndef DEVICE_INFO_X3_H_
#define DEVICE_INFO_X3_H_

#include <stdint.h>
#include "device_info_impl.h"

namespace ecovacs
{
    namespace videocapturemodule
    {
        class DeviceInfoX3 : public DeviceInfoImpl
        {
        public:
            DeviceInfoX3();
            ~DeviceInfoX3() override;
            uint32_t NumberOfDevices() override;
            int32_t GetDeviceName(uint32_t deviceNumber,
                                  char *deviceNameUTF8,
                                  uint32_t deviceNameLength,
                                  char *deviceUniqueIdUTF8,
                                  uint32_t deviceUniqueIdUTF8Length,
                                  char *productUniqueIdUTF8 = 0,
                                  uint32_t productUniqueIdUTF8Length = 0) override;
            /*
             * Fills the membervariable _captureCapabilities with capabilites for the
             * given device name.
             */
            int32_t CreateCapabilityMap(const char *deviceUniqueIdUTF8) override;
            int32_t FillCapabilities(int fd);
            int32_t Init() override;

        private:
            bool IsDeviceNameMatches(const char *name, const char *deviceUniqueIdUTF8);
        };

    } // namespace videocapturemodule

} // namespace ecovacs

#endif // DEVICE_INFO_X3_H_
