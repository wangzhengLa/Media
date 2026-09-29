#include "device_info_v4l2.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <vector>
#include "video_capture.h"
#include "video_capture_defines.h"
#include "video_capture_impl.h"
#include "log.h"

namespace ecovacs
{
    namespace videocapturemodule
    {
        DeviceInfoV4l2::DeviceInfoV4l2() : DeviceInfoImpl() {}

        int32_t DeviceInfoV4l2::Init()
        {
            return 0;
        }

        DeviceInfoV4l2::~DeviceInfoV4l2() {}

        uint32_t DeviceInfoV4l2::NumberOfDevices()
        {
            uint32_t count = 0;
            char device[20];
            int fd = -1;
            struct v4l2_capability cap;

            /* detect /dev/video [0-63]VideoCaptureModule entries */
            for (int n = 0; n < 64; n++)
            {
                snprintf(device, sizeof(device), "/dev/video%d", n);
                if ((fd = open(device, O_RDONLY)) != -1)
                {
                    // query device capabilities and make sure this is a video capture device
                    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0 || !(cap.device_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE))
                    {
                        close(fd);
                        continue;
                    }

                    close(fd);
                    count++;
                }
            }

            return count;
        }

        int32_t DeviceInfoV4l2::GetDeviceName(uint32_t deviceNumber,
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
            struct v4l2_capability cap;
            for (int n = 0; n < 64; n++)
            {
                snprintf(device, sizeof(device), "/dev/video%d", n);
                if ((fd = open(device, O_RDONLY)) != -1)
                {
                    // query device capabilities and make sure this is a video capture device
                    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0 || !(cap.device_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE))
                    {
                        close(fd);
                        continue;
                    }
                    if (count == deviceNumber)
                    {
                        // Found the device
                        found = true;
                        break;
                    }
                    else
                    {
                        close(fd);
                        count++;
                    }
                }
            }

            if (!found)
                return -1;

            // query device capabilities
            if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0)
            {
                ump_info("error in querying the device capability for device[%s] errno[%d]\r\n",
                         device, errno);
                close(fd);
                return -1;
            }

            close(fd);

            char cameraName[64];
            memset(deviceNameUTF8, 0, deviceNameLength);
            memcpy(cameraName, cap.card, sizeof(cap.card));
            if (deviceNameLength > strlen(cameraName))
            {
                memcpy(deviceNameUTF8, cameraName, strlen(cameraName));
            }
            else
            {
                ump_info("buffer passed is too small\r\n");
                return -1;
            }

            if (cap.bus_info[0] != 0)
            {
                // may not available in all drivers
                // copy device id
                size_t len = strlen(reinterpret_cast<const char *>(cap.bus_info));
                if (deviceUniqueIdUTF8Length > len)
                {
                    memset(deviceUniqueIdUTF8, 0, deviceUniqueIdUTF8Length);
                    memcpy(deviceUniqueIdUTF8, cap.bus_info, len);
                }
                else
                {
                    ump_info("buffer passed is too small\r\n");
                    return -1;
                }
            }

            return 0;
        }

        int32_t DeviceInfoV4l2::CreateCapabilityMap(const char *deviceUniqueIdUTF8)
        {
            int fd;
            char device[32];
            bool found = false;

            const int32_t deviceUniqueIdUTF8Length = strlen(deviceUniqueIdUTF8);
            if (deviceUniqueIdUTF8Length >= kVideoCaptureUniqueNameLength)
            {
                ump_error("Device name too long\r\n");
                return -1;
            }

            ump_info("CreateCapabilityMap called for device[%s]\r\n", deviceUniqueIdUTF8);

            /* detect /dev/video [0-63] entries */
            for (int n = 0; n < 64; ++n)
            {
                snprintf(device, sizeof(device), "/dev/video%d", n);
                fd = open(device, O_RDONLY);
                if (fd == -1)
                    continue;

                // query device capabilities
                struct v4l2_capability cap;
                if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0)
                {
                    // skip devices without video capture capability
                    if (!(cap.device_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE))
                    {
                        continue;
                    }

                    if (cap.bus_info[0] != 0)
                    {
                        if (strncmp(reinterpret_cast<const char *>(cap.bus_info), deviceUniqueIdUTF8, strlen(deviceUniqueIdUTF8)) == 0)
                        {
                            // match with device id
                            found = true;
                            break; // fd matches with device unique id supplied
                        }
                    }
                    else
                    {
                        // match for device name
                        if (IsDeviceNameMatches(reinterpret_cast<const char *>(cap.card), deviceUniqueIdUTF8))
                        {
                            found = true;
                            break;
                        }
                    }
                }
                close(fd); // close since this is not the matching device
            }

            if (!found)
            {
                ump_info("no matching device found\r\n");
                return -1;
            }

            // now fd will point to the matching device
            // reset old capability list.
            _captureCapabilities.clear();

            int size = FillCapabilities(fd);
            close(fd);

            // Store the new used device name
            _lastUsedDeviceNameLength = deviceUniqueIdUTF8Length;
            _lastUsedDeviceName = reinterpret_cast<char *>(realloc(_lastUsedDeviceName, _lastUsedDeviceNameLength + 1));
            memcpy(_lastUsedDeviceName, deviceUniqueIdUTF8, _lastUsedDeviceNameLength + 1);

            ump_info("CreateCapabilityMap[%d]\r\n", _captureCapabilities.size());

            return size;
        }

        bool DeviceInfoV4l2::IsDeviceNameMatches(const char *name,
                                                 const char *deviceUniqueIdUTF8)
        {
            if (strncmp(deviceUniqueIdUTF8, name, strlen(name)) == 0)
                return true;
            return false;
        }

        int32_t DeviceInfoV4l2::FillCapabilities(int fd)
        {
            // set image format
            struct v4l2_format video_fmt;
            memset(&video_fmt, 0, sizeof(struct v4l2_format));

            video_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            // video_fmt.fmt.pix.sizeimage = 0;

            int totalFmts = 5;
            unsigned int videoFormats[] = {V4L2_PIX_FMT_MJPEG, V4L2_PIX_FMT_YUV420,
                                           V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_UYVY,
                                           V4L2_PIX_FMT_NV12};

            int sizes = 13;
            unsigned int size[][2] = {{128, 96}, {160, 120}, {176, 144}, {320, 240}, {352, 288}, {640, 480}, {704, 576}, {800, 600}, {960, 720}, {1280, 720}, {1024, 768}, {1440, 1080}, {1920, 1080}};

            for (int fmts = 0; fmts < totalFmts; fmts++)
            {
                for (int i = 0; i < sizes; i++)
                {
                    video_fmt.fmt.pix_mp.pixelformat = videoFormats[fmts];
                    video_fmt.fmt.pix_mp.width = size[i][0];
                    video_fmt.fmt.pix_mp.height = size[i][1];

                    if (ioctl(fd, VIDIOC_TRY_FMT, &video_fmt) >= 0)
                    {
                        if ((video_fmt.fmt.pix_mp.width == size[i][0]) && (video_fmt.fmt.pix_mp.height == size[i][1]))
                        {
                            VideoCaptureCapability cap;
                            cap.width = video_fmt.fmt.pix_mp.width;
                            cap.height = video_fmt.fmt.pix_mp.height;
                            if (videoFormats[fmts] == V4L2_PIX_FMT_YUYV)
                            {
                                cap.videoType = video_common::VideoType::kYUY2;
                            }
                            else if (videoFormats[fmts] == V4L2_PIX_FMT_YUV420)
                            {
                                cap.videoType = video_common::VideoType::kI420;
                            }
                            else if (videoFormats[fmts] == V4L2_PIX_FMT_MJPEG)
                            {
                                cap.videoType = video_common::VideoType::kMJPEG;
                            }
                            else if (videoFormats[fmts] == V4L2_PIX_FMT_UYVY)
                            {
                                cap.videoType = video_common::VideoType::kUYVY;
                            }
                            else if (videoFormats[fmts] == V4L2_PIX_FMT_NV12)
                            {
                                cap.videoType = video_common::VideoType::kNV12;
                            }

                            // get fps of current camera mode
                            // V4l2 does not have a stable method of knowing so we just guess.
                            if (cap.width >= 800 && cap.videoType != video_common::VideoType::kMJPEG)
                            {
                                cap.maxFPS = 15;
                            }
                            else
                            {
                                cap.maxFPS = 30;
                            }

                            _captureCapabilities.push_back(cap);
                            ump_debug("Camera capability, width:%d height:%d type:%d fps:%d",
                                      cap.width,
                                      cap.height,
                                      static_cast<int32_t>(cap.videoType),
                                      cap.maxFPS);
                        }
                    }
                }
            }

            ump_info("CreateCapabilityMap[%d]\r\n", _captureCapabilities.size());

            return _captureCapabilities.size();
        }

    } // namespace videocapturemodule

} // namespace ecovacs
