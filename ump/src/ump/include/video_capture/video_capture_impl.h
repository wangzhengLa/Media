#ifndef VIDEO_CAPTURE_IMPL_H_
#define VIDEO_CAPTURE_IMPL_H_

/*
 * video_capture_impl.h
 */

#include <stddef.h>
#include <stdint.h>
#include <mutex>
#include "video_common_defines.h"
#include "video_capture.h"
#include "video_capture_config.h"
#include "video_capture_defines.h"

namespace ecovacs
{
    namespace videocapturemodule
    {
        // Class definitions
        class VideoCaptureImpl : public VideoCaptureModule
        {
        public:
            /*
             *   Create a video capture module object
             *
             *   id              - unique identifier of this video capture module object
             *   deviceUniqueIdUTF8 -  name of the device. Available names can be found by
             * using GetDeviceName
             */
            static VideoCaptureModule *Create(const char *deviceUniqueIdUTF8);

            static VideoCaptureModule *Create(int32_t deviceIndex);

            static DeviceInfo *CreateDeviceInfo();

            // Helpers for converting between (integral) degrees and
            // VideoRotation values.  Return 0 on success.
            static int32_t RotationFromDegrees(int degrees, video_common::VideoRotation *rotation);
            static int32_t RotationInDegrees(video_common::VideoRotation rotation, int *degrees);

            // Call backs
            void RegisterCaptureDataCallback(video_common::VideoSinkInterface *dataCallback) override;
            void DeRegisterCaptureDataCallback() override;

            int32_t SetCaptureRotation(video_common::VideoRotation rotation) override;
            bool SetApplyRotation(bool enable) override;
            bool GetApplyRotation() override;

            const char *CurrentDeviceName() const override;

            // `capture_time` must be specified in NTP time format in milliseconds.
            int32_t IncomingFrame(uint8_t *videoFrame,
                                  size_t videoFrameLength,
                                  const VideoCaptureCapability &frameInfo,
                                  int64_t captureTime, uint32_t sequence);
            // `capture_time` must be specified in NTP time format in milliseconds.
            int32_t IncomingFrame(video_common::VideoFrameBuffer *buffer);

            // Platform dependent
            int32_t StartCapture(const VideoCaptureCapability &capability) override;
            int32_t StopCapture() override;
            bool CaptureStarted() override;
            int32_t CaptureSettings(VideoCaptureCapability & /*settings*/) override;

        protected:
            VideoCaptureImpl();
            ~VideoCaptureImpl() override;

            char *_deviceUniqueId; // current Device unique name;
            std::mutex api_lock_;
            VideoCaptureCapability _requestedCapability; // Should be set by platform
                                                         // dependent code in
                                                         // StartCapture.
        private:
            void UpdateFrameCount();
            uint32_t CalculateFrameRate(int64_t now_ns);
            int32_t DeliverCapturedFrame(video_common::VideoFrameBuffer *captureFrame);

            // last time the module process function was called.
            int64_t _lastProcessTimeNanos;
            // last time the frame rate callback function was called.
            int64_t _lastFrameRateCallbackTimeNanos;

            video_common::VideoSinkInterface *_dataCallBack;

            int64_t _lastProcessFrameTimeNanos;
            // timestamp for local captured frames
            int64_t _incomingFrameTimesNanos[kFrameRateCountHistorySize];
            video_common::VideoRotation _rotateFrame; // Set if the frame should be rotated by the
                                                      // capture module.

            // Indicate whether rotation should be applied before delivered externally.
            bool apply_rotation_;
        };
    } // namespace videocapturemodule

} // namespace ecovacs

#endif // VIDEO_CAPTURE_IMPL_H_
