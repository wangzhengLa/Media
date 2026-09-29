#include "video_capture_v4l2.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/prctl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#include <new>
#include <string>
#include "video_capture.h"
#include "bus.h"
#include "log.h"

namespace ecovacs
{
    namespace videocapturemodule
    {
        VideoCaptureModuleV4L2::VideoCaptureModuleV4L2()
            : VideoCaptureImpl(),
              _deviceId(-1),
              _deviceFd(-1),
              _deviceCaps(0),
              _buffersAllocatedByDevice(-1),
              _currentWidth(-1),
              _currentHeight(-1),
              _currentFrameRate(-1),
              _captureStarted(false),
              _captureVideoType(video_common::VideoType::kI420),
              _pool(NULL),
              _numPlanes(1),   
              quit_(false) {}

        int32_t VideoCaptureModuleV4L2::Init(const char *deviceUniqueIdUTF8)
        {
            if (_deviceUniqueId) 
            {
                delete[] _deviceUniqueId;
                _deviceUniqueId = nullptr;
            }
            int len = strlen((const char *)deviceUniqueIdUTF8);
            _deviceUniqueId = new (std::nothrow) char[len + 1];
            if (_deviceUniqueId)
            {
                memcpy(_deviceUniqueId, deviceUniqueIdUTF8, len + 1);
            }

            int fd;
            char device[32];
            bool found = false;

            /* detect /dev/video [0-63] entries */
            int n;
            for (n = 0; n < 64; n++)
            {
                snprintf(device, sizeof(device), "/dev/video%d", n);
                if ((fd = open(device, O_RDONLY)) != -1)
                {
                    // query device capabilities
                    struct v4l2_capability cap;
                    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0)
                    {
                        if (cap.bus_info[0] != 0)
                        {
                            if (strncmp((const char *)cap.bus_info,
                                        (const char *)deviceUniqueIdUTF8,
                                        strlen((const char *)deviceUniqueIdUTF8)) == 0)
                            {
                                // match with device id
                                close(fd);
                                found = true;
                                break; // fd matches with device unique id supplied
                            }
                        }
                    }
                    close(fd); // close since this is not the matching device
                }
            }
            if (!found)
            {
                ump_info("no matching device found\r\n");
                return -1;
            }
            _deviceId = n; // store the device id
            return 0;
        }

        int32_t VideoCaptureModuleV4L2::Init(int32_t deviceIndex)
        {
            _deviceId = deviceIndex; // store the device id
            return 0;
        }

        VideoCaptureModuleV4L2::~VideoCaptureModuleV4L2()
        {
            StopCapture();
            if (_deviceFd != -1) 
            {
                close(_deviceFd);
                _deviceFd =-1;
            }
            _deviceUniqueId = nullptr;
        }

        int32_t VideoCaptureModuleV4L2::StartCapture(const VideoCaptureCapability &capability)
        {
            if (_captureStarted)
            {
                if (capability.width == _currentWidth &&
                    capability.height == _currentHeight &&
                    _captureVideoType == capability.videoType)
                    return 0;
                else
                    StopCapture();
            }

            std::lock_guard<std::mutex> guard(capture_lock_);

            // first open /dev/video device
            char device[20];
            snprintf(device, sizeof(device), "/dev/video%d", _deviceId);
            if ((_deviceFd = open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC, 0)) < 0)
            {
                ump_info("error in opening device[%s] errono[%d]\r\n", device, errno);
                return -1;
            }

            // Supported video formats in preferred order.
            // If the requested resolution is larger than VGA, we prefer MJPEG. Go for
            // I420 otherwise.
            const int nFormats = 6;
            unsigned int fmts[nFormats];
            fmts[0] = V4L2_PIX_FMT_MJPEG;
            fmts[1] = V4L2_PIX_FMT_NV12;
            fmts[2] = V4L2_PIX_FMT_YUYV;
            fmts[3] = V4L2_PIX_FMT_UYVY;
            fmts[4] = V4L2_PIX_FMT_YUV420;
            fmts[5] = V4L2_PIX_FMT_JPEG;

            struct v4l2_capability cap;
            if (ioctl(_deviceFd, VIDIOC_QUERYCAP, &cap) < 0)
            {
                ump_error("_deviceFd %d VIDIOC_QUERYCAP failed\r\n", _deviceFd);
                close(_deviceFd);
                _deviceFd = -1;
                return -1;
            }

            _deviceCaps = cap.device_caps;

            // Enumerate image formats.
            struct v4l2_fmtdesc fmt;
            int fmtsIdx = nFormats;
            memset(&fmt, 0, sizeof(fmt));
            fmt.index = 0;
            fmt.type = (_deviceCaps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ump_info("Video Capture enumerats supported image formats:\r\n");
            while (ioctl(_deviceFd, VIDIOC_ENUM_FMT, &fmt) == 0)
            {
                ump_info("{ pixelformat = %s, description = %s }\r\n",
                         video_common::GetFourccName(fmt.pixelformat).c_str(), fmt.description);
                // Match the preferred order.
                for (int i = 0; i < nFormats; i++)
                {
                    if (fmt.pixelformat == fmts[i] && i < fmtsIdx)
                        fmtsIdx = i;
                }
                // Keep enumerating.
                fmt.index++;
            }
            if (fmtsIdx == nFormats)
            {
                ump_info("no supporting video formats found\r\n");
                close(_deviceFd);
                _deviceFd = -1;
                return -1;
            }

            ump_info("We prefer format[%s]\r\n", video_common::GetFourccName(fmts[fmtsIdx]).c_str());

#ifdef AW_PLATFORM
            /* select the current video input */
            struct v4l2_input inp;
            memset(&inp, 0, sizeof(inp));
            inp.index = 0;
            inp.type = V4L2_INPUT_TYPE_CAMERA;
            if (ioctl(_deviceFd, VIDIOC_S_INPUT, &inp) < 0)
            {
                ump_error(" VIDIOC_S_INPUT failed! s_input: %d\n", inp.index);
                close(_deviceFd);
                _deviceFd = -1;
                return -1;
            }
#endif
            struct v4l2_format video_fmt;
            memset(&video_fmt, 0, sizeof(struct v4l2_format));
            if ((_deviceCaps & V4L2_CAP_VIDEO_CAPTURE_MPLANE))
            {
                video_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
                video_fmt.fmt.pix_mp.width = capability.width;
                video_fmt.fmt.pix_mp.height = capability.height;
                video_fmt.fmt.pix_mp.pixelformat = fmts[fmtsIdx];
                video_fmt.fmt.pix_mp.field = V4L2_FIELD_INTERLACED;

                if (video_fmt.fmt.pix_mp.pixelformat == V4L2_PIX_FMT_YUYV)
                    _captureVideoType = video_common::VideoType::kYUY2;
                else if (video_fmt.fmt.pix_mp.pixelformat == V4L2_PIX_FMT_YUV420)
                    _captureVideoType = video_common::VideoType::kI420;
                else if (video_fmt.fmt.pix_mp.pixelformat == V4L2_PIX_FMT_UYVY)
                    _captureVideoType = video_common::VideoType::kUYVY;
                else if (video_fmt.fmt.pix_mp.pixelformat == V4L2_PIX_FMT_NV12)
                    _captureVideoType = video_common::VideoType::kNV12;
                else if (video_fmt.fmt.pix_mp.pixelformat == V4L2_PIX_FMT_MJPEG ||
                         video_fmt.fmt.pix_mp.pixelformat == V4L2_PIX_FMT_JPEG)
                    _captureVideoType = video_common::VideoType::kMJPEG;
            }
            else
            {
                video_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                video_fmt.fmt.pix.width = capability.width;
                video_fmt.fmt.pix.height = capability.height;
                video_fmt.fmt.pix.pixelformat = fmts[fmtsIdx];
                video_fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;

                if (video_fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV)
                    _captureVideoType = video_common::VideoType::kYUY2;
                else if (video_fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUV420)
                    _captureVideoType = video_common::VideoType::kI420;
                else if (video_fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_UYVY)
                    _captureVideoType = video_common::VideoType::kUYVY;
                else if (video_fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_NV12)
                    _captureVideoType = video_common::VideoType::kNV12;
                else if (video_fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_MJPEG ||
                         video_fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_JPEG)
                    _captureVideoType = video_common::VideoType::kMJPEG;
            }

            // set format and frame size now
            if (ioctl(_deviceFd, VIDIOC_S_FMT, &video_fmt) < 0)
            {
                ump_info("error in VIDIOC_S_FMT, errno = %d", errno);
                close(_deviceFd);
                _deviceFd = -1;
                return -1;
            }

            // initialize current width and height
            _numPlanes = 1;
            if (_deviceCaps & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
            {
                if (ioctl(_deviceFd, VIDIOC_G_FMT, &video_fmt) < 0)
                {
                    ump_error("get the data format failed!\n");
                    close(_deviceFd);
                    _deviceFd = -1;
                    return -1;
                }
                if (video_fmt.fmt.pix_mp.num_planes != _numPlanes)
                {
                    ump_info("video_fmt.fmt.pix_mp.num_planes %d != _numPlanes %d\r\n",
                             video_fmt.fmt.pix_mp.num_planes, _numPlanes);
                    close(_deviceFd);
                    _deviceFd = -1;
                    return -1;
                }
                _currentWidth = video_fmt.fmt.pix_mp.width;
                _currentHeight = video_fmt.fmt.pix_mp.height;
            }
            else
            {
                _currentWidth = video_fmt.fmt.pix.width;
                _currentHeight = video_fmt.fmt.pix.height;
            }

            // Trying to set frame rate, before check driver capability.
            bool driver_framerate_support = true;
            struct v4l2_streamparm streamparms;
            memset(&streamparms, 0, sizeof(streamparms));
            streamparms.type = _deviceCaps & V4L2_CAP_VIDEO_CAPTURE_MPLANE ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (ioctl(_deviceFd, VIDIOC_G_PARM, &streamparms) < 0)
            {
                ump_info("error in VIDIOC_G_PARM errno = %d\r\n", errno);
                driver_framerate_support = false;
            }
            else
            {
                // check the capability flag is set to V4L2_CAP_TIMEPERFRAME.
                if (streamparms.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)
                {
                    // driver supports the feature. Set required framerate.
                    memset(&streamparms, 0, sizeof(streamparms));
                    streamparms.type = _deviceCaps & V4L2_CAP_VIDEO_CAPTURE_MPLANE ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;
                    streamparms.parm.capture.timeperframe.numerator = 1;
                    streamparms.parm.capture.timeperframe.denominator = capability.maxFPS;
                    if (ioctl(_deviceFd, VIDIOC_S_PARM, &streamparms) < 0)
                    {
                        ump_info("Failed to set the framerate. errno = %d", errno);
                        driver_framerate_support = false;
                    }
                    else
                    {
                        ump_info("Successed to set the framerate. capability.maxFPS = %d\r\n", capability.maxFPS);
                        _currentFrameRate = capability.maxFPS;
                    }
                }
            }

            // If driver doesn't support framerate control, need to hardcode.
            // Hardcoding the value based on the frame size.
            if (!driver_framerate_support)
            {
                if (_currentWidth >= 800 && _captureVideoType != video_common::VideoType::kMJPEG)
                    _currentFrameRate = 15;
                else
                    _currentFrameRate = capability.maxFPS;
            }

            ump_info("final choose _currentWidth[%d] _currentHeight[%d] _currentFrameRate[%d] _captureVideoType[%d]\r\n",
                     _currentWidth, _currentHeight, _currentFrameRate, _captureVideoType);

            if (!AllocateVideoBuffers())
            {
                ump_info("failed to allocate video capture buffers\r\n");
                close(_deviceFd);
                _deviceFd = -1;
                return -1;
            }

            // Needed to start UVC camera - from the uvcview application
            enum v4l2_buf_type type;
            type = _deviceCaps & V4L2_CAP_VIDEO_CAPTURE_MPLANE ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (ioctl(_deviceFd, VIDIOC_STREAMON, &type) == -1)
            {
                ump_info("Failed to turn on stream\r\n");
                close(_deviceFd);
                _deviceFd = -1;
                return -1;
            }

            _captureStarted = true;

            // start capture thread;
            {
                quit_ = false;
                _captureThread = std::thread(
                    [this]
                    {
                        prctl(PR_SET_NAME, "capture_process");
                        int policy;
                        struct sched_param param;
                        pthread_getschedparam(pthread_self(), &policy, &param);
                        param.sched_priority = 40;
                        ump_info("param.sched_priority %d\r\n", param.sched_priority);
                        pthread_setschedparam(pthread_self(), SCHED_RR, &param);
                        while (CaptureProcess())
                        {
                        }
                    });
            }

            return 0;
        }

        int32_t VideoCaptureModuleV4L2::StopCapture()
        {
            {
                std::lock_guard<std::mutex> guard(capture_lock_);
                quit_ = true;
            }

            // Make sure the capture thread stops using the mutex.
            if (_captureThread.joinable()) 
            {
        	_captureThread.join();
    	    }

            std::lock_guard<std::mutex> guard(capture_lock_);
            if (_captureStarted)
            {
                _captureStarted = false;
                DeAllocateVideoBuffers();
                if (_deviceFd != -1) 
                {
                    close(_deviceFd);
                    _deviceFd = -1;
                }
            }

            return 0;
        }

        // critical section protected by the caller
        bool VideoCaptureModuleV4L2::AllocateVideoBuffers() 
        {
            struct v4l2_requestbuffers rbuffer;
            memset(&rbuffer, 0, sizeof(v4l2_requestbuffers));
            rbuffer.type = (_deviceCaps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;
            rbuffer.memory = V4L2_MEMORY_MMAP;
            rbuffer.count = kNoOfV4L2Bufffers;
            if (ioctl(_deviceFd, VIDIOC_REQBUFS, &rbuffer) < 0) 
            {
                ump_error("Could not get buffers from device. errno = %d\r\n", errno);
                return false;
            }

            if (rbuffer.count > kNoOfV4L2Bufffers)
                rbuffer.count = kNoOfV4L2Bufffers;

            _buffersAllocatedByDevice = rbuffer.count;
            _pool = new Buffer[rbuffer.count];

            for (unsigned int i = 0; i < rbuffer.count; i++) 
            {
                ump_info("ioctl VIDIOC_QUERYBUF %d \r\n", i);
                struct v4l2_buffer buffer;
                struct v4l2_plane planes[_numPlanes];
                memset(&buffer, 0, sizeof(v4l2_buffer));
                buffer.type = (_deviceCaps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buffer.memory = V4L2_MEMORY_MMAP;
                buffer.index = i;
                if (V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE == buffer.type) 
                {
                    buffer.length = _numPlanes;
                    buffer.m.planes = planes;
                }

                if (ioctl(_deviceFd, VIDIOC_QUERYBUF, &buffer) < 0) 
                {
                    ump_error("ioctl VIDIOC_QUERYBUF failed\r\n");
                    for (unsigned int j = 0; j < i; ++j) 
                    {
                        if (_pool[j].start && _pool[j].start != MAP_FAILED)
                            munmap(_pool[j].start, _pool[j].length);
                    }
                    delete[] _pool;
                    _pool = nullptr;
                    _buffersAllocatedByDevice = 0;
                    return false;
                }

                if (V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE == buffer.type)
                    _pool[i].start = mmap(NULL, buffer.m.planes[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, _deviceFd, buffer.m.planes[0].m.mem_offset);
                else
                    _pool[i].start = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, _deviceFd, buffer.m.offset);

                if (MAP_FAILED == _pool[i].start) 
                {
                    for (unsigned int j = 0; j < i; ++j) 
                    {
                        if (_pool[j].start && _pool[j].start != MAP_FAILED)
                            munmap(_pool[j].start, _pool[j].length);
                    }
                    delete[] _pool;
                    _pool = nullptr;
                    _buffersAllocatedByDevice = 0;
                    return false;
                }

                _pool[i].length = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE == buffer.type ? buffer.m.planes[0].length : buffer.length;

                if (ioctl(_deviceFd, VIDIOC_QBUF, &buffer) < 0) 
                {
                    ump_error("ioctl VIDIOC_QBUF failed\r\n");
                    if (_pool[i].start && _pool[i].start != MAP_FAILED)
                        munmap(_pool[i].start, _pool[i].length);
                    for (unsigned int j = 0; j < i; ++j) 
                    {
                        if (_pool[j].start && _pool[j].start != MAP_FAILED)
                            munmap(_pool[j].start, _pool[j].length);
                    }
                    delete[] _pool;
                    _pool = nullptr;
                    _buffersAllocatedByDevice = 0;
                    return false;
                }
            }
            return true;
        }

        bool VideoCaptureModuleV4L2::DeAllocateVideoBuffers() 
        {
            if (_pool) 
            {
                for (int i = 0; i < _buffersAllocatedByDevice; ++i) 
                {
                    if (_pool[i].start && _pool[i].start != MAP_FAILED)
                        munmap(_pool[i].start, _pool[i].length);
                }
                delete[] _pool;
                _pool = nullptr;
                _buffersAllocatedByDevice = 0;
            }

            if (_deviceFd != -1) {
                enum v4l2_buf_type type;
                type = (_deviceCaps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;
                if (ioctl(_deviceFd, VIDIOC_STREAMOFF, &type) < 0)
                {
                    ump_info("VIDIOC_STREAMOFF error. errno: %d\r\n", errno);
                    return false;
                }
            }

            return true;
        }

        bool VideoCaptureModuleV4L2::CaptureStarted()
        {
            return _captureStarted;
        }

        bool VideoCaptureModuleV4L2::CaptureProcess()
        {
            int retVal = 0;
            fd_set rSet;
            struct timeval timeout;

            FD_ZERO(&rSet);
            FD_SET(_deviceFd, &rSet);
            timeout.tv_sec = 2;
            timeout.tv_usec = 0;

            // _deviceFd written only in StartCapture, when this thread isn't running.
            retVal = select(_deviceFd + 1, &rSet, NULL, NULL, &timeout);
            {
                std::lock_guard<std::mutex> guard(capture_lock_);
                if (quit_)
                    return false;

                // continue if interrupted select failed
                if (retVal < 0 && errno != EINTR)
                {
                    ump_error("video capture interrupted select failed\r\n");
                    return false;
                }
                // select timed out
                else if (retVal == 0)
                {
                    ump_error("video capture select timed out\r\n");
                    // 上报采集异常事件
                    const char *string = "{\"cmd\":\"capture_select_timeout\"}";
                    bus_put_message((char *)string, strlen(string));
                    return false;
                }
                // not event on camera handle
                else if (!FD_ISSET(_deviceFd, &rSet))
                {
                    return true;
                }

                if (_captureStarted)
                {
                    struct v4l2_buffer buf;
                    struct v4l2_plane planes[_numPlanes];
                    memset(&buf, 0, sizeof(struct v4l2_buffer));
                    buf.type = (_deviceCaps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;
                    buf.memory = V4L2_MEMORY_MMAP;
                    if (V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE == buf.type)
                    {
                        buf.m.planes = planes;
                        buf.length = _numPlanes;
                    }
                    if (ioctl(_deviceFd, VIDIOC_DQBUF, &buf) < 0)
                    {
                        if (errno != EINTR)
                        {
                            ump_info("could not sync on a buffer on device %s", strerror(errno));
                            return true;
                        }
                    }

                    VideoCaptureCapability frameInfo;
                    frameInfo.width = _currentWidth;
                    frameInfo.height = _currentHeight;
                    frameInfo.videoType = _captureVideoType;
                    // convert to to I420 if needed
                    IncomingFrame(reinterpret_cast<uint8_t *>(_pool[buf.index].start),
                                  V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE == buf.type ? buf.m.planes[0].bytesused : buf.bytesused, frameInfo,
                                  buf.timestamp.tv_sec * 1000 + buf.timestamp.tv_usec / 1000, buf.sequence);

                    // enqueue the buffer again
                    if (ioctl(_deviceFd, VIDIOC_QBUF, &buf) == -1)
                    {
                        ump_error("Failed to enqueue capture buffer\r\n");
                    }
                }
            }

            return true;
        }

        int32_t VideoCaptureModuleV4L2::CaptureSettings(VideoCaptureCapability &settings)
        {
            settings.width = _currentWidth;
            settings.height = _currentHeight;
            settings.maxFPS = _currentFrameRate;
            settings.videoType = _captureVideoType;
            return 0;
        }

    } // namespace videocapturemodule

} // namespace ecovacs
