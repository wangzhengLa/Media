/**
 * @file    video_capture_am311x.h
 * @brief   AM311x 平台视频采集（Media Controller + V4L2）
 *
 * 框架：ump VideoCaptureModule 接口（与 RK VideoCaptureModuleV4L2 对齐）
 *   Init / StartCapture / StopCapture / CaptureProcess / CaptureStarted / CaptureSettings
 *
 * 实现：移植自 am311x_open_sdk camera_v2_capture.c + am_camera.c
 *   - ConfigureSensor / RestoreSensor    ← subdev RAW 格式配置
 *   - PrepareStream / DeallocateStream   ← V4L2 stream 准备/清理
 *   - CaptureProcess                     ← poll 循环（不是 select！）
 *   - IoctlEintrSafe / MbusIsRaw / FourccToString / FourccToVideoType ← 工具函数
 */

#ifndef VIDEO_CAPTURE_AM311X_H_
#define VIDEO_CAPTURE_AM311X_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <stddef.h>
#include <stdint.h>
#include <thread>

#include <linux/v4l2-subdev.h>

#include "video_capture_impl.h"
#include "video_capture_defines.h"

namespace ecovacs
{
    namespace videocapturemodule
    {
        class VideoCaptureModuleAm311x : public VideoCaptureImpl
        {
        public:
            VideoCaptureModuleAm311x();
            ~VideoCaptureModuleAm311x() override;

            // ===== ump VideoCaptureModule 接口 =====
            int32_t Init(const char *deviceUniqueId);
            int32_t Init(int32_t deviceIndex, int32_t csi_id,
                         const char *stream_type = "fr", const char *image_type = "YUYV");

            int32_t StartCapture(const VideoCaptureCapability &capability) override;
            int32_t StopCapture() override;
            bool CaptureStarted() override;
            int32_t CaptureSettings(VideoCaptureCapability &settings) override;

        private:
            // ===== 常量 =====
            static constexpr int kDefaultBufferCount = 8;
            static constexpr int kDefaultTimeoutMs  = 2000;

            // ===== V4L2 / subdev fd =====
            int fr_fd_;             ///< FR 通道 V4L2（如 /dev/video3）
            int ds_fd_;             ///< DS 通道 V4L2（如 /dev/video4）
            int subdev_fd_;         ///< sensor subdev（如 /dev/v4l-subdev0）

            // ===== 设备路径 =====
            char fr_device_[128];
            char ds_device_[128];
            char subdev_device_[128];
            int32_t device_index_;
            int32_t csi_id_;        ///< CSI 编号：-1=自动，0/1/2=指定
            std::string stream_type_;  ///< "fr" 或 "ds"
            std::string image_type_;   ///< "YUYV"/"RGB"/"NV12"

            // ===== V4L2 stream 参数（PrepareStream 后填入）=====
            uint32_t width_;
            uint32_t height_;
            uint32_t fourcc_;       ///< V4L2 pixelformat（如 V4L2_PIX_FMT_YUYV）
            uint32_t bytesperline_;
            uint32_t sizeimage_;
            uint32_t buffer_count_;
            int timeout_ms_;        ///< poll timeout（默认 2000ms）

            // ===== 帧回调用 =====
            struct RequestedCapability
            {
                uint32_t width;
                uint32_t height;
                int32_t maxFPS;
                video_common::VideoType videoType;
            };
            RequestedCapability requested_cap_;
            uint32_t requested_fourcc_;   ///< StartCapture 时根据 videoType 选

            // ===== mmap buffers =====
            struct MmapBuffer
            {
                void *address;
                size_t length;
            };
            MmapBuffer *buffers_;
            int32_t mapped_count_;

            // ===== sensor 原始格式（ConfigureSensor 保存，RestoreSensor 恢复）=====
            struct OriginalSensorFormat
            {
                struct v4l2_mbus_framefmt format;
                struct v4l2_fract interval;
                int interval_valid;
            };
            OriginalSensorFormat original_sensor_format_;
            bool restore_sensor_;

            // ===== capture 统计 + sequence gap =====
            uint32_t captured_;
            uint32_t last_sequence_;
            bool have_sequence_;

            // ===== 线程同步 =====
            std::thread capture_thread_;
            std::mutex capture_lock_;
            std::atomic<bool> quit_;
            std::atomic<bool> capture_started_;

            // ===== 移植自 camera_v2_capture.c 的内部方法 =====
            // 工具函数
            static int IoctlEintrSafe(int fd, unsigned long request, void *arg);
            static bool MbusIsRaw(uint32_t code);
            static void FourccToString(uint32_t fourcc, char text[5]);
            static video_common::VideoType FourccToVideoType(uint32_t fourcc);

            // stream 管理（camera_v2_stream_prepare / cleanup 的类成员化）
            int PrepareStream();
            void DeallocateStream();

            // sensor 管理（camera_v2_configure_sensor / restore_sensor）
            int ConfigureSensor();
            int RestoreSensor();

            // 采集循环
            bool CaptureProcess();
        };

    } // namespace videocapturemodule

} // namespace ecovacs

#endif // VIDEO_CAPTURE_AM311X_H_
