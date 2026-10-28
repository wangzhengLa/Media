/**
 * @file    video_capture_am311x.cpp
 * @brief   AM311x 视频采集实现
 */

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/media-bus-format.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>
#include <new>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#include "bus.h"
#include "device_info_am311x.h"   // am311x_discovery
#include "log.h"
#include "video_capture.h"
#include "video_capture_am311x.h"

namespace ecovacs
{
    namespace videocapturemodule
    {

        int VideoCaptureModuleAm311x::IoctlEintrSafe(int fd, unsigned long request, void *arg)
        {
            int ret;
            do {
                ret = ::ioctl(fd, request, arg);
            } while (ret < 0 && errno == EINTR);
            return ret;
        }

        // ==========================================================================
        // 构造 / 析构
        // ==========================================================================
        VideoCaptureModuleAm311x::VideoCaptureModuleAm311x()
            : VideoCaptureImpl()
            , fr_fd_(-1)
            , ds_fd_(-1)
            , subdev_fd_(-1)
            , device_index_(-1)
            , csi_id_(-1)
            , stream_type_("fr")
            , image_type_("YUYV")
            , width_(0)
            , height_(0)
            , fourcc_(0)
            , bytesperline_(0)
            , sizeimage_(0)
            , buffer_count_(kDefaultBufferCount)
            , timeout_ms_(2000)
            , captured_(0)
            , last_sequence_(0)
            , have_sequence_(false)
            , quit_(false)
            , capture_started_(false)
            , restore_sensor_(false)
            , requested_fourcc_(V4L2_PIX_FMT_YUYV)  // 默认值
        {
            fr_device_[0] = '\0';
            ds_device_[0] = '\0';
            subdev_device_[0] = '\0';
            buffers_ = nullptr;
            mapped_count_ = 0;

            // original sensor format 保存槽位
            memset(&original_sensor_format_, 0, sizeof(original_sensor_format_));
        }

        VideoCaptureModuleAm311x::~VideoCaptureModuleAm311x()
        {
            StopCapture();  // 内部会做 STREAMOFF + DeallocateStream + close

            // 如果 ConfigureSensor 修改过 sensor 格式，恢复它
            if (restore_sensor_) {
                RestoreSensor();
            }

            // 最终兜底
            if (fr_fd_ >= 0) { ::close(fr_fd_); fr_fd_ = -1; }
            if (ds_fd_ >= 0) { ::close(ds_fd_); ds_fd_ = -1; }
            if (subdev_fd_ >= 0) { ::close(subdev_fd_); subdev_fd_ = -1; }
        }

        // ==========================================================================
        // Init — 两种入口
        // ==========================================================================
        int32_t VideoCaptureModuleAm311x::Init(const char *deviceUniqueId)
        {
            if (deviceUniqueId && strlen(deviceUniqueId) > 0) {
                // 存 uniqueId 到基类
                size_t len = strlen(deviceUniqueId);
                if (_deviceUniqueId) { delete[] _deviceUniqueId; _deviceUniqueId = nullptr; }
                _deviceUniqueId = new (std::nothrow) char[len + 1];
                if (_deviceUniqueId) memcpy(_deviceUniqueId, deviceUniqueId, len + 1);

                // Init(const char*) 传入的 uniqueId 就是 fr_device（如 "/dev/video3"）
                snprintf(fr_device_, sizeof(fr_device_), "%s", deviceUniqueId);

                // 但 subdev_device_ 还不知道——discover_graph 找
                am311x_discovery::camera_v2_graph g;
                int ret = am311x_discovery::camera_v2_discover_graph(
                    nullptr, -1, 0, &g);
                if (ret == 0) {
                    snprintf(subdev_device_, sizeof(subdev_device_), "%s", g.sensor_device);
                    ump_info("Init(deviceUniqueId=%s): subdev=%s, fr=%s\n",
                             deviceUniqueId, subdev_device_, fr_device_);
                } else {
                    ump_warn("Init(deviceUniqueId=%s): discover_graph failed ret=%d\n",
                             deviceUniqueId, ret);
                }
            }
            return 0;
        }

        // ==========================================================================
        // Init(int32_t deviceIndex, int32_t csi_id, stream_type, image_type)
        //   csi_id >= 0: 指定 CSI（csi0/csi1/csi2），按硬件编号选择
        //   csi_id < 0:  自动（任意 CSI，要求唯一匹配）
        //   stream_type: "fr"=全分辨率主路, "ds"=降采样子路
        //   image_type:  "YUYV"/"RGB"/"NV12"
        // ==========================================================================
        int32_t VideoCaptureModuleAm311x::Init(int32_t deviceIndex, int32_t csi_id,
                                                const char *stream_type, const char *image_type)
        {
            device_index_ = deviceIndex;
            csi_id_ = csi_id;
            stream_type_ = stream_type ? stream_type : "fr";
            image_type_ = image_type ? image_type : "YUYV";

            am311x_discovery::camera_v2_graph g;
            int ret = am311x_discovery::camera_v2_discover_graph(
                nullptr,                                    // media_device = nullptr → 扫全部
                csi_id,                                     // requested_csi：指定 CSI
                am311x_discovery::CAMERA_V2_GRAPH_CAP_FULL_ISP,  // FR+DS 都要有
                &g);
            if (ret != 0) {
                ump_error("Init(deviceIndex=%d, csi_id=%d): discover_graph failed ret=%d\n",
                          deviceIndex, csi_id, ret);
                return -1;
            }

            snprintf(fr_device_, sizeof(fr_device_), "%s", g.fr_device);
            snprintf(ds_device_, sizeof(ds_device_), "%s", g.ds_device);
            snprintf(subdev_device_, sizeof(subdev_device_), "%s", g.sensor_device);

            // 存到基类 _deviceUniqueId（用当前选择的流设备）
            const char *active_device = (stream_type_ == "ds") ? ds_device_ : fr_device_;
            if (_deviceUniqueId) { delete[] _deviceUniqueId; _deviceUniqueId = nullptr; }
            _deviceUniqueId = new (std::nothrow) char[strlen(active_device) + 1];
            if (_deviceUniqueId) strcpy(_deviceUniqueId, active_device);

            ump_info("Init(deviceIndex=%d, csi_id=%d, stream=%s, image=%s): "
                     "matched csi=%u subdev=%s fr=%s ds=%s (active=%s)\n",
                     deviceIndex, csi_id, stream_type_.c_str(), image_type_.c_str(),
                     g.csi_id, subdev_device_, fr_device_, ds_device_, active_device);
            return 0;
        }

        // ==========================================================================
        // ConfigureSensor
        // ==========================================================================
        //   - ISP 要求 sensor 必须输出 RAW8/RAW10，否则 STREAMON 返回 EOPNOTSUPP
        //   - FR 流：如果 current 已经是 RAW 则保持，否则设为 BG10 RAW
        // ==========================================================================
        int VideoCaptureModuleAm311x::ConfigureSensor()
        {
            if (!subdev_device_[0]) {
                ump_error("ConfigureSensor: subdev_device_ not set\n");
                return -1;
            }

            struct v4l2_subdev_format format;
            struct v4l2_subdev_frame_interval interval;

            // 1. open subdev
            subdev_fd_ = ::open(subdev_device_, O_RDWR | O_NONBLOCK);
            if (subdev_fd_ < 0) {
                ump_error("ConfigureSensor: open %s failed errno=%d\n",
                          subdev_device_, errno);
                return -errno;
            }

            // 2. 读当前 ACTIVE 格式 → 保存 original
            memset(&format, 0, sizeof(format));
            format.which = V4L2_SUBDEV_FORMAT_ACTIVE;
            format.pad = 0;
            if (IoctlEintrSafe(subdev_fd_, VIDIOC_SUBDEV_G_FMT, &format) < 0) {
                int ret = -errno;
                ump_error("ConfigureSensor: VIDIOC_SUBDEV_G_FMT failed errno=%d\n",
                          errno);
                ::close(subdev_fd_);
                subdev_fd_ = -1;
                return ret;
            }
            original_sensor_format_.format = format.format;

            // 读 original 帧间隔
            memset(&interval, 0, sizeof(interval));
            interval.pad = 0;
            if (!IoctlEintrSafe(subdev_fd_, VIDIOC_SUBDEV_G_FRAME_INTERVAL, &interval)) {
                original_sensor_format_.interval = interval.interval;
                original_sensor_format_.interval_valid = 1;
            }

            // 3. 判断是否需要设置 sensor 格式
            //    - DS 流：sensor 由 FR 流配置，DS 始终保持 current
            //    - FR 流：如果 current 已经是 RAW → 保持 current
            //    - FR 流且 current 不是 RAW → 设置为 BG10 RAW
            bool current_is_raw = MbusIsRaw(format.format.code);
            bool is_fr_stream = (stream_type_ == "fr");

            ump_info("ConfigureSensor: stream=%s current=%ux%u mbus=0x%04x is_raw=%d\n",
                     stream_type_.c_str(), format.format.width, format.format.height,
                     format.format.code, current_is_raw);

            if (!is_fr_stream) {
                ump_info("ConfigureSensor: keep ACTIVE %ux%u mbus=0x%04x [DS stream, skip sensor config]\n",
                         format.format.width, format.format.height, format.format.code);
                restore_sensor_ = false;
                ::close(subdev_fd_);
                subdev_fd_ = -1;
                return 0;
            }

            if (current_is_raw) {
                ump_info("ConfigureSensor: keep ACTIVE %ux%u mbus=0x%04x (RAW)\n",
                         format.format.width, format.format.height, format.format.code);
                restore_sensor_ = false;
                ::close(subdev_fd_);
                subdev_fd_ = -1;
                return 0;
            }

            // 4. FR 流且 current 不是 RAW：设置 sensor 为 BG10 RAW，分辨率保持 current
            format.format.code = MEDIA_BUS_FMT_SBGGR10_1X10;

            if (IoctlEintrSafe(subdev_fd_, VIDIOC_SUBDEV_S_FMT, &format) < 0) {
                int ret = -errno;
                ump_error("ConfigureSensor: VIDIOC_SUBDEV_S_FMT failed errno=%d (code=0x%04x)\n",
                          errno, format.format.code);
                ::close(subdev_fd_);
                subdev_fd_ = -1;
                return ret;
            }
            restore_sensor_ = true;

            // 5. 验证设置后是 RAW 格式（ISP 要求）
            if (!MbusIsRaw(format.format.code)) {
                ump_error("ConfigureSensor: sensor code 0x%04x is not RAW, ISP cannot process\n",
                          format.format.code);
                RestoreSensor();
                ::close(subdev_fd_);
                subdev_fd_ = -1;
                return -EOPNOTSUPP;
            }

            // 6. 读实际帧间隔
            memset(&interval, 0, sizeof(interval));
            interval.pad = 0;
            if (!IoctlEintrSafe(subdev_fd_, VIDIOC_SUBDEV_G_FRAME_INTERVAL, &interval)) {
                requested_cap_.maxFPS = interval.interval.denominator / interval.interval.numerator;
            }

            ump_info("ConfigureSensor: set ACTIVE %ux%u mbus=0x%04x (RAW), fps=%d\n",
                     format.format.width, format.format.height, format.format.code,
                     requested_cap_.maxFPS);

            ::close(subdev_fd_);
            subdev_fd_ = -1;
            return 0;
        }

        // ==========================================================================
        // RestoreSensor
        // ==========================================================================
        int VideoCaptureModuleAm311x::RestoreSensor()
        {
            struct v4l2_subdev_format format;
            struct v4l2_subdev_frame_interval interval;
            int fd;
            int ret = 0;

            if (!restore_sensor_)
                return 0;  // 没改过 original 不需要恢复

            if (!original_sensor_format_.format.code ||
                !original_sensor_format_.format.width ||
                !original_sensor_format_.format.height)
                return 0;
            if (!subdev_device_[0])
                return 0;

            fd = ::open(subdev_device_, O_RDWR | O_NONBLOCK);
            if (fd < 0) {
                ump_warn("RestoreSensor: open %s failed errno=%d (skip restore)\n",
                         subdev_device_, errno);
                return 0;
            }

            // S_FMT 恢复
            memset(&format, 0, sizeof(format));
            format.which = V4L2_SUBDEV_FORMAT_ACTIVE;
            format.pad = 0;
            format.format = original_sensor_format_.format;
            if (IoctlEintrSafe(fd, VIDIOC_SUBDEV_S_FMT, &format) < 0) {
                ump_warn("RestoreSensor: VIDIOC_SUBDEV_S_FMT failed errno=%d\n", errno);
                ret = -errno;
            }

            // 恢复帧间隔
            if (original_sensor_format_.interval_valid) {
                memset(&interval, 0, sizeof(interval));
                interval.pad = 0;
                interval.interval = original_sensor_format_.interval;
                if (IoctlEintrSafe(fd, VIDIOC_SUBDEV_S_FRAME_INTERVAL, &interval) < 0) {
                    ump_warn("RestoreSensor: VIDIOC_SUBDEV_S_FRAME_INTERVAL failed errno=%d\n",
                             errno);
                    ret = -errno;
                }
            }

            ::close(fd);
            ump_info("RestoreSensor: restored %s ACTIVE %ux%u mbus=0x%04x\n",
                     subdev_device_,
                     original_sensor_format_.format.width,
                     original_sensor_format_.format.height,
                     original_sensor_format_.format.code);
            return ret;
        }

        // ==========================================================================
        // PrepareStream
        // ==========================================================================
        // ==========================================================================
        int VideoCaptureModuleAm311x::PrepareStream()
        {
            struct v4l2_requestbuffers request;
            struct v4l2_capability capability;
            struct v4l2_format format;
            uint32_t capabilities;
            unsigned int i;
            char fourcc_text[5];
            int ret;
            const int MAX_REQBUFS_RETRY = 5;
            const int RETRY_INTERVAL_US = 20000;  // 20ms 间隔
            unsigned int alloc_count = 0;

            // 根据 stream_type 选择 FR 或 DS 设备
            int &capture_fd = (stream_type_ == "ds") ? ds_fd_ : fr_fd_;
            const char *capture_device = (stream_type_ == "ds") ? ds_device_ : fr_device_;
            ump_info("PrepareStream: stream_type=%s, device=%s\n", stream_type_.c_str(), capture_device);

            // 每次都 open（StopCapture 里已经 close 了）
            capture_fd = ::open(capture_device, O_RDWR | O_NONBLOCK);
            if (capture_fd < 0) {
                ret = -errno;
                ump_error("PrepareStream: open %s failed: %s\n", capture_device, strerror(errno));
                return ret;
            }

            // VIDIOC_QUERYCAP 检查 SINGLE_PLANE CAPTURE + STREAMING
            memset(&capability, 0, sizeof(capability));
            if (IoctlEintrSafe(capture_fd, VIDIOC_QUERYCAP, &capability) < 0) {
                ret = -errno;
                ump_error("%s: VIDIOC_QUERYCAP: %s\n", capture_device, strerror(errno));
                goto error;
            }
            capabilities = capability.capabilities;
            if (capabilities & V4L2_CAP_DEVICE_CAPS)
                capabilities = capability.device_caps;
            if (!(capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
                !(capabilities & V4L2_CAP_STREAMING)) {
                ump_error("%s: not a streaming single-plane capture node\n",
                          capture_device);
                ret = -ENODEV;
                goto error;
            }

            // DS 节点：确保 ds_mode = Normal（CPU 可取帧），0x009819d0 = V4L2_CID_USER_BASE + 0x19d0
            if (stream_type_ == "ds") {
                struct v4l2_control ds_ctrl;
                memset(&ds_ctrl, 0, sizeof(ds_ctrl));
                ds_ctrl.id = V4L2_CID_USER_BASE + 0x19d0;  // ds_mode control
                ds_ctrl.value = 0;  // 0 = Normal（CPU 取帧），1 = NPU 直连
                if (IoctlEintrSafe(capture_fd, VIDIOC_S_CTRL, &ds_ctrl) < 0) {
                    ump_warn("%s: set ds_mode=Normal failed (errno=%d), continuing anyway\n",
                             capture_device, errno);
                } else {
                    ump_info("%s: ds_mode set to Normal (CPU capture)\n", capture_device);
                }
            }

            // G_FMT 拿驱动默认格式，再按配置 S_FMT（sensor 已设为 RAW，ISP 就绪）
            memset(&format, 0, sizeof(format));
            format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (IoctlEintrSafe(capture_fd, VIDIOC_G_FMT, &format) < 0) {
                ret = -errno;
                ump_error("%s: VIDIOC_G_FMT: %s\n", capture_device, strerror(errno));
                goto error;
            }

            // 按配置覆盖 width/height/fourcc
            if (requested_cap_.width)  format.fmt.pix.width = requested_cap_.width;
            if (requested_cap_.height) format.fmt.pix.height = requested_cap_.height;
            if (requested_fourcc_)     format.fmt.pix.pixelformat = requested_fourcc_;
            format.fmt.pix.field = V4L2_FIELD_ANY;

            if (IoctlEintrSafe(capture_fd, VIDIOC_S_FMT, &format) < 0) {
                ret = -errno;
                ump_error("%s: VIDIOC_S_FMT: %s\n", capture_device, strerror(errno));
                goto error;
            }

            // 验证实际分辨率是否与请求一致
            if (format.fmt.pix.width != requested_cap_.width ||
                format.fmt.pix.height != requested_cap_.height) {
                ump_warn("%s: S_FMT requested %ux%u but got %ux%u\n",
                         capture_device, requested_cap_.width, requested_cap_.height,
                         format.fmt.pix.width, format.fmt.pix.height);
            }

            // 保存实际值
            width_ = format.fmt.pix.width;
            height_ = format.fmt.pix.height;
            fourcc_ = format.fmt.pix.pixelformat;
            bytesperline_ = format.fmt.pix.bytesperline;
            sizeimage_ = format.fmt.pix.sizeimage;
            FourccToString(fourcc_, fourcc_text);
            ump_info("%s: %s %ux%u %s bytesperline=%u sizeimage=%u\n",
                     stream_type_.c_str(), capture_device, width_, height_, fourcc_text,
                     bytesperline_, sizeimage_);

            // REQBUFS — 加重试应对 close 后 CMA 异步释放延迟
            for (int retry = 0; retry < MAX_REQBUFS_RETRY; retry++) {
                struct v4l2_requestbuffers clear_req;
                memset(&clear_req, 0, sizeof(clear_req));
                clear_req.count = 0;
                clear_req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                clear_req.memory = V4L2_MEMORY_MMAP;
                IoctlEintrSafe(capture_fd, VIDIOC_REQBUFS, &clear_req);

                if (retry > 0)
                    ::usleep(RETRY_INTERVAL_US);

                memset(&request, 0, sizeof(request));
                request.count = kDefaultBufferCount;
                request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                request.memory = V4L2_MEMORY_MMAP;
                if (IoctlEintrSafe(capture_fd, VIDIOC_REQBUFS, &request) < 0) {
                    ret = -errno;
                    ump_error("%s: VIDIOC_REQBUFS: %s\n", capture_device, strerror(errno));
                    goto error;
                }

                alloc_count = request.count;
                if (alloc_count >= 2) break;

                ump_warn("%s: REQBUFS(count=%d) granted %u buffers, retry %d/%d\n",
                         capture_device, kDefaultBufferCount, alloc_count,
                         retry + 1, MAX_REQBUFS_RETRY);
            }

            if (alloc_count < 2) {
                ump_error("%s: driver granted only %u buffers (requested %d) after %d retries\n",
                         capture_device, alloc_count, kDefaultBufferCount,
                         MAX_REQBUFS_RETRY);
                ret = -ENOMEM;
                goto error;
            }
            buffer_count_ = alloc_count;

            // mmap 每个 buffer
            buffers_ = new (std::nothrow) struct MmapBuffer[buffer_count_];
            if (!buffers_) { ret = -ENOMEM; goto error; }
            memset(buffers_, 0, sizeof(MmapBuffer) * buffer_count_);

            for (i = 0; i < buffer_count_; i++) {
                struct v4l2_buffer buffer;
                memset(&buffer, 0, sizeof(buffer));
                buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buffer.memory = V4L2_MEMORY_MMAP;
                buffer.index = i;

                if (IoctlEintrSafe(capture_fd, VIDIOC_QUERYBUF, &buffer) < 0) {
                    ret = -errno;
                    ump_error("%s: VIDIOC_QUERYBUF[%u]: %s\n",
                              capture_device, i, strerror(errno));
                    goto error;
                }

                buffers_[i].length = buffer.length;
                buffers_[i].address = ::mmap(nullptr, buffer.length,
                                             PROT_READ | PROT_WRITE, MAP_SHARED,
                                             capture_fd, buffer.m.offset);
                if (buffers_[i].address == MAP_FAILED) {
                    buffers_[i].address = nullptr;
                    ret = -errno;
                    ump_error("%s: mmap[%u]: %s\n", capture_device, i, strerror(errno));
                    goto error;
                }
                mapped_count_++;
            }

            // 全部初始 QBUF
            for (i = 0; i < buffer_count_; i++) {
                struct v4l2_buffer buffer;
                memset(&buffer, 0, sizeof(buffer));
                buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buffer.memory = V4L2_MEMORY_MMAP;
                buffer.index = i;
                if (IoctlEintrSafe(capture_fd, VIDIOC_QBUF, &buffer) < 0) {
                    ret = -errno;
                    ump_error("%s: initial VIDIOC_QBUF[%u]: %s\n",
                              capture_device, i, strerror(errno));
                    goto error;
                }
            }

            ump_info("%s: buffers requested=%d granted=%u mapped=%u, all queued\n",
                     stream_type_.c_str(), kDefaultBufferCount, buffer_count_, mapped_count_);
            return 0;

        error:
            DeallocateStream();
            if (fr_fd_ >= 0) { ::close(fr_fd_); fr_fd_ = -1; }
            if (ds_fd_ >= 0) { ::close(ds_fd_); ds_fd_ = -1; }
            return ret;
        }

        // ==========================================================================
        // DeallocateStream
        // ==========================================================================
        // ==========================================================================
        void VideoCaptureModuleAm311x::DeallocateStream()
        {
            unsigned int i;

            // munmap
            if (buffers_) {
                for (i = 0; i < mapped_count_; i++) {
                    if (buffers_[i].address)
                        ::munmap(buffers_[i].address, buffers_[i].length);
                }
                delete[] buffers_;
                buffers_ = nullptr;
                mapped_count_ = 0;
                buffer_count_ = 0;
            }

            // REQBUFS(count=0) 释放驱动缓冲（FR 和 DS 都清）
            if (fr_fd_ >= 0) {
                struct v4l2_requestbuffers request;
                memset(&request, 0, sizeof(request));
                request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                request.memory = V4L2_MEMORY_MMAP;
                IoctlEintrSafe(fr_fd_, VIDIOC_REQBUFS, &request);
            }
            if (ds_fd_ >= 0) {
                struct v4l2_requestbuffers request;
                memset(&request, 0, sizeof(request));
                request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                request.memory = V4L2_MEMORY_MMAP;
                IoctlEintrSafe(ds_fd_, VIDIOC_REQBUFS, &request);
            }
        }

        // ==========================================================================
        // StartCapture
        // ==========================================================================
        int32_t VideoCaptureModuleAm311x::StartCapture(const VideoCaptureCapability &capability)
        {
            std::lock_guard<std::mutex> lock(capture_lock_);

            if (capture_started_.load(std::memory_order_acquire)) {
                // 已经在跑——检查参数是否相同
                // fourcc_ 是 PrepareStream 后才知道的；这里只比 width/height，帧格式后续自然会对
                if (capability.width == width_ &&
                    capability.height == height_ &&
                    FourccToVideoType(fourcc_) == capability.videoType)
                    return 0;
                ump_error("StartCapture: cannot change params while running\n");
                return -1;
            }

            // 保存请求参数
            requested_cap_.width = capability.width;
            requested_cap_.height = capability.height;
            requested_cap_.maxFPS = capability.maxFPS;
            requested_cap_.videoType = capability.videoType;

            // 优先根据 image_type_ 选格式（配置文件指定），否则按 videoType
            ump_info("[CAP-DEBUG] StartCapture videoType=%d image_type=%s stream_type=%s\n",
                    (int)capability.videoType, image_type_.c_str(), stream_type_.c_str());

            if (image_type_ == "RGB") {
                requested_fourcc_ = V4L2_PIX_FMT_RGB24;
            } else if (image_type_ == "YUYV" || image_type_ == "YUY2") {
                requested_fourcc_ = V4L2_PIX_FMT_YUYV;
            } else if (image_type_ == "NV12") {
                requested_fourcc_ = V4L2_PIX_FMT_NV12;
            } else if (image_type_ == "UYVY") {
                requested_fourcc_ = V4L2_PIX_FMT_UYVY;
            } else if (image_type_ == "I420" || image_type_ == "YUV420") {
                requested_fourcc_ = V4L2_PIX_FMT_YUV420;
            } else {
                // image_type 未指定，按 videoType 选
                switch (capability.videoType) {
                case video_common::VideoType::kNV12:
                    requested_fourcc_ = V4L2_PIX_FMT_NV12;
                    break;
                case video_common::VideoType::kYUY2:
                    requested_fourcc_ = V4L2_PIX_FMT_YUYV;
                    break;
                case video_common::VideoType::kUYVY:
                    requested_fourcc_ = V4L2_PIX_FMT_UYVY;
                    break;
                case video_common::VideoType::kI420:
                    requested_fourcc_ = V4L2_PIX_FMT_YUV420;
                    break;
                default:
                    requested_fourcc_ = V4L2_PIX_FMT_YUYV;  // 默认 YUYV
                    break;
                }
            }
            {
                char fc[5];
                FourccToString(requested_fourcc_, fc);
                ump_info("[CAP-DEBUG] StartCapture requested_fourcc=%s (0x%08x)\n", fc, requested_fourcc_);
            }

            // ===== 步骤 1：ConfigureSensor=====
            int ret = ConfigureSensor();
            if (ret) {
                ump_error("StartCapture: ConfigureSensor failed ret=%d\n", ret);
                return -1;
            }

            // ===== 步骤 2：PrepareStream=====
            ret = PrepareStream();
            if (ret) {
                ump_error("StartCapture: PrepareStream failed ret=%d\n", ret);
                return -1;  // PrepareStream 内部已经调 cleanup
            }

            // ===== 步骤 3：VIDIOC_STREAMON=====
            {
                int &capture_fd = (stream_type_ == "ds") ? ds_fd_ : fr_fd_;
                const char *capture_device = (stream_type_ == "ds") ? ds_device_ : fr_device_;
                enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                if (IoctlEintrSafe(capture_fd, VIDIOC_STREAMON, &type) < 0) {
                    ret = -errno;
                    ump_error("%s: VIDIOC_STREAMON: %s\n", capture_device, strerror(errno));
                    DeallocateStream();
                    ::close(capture_fd);
                    if (stream_type_ == "ds") ds_fd_ = -1; else fr_fd_ = -1;
                    return -1;
                }
                ump_info("StartCapture: STREAMON OK stream=%s fd=%d device=%s, %u buffers queued\n",
                         stream_type_.c_str(), capture_fd, capture_device, buffer_count_);
            }

            // ===== 步骤 4：启采集线程（和 RK 一样的 std::thread + RT 优先级）=====
            captured_ = 0;
            last_sequence_ = 0;
            have_sequence_ = false;
            quit_.store(false, std::memory_order_release);
            capture_started_.store(true, std::memory_order_release);

            capture_thread_ = std::thread([this] {
                ::prctl(PR_SET_NAME, "am311x_capture");

                int policy;
                struct sched_param param;
                pthread_getschedparam(pthread_self(), &policy, &param);
                param.sched_priority = 40;
                pthread_setschedparam(pthread_self(), SCHED_RR, &param);

                while (!quit_.load(std::memory_order_acquire)) {
                    if (!CaptureProcess())
                        break;
                }
            });

            ump_info("StartCapture: %s %ux%u %dfps started\n",
                     (stream_type_ == "ds") ? ds_device_ : fr_device_,
                     width_, height_, requested_cap_.maxFPS);
            return 0;
        }

        // ==========================================================================
        // StopCapture
        // ==========================================================================
        int32_t VideoCaptureModuleAm311x::StopCapture()
        {
            // 1. 通知线程退出
            quit_.store(true, std::memory_order_release);

            // 2. 等线程结束
            if (capture_thread_.joinable()) {
                capture_thread_.join();
            }

            std::lock_guard<std::mutex> lock(capture_lock_);

            if (!capture_started_.load(std::memory_order_acquire))
                return 0;

            // 3. STREAMOFF（FR 和 DS 都停）
            if (fr_fd_ >= 0) {
                enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                IoctlEintrSafe(fr_fd_, VIDIOC_STREAMOFF, &type);
            }
            if (ds_fd_ >= 0) {
                enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                IoctlEintrSafe(ds_fd_, VIDIOC_STREAMOFF, &type);
            }

            // 4. munmap + REQBUFS(0) — 必须在 close 之前！
            DeallocateStream();

            // 5. close fd — 彻底释放，下次 StartCapture 重新 open
            if (fr_fd_ >= 0) {
                ::close(fr_fd_);
                fr_fd_ = -1;
            }
            if (ds_fd_ >= 0) {
                ::close(ds_fd_);
                ds_fd_ = -1;
            }

            capture_started_.store(false, std::memory_order_release);
            ump_info("StopCapture: stopped, captured=%u frames\n", captured_);
            return 0;
        }

        // ==========================================================================
        // CaptureStarted / CaptureSettings — simple getters
        // ==========================================================================
        bool VideoCaptureModuleAm311x::CaptureStarted()
        {
            return capture_started_.load(std::memory_order_acquire);
        }

        int32_t VideoCaptureModuleAm311x::CaptureSettings(VideoCaptureCapability &settings)
        {
            settings.width = width_;
            settings.height = height_;
            settings.maxFPS = requested_cap_.maxFPS;
            settings.videoType = FourccToVideoType(fourcc_);
            return 0;
        }

        // ==========================================================================
        // CaptureProcess
        // ==========================================================================
        bool VideoCaptureModuleAm311x::CaptureProcess()
        {
            // 根据 stream_type 选择活跃的 fd
            int capture_fd = (stream_type_ == "ds") ? ds_fd_ : fr_fd_;

            // while 循环，quit 在入口检查
            while (!quit_.load(std::memory_order_acquire)) {
                struct pollfd poll_fd;
                int poll_ret;
                int ret;

                poll_fd.fd = capture_fd;
                poll_fd.events = POLLIN | POLLPRI;
                poll_fd.revents = 0;
                poll_ret = ::poll(&poll_fd, 1, timeout_ms_);


                // poll 失败，EINTR 重试，其他返回 -errno
                if (poll_ret < 0) {
                    if (errno == EINTR)
                        continue;  // 被信号打断，继续 while
                    ump_error("CaptureProcess: poll failed errno=%d\n", errno);
                    return false;
                }

                // poll 超时
                if (!poll_ret) {
                    fprintf(stderr, "[CAPTURE][%s] poll timed out (%dms), fd=%d quit=%d\n",
                            stream_type_.c_str(), timeout_ms_, capture_fd, quit_.load());
                    fflush(stderr);
                    continue;  // 超时不退出，回去检查 quit 再等
                }

                // revents 错误检查
                if (poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    ump_error("CaptureProcess: poll revents=0x%x (ERR|HUP|NVAL)\n",
                              poll_fd.revents);
                    return false;
                }

                // 必须有 POLLIN|POLLPRI 才处理
                if (!(poll_fd.revents & (POLLIN | POLLPRI))) {
                    ump_warn("CaptureProcess: poll revents=0x%x (no POLLIN/PRI)\n",
                             poll_fd.revents);
                    continue;
                }

                // ========== 加锁处理帧 ==========
                {
                    std::lock_guard<std::mutex> lock(capture_lock_);

                    // dequeue
                    struct v4l2_buffer buf;
                    memset(&buf, 0, sizeof(buf));
                    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                    buf.memory = V4L2_MEMORY_MMAP;

                    if (IoctlEintrSafe(capture_fd, VIDIOC_DQBUF, &buf) < 0) {
                        ret = -errno;
                        ump_error("CaptureProcess: VIDIOC_DQBUF failed errno=%d\n", errno);
                        if (ret == -EAGAIN)
                            continue;
                        return false;
                    }

                    // bytesused 有效性检查
                    if (!buf.bytesused || buf.index >= buffer_count_ || buf.index >= mapped_count_) {
                        ump_warn("CaptureProcess: invalid buffer index=%u bytesused=%u\n",
                                 buf.index, buf.bytesused);
                        IoctlEintrSafe(capture_fd, VIDIOC_QBUF, &buf);
                        continue;
                    }

                    // sequence gap 检测
                    if (have_sequence_ && buf.sequence != last_sequence_ + 1) {
                        ump_warn("CaptureProcess: sequence gap %u -> %u\n",
                                 last_sequence_, buf.sequence);
                    }
                    last_sequence_ = buf.sequence;
                    have_sequence_ = true;

                    // IncomingFrame 回调（ump 框架要求的接口）
                    VideoCaptureCapability frameInfo;
                    frameInfo.width = width_;
                    frameInfo.height = height_;
                    frameInfo.videoType = FourccToVideoType(fourcc_);

                    IncomingFrame(
                        reinterpret_cast<uint8_t *>(buffers_[buf.index].address),
                        buf.bytesused,
                        frameInfo,
                        buf.timestamp.tv_sec * 1000 + buf.timestamp.tv_usec / 1000,
                        buf.sequence);

                    // captured 计数
                    captured_++;

                    // 归还 buffer
                    if (IoctlEintrSafe(capture_fd, VIDIOC_QBUF, &buf) < 0) {
                        ump_error("CaptureProcess: VIDIOC_QBUF failed errno=%d\n", errno);
                        return false;
                    }
                }
            }

            ump_info("CaptureProcess: quit signalled, captured=%u frames\n", captured_);
            return false;
        }

        // ==========================================================================
        // 辅助函数
        // ==========================================================================

        bool VideoCaptureModuleAm311x::MbusIsRaw(uint32_t code)
        {
            switch (code) {
            case MEDIA_BUS_FMT_SBGGR8_1X8:
            case MEDIA_BUS_FMT_SGBRG8_1X8:
            case MEDIA_BUS_FMT_SGRBG8_1X8:
            case MEDIA_BUS_FMT_SRGGB8_1X8:
            case MEDIA_BUS_FMT_SBGGR10_1X10:
            case MEDIA_BUS_FMT_SGBRG10_1X10:
            case MEDIA_BUS_FMT_SGRBG10_1X10:
            case MEDIA_BUS_FMT_SRGGB10_1X10:
                return true;
            default:
                return false;
            }
        }

        void VideoCaptureModuleAm311x::FourccToString(uint32_t fourcc, char text[5])
        {
            text[0] = fourcc & 0xff;
            text[1] = (fourcc >> 8) & 0xff;
            text[2] = (fourcc >> 16) & 0xff;
            text[3] = (fourcc >> 24) & 0xff;
            text[4] = '\0';
        }

        // fourcc → video_common::VideoType（StartCapture 里也用得到）
        video_common::VideoType VideoCaptureModuleAm311x::FourccToVideoType(uint32_t fourcc)
        {
            switch (fourcc) {
            case V4L2_PIX_FMT_NV12:  return video_common::VideoType::kNV12;
            case V4L2_PIX_FMT_YUYV:  return video_common::VideoType::kYUY2;
            case V4L2_PIX_FMT_UYVY:  return video_common::VideoType::kUYVY;
            case V4L2_PIX_FMT_YUV420: return video_common::VideoType::kI420;
            case V4L2_PIX_FMT_RGB24: return video_common::VideoType::kRGB24;
            default:                  return video_common::VideoType::kNV12;
            }
        }

    } // namespace videocapturemodule

} // namespace ecovacs
