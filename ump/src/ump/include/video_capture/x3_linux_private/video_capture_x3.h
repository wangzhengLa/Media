#ifndef VIDEO_CAPTURE_X3_H_
#define VIDEO_CAPTURE_X3_H_

#include <stddef.h>
#include <stdint.h>
#include <memory>
#include <mutex>
#include <thread>
#include "osp.h"
#include "video_capture_defines.h"
#include "video_capture_impl.h"

#include "stdint.h"
#include "camera.h"
#include "stddef.h"

#include "vio/hb_vio_interface.h"
#include "vio/hb_vps_api.h"
#include "vio/hb_vp_api.h"

extern "C"
{
#include "vio/hb_mode.h"
#include "vio/hb_isp_api.h"
}

#define MDS_X3_VIDEO_ORIGIN_STREAM_SKIP_FRAMES 100

namespace ecovacs
{
    namespace videocapturemodule
    {
        class VideoCaptureModuleX3 : public VideoCaptureImpl
        {
        public:
            VideoCaptureModuleX3();
            ~VideoCaptureModuleX3() override;
            int32_t Init(const char *deviceUniqueId);
            int32_t StartCapture(const VideoCaptureCapability &capability) override;
            int32_t StopCapture() override;
            bool CaptureStarted() override;
            int32_t CaptureSettings(VideoCaptureCapability &settings) override;

        private:
            enum
            {
                kNoOfX3Bufffers = 4
            };
            typedef struct X3_VIN_INFO_S
            {
                int dev_id;
                MIPI_SENSOR_INFO_S mipi_sensor_info;
                MIPI_ATTR_S mipi_attr;
                VIN_PIPE_ATTR_S vin_pipe_attr;
                VIN_DEV_ATTR_S vin_dev_attr;
                VIN_LDC_ATTR_S vin_ldc_attr;
                VIN_DIS_ATTR_S vin_dis_attr;
            } X3_VIN_INFO;

            typedef struct
            {
                char *camera_type;
                VIN_PIPE_ATTR_S *vin_pipe;
                VIN_DEV_ATTR_S *vin_dev;
                MIPI_ATTR_S *mipi;
                MIPI_SENSOR_INFO_S *mipi_sensor;
            } X3_Vin_Param_t;

            X3_Vin_Param_t _x3cameraVinVec[3] = {
                {"sc1330t",
                 &PIPE_ATTR_SC1330T_LINEAR_BASE,
                 &DEV_ATTR_SC1330T_LINEAR_BASE,
                 &MIPI_1LANE_SENSOR_SC1330T_30FPS_10BIT_NORMAL_ATTR,
                 &SENSOR_1LANE_SC1330T_30FPS_10BIT_LINEAR_INFO},
                {"h65",
                 &PIPE_ATTR_H65_LINEAR_BASE,
                 &DEV_ATTR_H65_LINEAR_BASE,
                 &MIPI_1LANE_SENSOR_H65_30FPS_10BIT_NORMAL_ATTR,
                 &SENSOR_1LANE_H65_30FPS_10BIT_LINEAR_INFO},
                {"f37",
                 &PIPE_ATTR_F37_LINEAR_BASE,
                 &DEV_ATTR_F37_LINEAR_BASE,
                 &MIPI_1LANE_SENSOR_F37_30FPS_10BIT_NORMAL_ATTR,
                 &SENSOR_1LANE_F37_30FPS_10BIT_LINEAR_INFO}};
            X3_Vin_Param_t _x3cameraVinVec_Hdr[3] = {
                {"sc1330t",
                 &PIPE_ATTR_SC1330T_DOL2_BASE,
                 &DEV_ATTR_SC1330T_DOL2_BASE,
                 &MIPI_1LANE_SENSOR_SC1330T_30FPS_10BIT_DOL2_ATTR,
                 &SENSOR_1LANE_SC1330T_30FPS_10BIT_DOL2_INFO},
                {"h65",
                 &PIPE_ATTR_H65_LINEAR_BASE,
                 &DEV_ATTR_H65_LINEAR_BASE,
                 &MIPI_1LANE_SENSOR_H65_30FPS_10BIT_NORMAL_ATTR,
                 &SENSOR_1LANE_H65_30FPS_10BIT_LINEAR_INFO},
                {"f37",
                 &PIPE_ATTR_F37_LINEAR_BASE,
                 &DEV_ATTR_F37_LINEAR_BASE,
                 &MIPI_1LANE_SENSOR_F37_30FPS_10BIT_NORMAL_ATTR,
                 &SENSOR_1LANE_F37_30FPS_10BIT_LINEAR_INFO}};

            static void CaptureThread(void *);
            bool CaptureProcess();

            int32_t OriginVideoInit();
            int32_t iot_sensor_init(int dev_id);
            int32_t iot_mipi_init(int mipi_host_id);
            int32_t iot_sensor_deinit(int devId);
            int32_t iot_mipi_deinit(int mipiIdx);
            int32_t iot_sensor_stop(int devId);
            int32_t iot_mipi_stop(int mipiIdx);
            void iot_vin_deinit(int pipeId, SYS_VIN_VPS_MODE_E mode);
            int32_t iot_vin_init(uint32_t pipe_id, SYS_VIN_VPS_MODE_E mode);
            void iot_vin_stop(int pipeId, SYS_VIN_VPS_MODE_E mode);
            int32_t X3VideoGetCamParamPtr(int32_t hdr, char *camera_type);

            std::thread _captureThread;
            std::mutex capture_lock_;
            bool quit_;
            int32_t _deviceId;
            int32_t _deviceFd;

            int32_t _currentWidth;
            int32_t _currentHeight;
            int32_t _currentFrameRate;
            int32_t _numPlanes;
            bool _captureStarted;
            video_common::VideoType _captureVideoType;

            int32_t _hdr;
            int32_t _originStatus;
            char _camera_type[64];
            X3_VIN_INFO x3_vin_info;
            X3_Vin_Param_t *_x3VinAttr;
        };

    } // namespace videocapturemodule

} // namespace ecovacs

#endif // VIDEO_CAPTURE_X3_H_
