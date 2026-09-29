#include "video_capture_x3.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#include <new>
#include <string>
#include "video_capture.h"
#include "libyuv.h"
#include "osp.h"
#include <com/amazonaws/kinesis/video/webrtcclient/log.h>

namespace ecovacs
{
    namespace videocapturemodule
    {
        VideoCaptureModuleX3::VideoCaptureModuleX3()
            : VideoCaptureImpl(),
              _deviceId(-1),
              _deviceFd(-1),
              _currentWidth(-1),
              _currentHeight(-1),
              _currentFrameRate(-1),
              _captureStarted(false),
              _captureVideoType(video_common::VideoType::kI420) {}

        int32_t VideoCaptureModuleX3::Init(const char *deviceUniqueIdUTF8)
        {
            int len = strlen((const char *)deviceUniqueIdUTF8);
            _deviceUniqueId = new (std::nothrow) char[len + 1];
            if (_deviceUniqueId)
            {
                memcpy(_deviceUniqueId, deviceUniqueIdUTF8, len + 1);
            }
            _deviceId = 0; // store the device id

            _hdr = 0;
            strncpy(_camera_type, "sc1330t", sizeof(_camera_type));

            if (X3VideoGetCamParamPtr(_hdr, _camera_type))
            {
                error("X3VideoGetCamParamPtr fail _hdr=%d,_camera_type=%s\n", _hdr, _camera_type);
                return -1;
            }

            info("vin_pipe:%p,vin_dev:%p,mipi:%p,mipi_sensor:%p\n", _x3VinAttr->vin_pipe,
                 _x3VinAttr->vin_dev, _x3VinAttr->mipi, _x3VinAttr->mipi_sensor);
            return 0;
        }

        VideoCaptureModuleX3::~VideoCaptureModuleX3()
        {
            _deviceFd = -1;
        }

        int32_t VideoCaptureModuleX3::StartCapture(const VideoCaptureCapability &capability)
        {
            if (_captureStarted)
            {
                if (capability.width == _currentWidth &&
                    capability.height == _currentHeight &&
                    _captureVideoType == capability.videoType)
                {
                    return 0;
                }
                else
                {
                    StopCapture();
                }
            }

            // first open /dev/video device
            _deviceFd = 1;

            _captureVideoType = video_common::VideoType::kNV12;

            // initialize current width and height
            _currentWidth = 1280;
            _currentHeight = 960;
            _numPlanes = 2;

            // Trying to set frame rate, before check driver capability.
            bool driver_framerate_support = true;

            // If driver doesn't support framerate control, need to hardcode.
            // Hardcoding the value based on the frame size.
            if (!driver_framerate_support)
            {
                if (_currentWidth >= 800 && _captureVideoType != video_common::VideoType::kMJPEG)
                {
                    _currentFrameRate = capability.maxFPS;
                }
                else
                {
                    _currentFrameRate = capability.maxFPS;
                }
            }

            info("final choose _currentWidth[%d] _currentHeight[%d] _currentFrameRate[%d] _captureVideoType[%d]\r\n",
                 _currentWidth, _currentHeight, _currentFrameRate, _captureVideoType);

            info("Start Init X3 Capture\n");
            int ret = OriginVideoInit();
            if (ret != 0)
            {
                info("Init X3 Capture Fail\n");
                return -1;
            }
            info("Init X3 Capture Success\n");

            _originStatus = 1;

            // 处理跳帧
            hb_vio_buffer_t out_buffer;
            int cnt = 0;
            int err_cnt = 0;
            while (1)
            {
                // skip origin several frames
                if (cnt >= MDS_X3_VIDEO_ORIGIN_STREAM_SKIP_FRAMES || err_cnt >= MDS_X3_VIDEO_ORIGIN_STREAM_SKIP_FRAMES * 2)
                    break;
                ret = HB_VIN_GetChnFrame(0, 0, &out_buffer, 50); // 50ms
                if (ret == 0)
                {
                    cnt++;
                    ret = HB_VIN_ReleaseChnFrame(0, 0, &out_buffer);
                    if (ret != 0)
                    {
                        error("HB_VIN_ReleaseChnFrame Failed. ret = %d\n", ret);
                    }
                }
                else
                {
                    err_cnt++;
                }
            }
            info("X3 Capture Skip Frame Success cnt:%d err_cnt:%d\n", cnt, err_cnt);

            ISP_AE_ATTR_S attr;
            memset(&attr, 0, sizeof(attr));
            attr.enOpType = 0;

            ret = HB_ISP_GetAeAttr(0, &attr);
            info("HB_ISP_GetAeAttr\n");
            if (ret != 0)
            {
                error("HB_ISP_GetAeAttr fail ret= %d\n", ret);
            }
            else
            {
                info("after_set:\n");
                info("u32MaxIntegrationTime=%d u32IntegrationTime=%d u32SensorAnalogGain=%d u32IspDigitalGain=%d\n",
                    attr.u32MaxIntegrationTime, attr.u32IntegrationTime, attr.u32SensorAnalogGain, attr.u32IspDigitalGain);
            }

            // start capture thread;
            std::lock_guard<std::mutex> guard(capture_lock_);
            {
                quit_ = false;
                _captureThread = std::thread(
                    [this]
                    {
                        while (CaptureProcess())
                        {
                        }
                    });
            }
            _captureStarted = true;

            return 0;
        }

        int32_t VideoCaptureModuleX3::StopCapture()
        {
            {
                std::lock_guard<std::mutex> guard(capture_lock_);
                quit_ = true;
            }
            // Make sure the capture thread stops using the mutex.
            _captureThread.join();

            std::lock_guard<std::mutex> guard(capture_lock_);
            if (_captureStarted)
            {
                _captureStarted = false;

                SYS_VIN_VPS_MODE_E mode = VIN_OFFLINE_VPS_OFFINE;
                HB_ISP_GetSetExit();
                iot_vin_stop(0, mode);
                iot_vin_deinit(0, mode);
                HB_SYS_Exit();
                // if (X3VideoClosePLL(elem))
                // {
                //     printf("X3VideoClosePLL fail\n");
                //     return -1;
                // }
                info("x3Originvideo exit\n");
                return 0;
            }

            return 0;
        }

        bool VideoCaptureModuleX3::CaptureStarted()
        {
            return _captureStarted;
        }

        bool VideoCaptureModuleX3::CaptureProcess()
        {
            int32_t ret = 0;
            hb_vio_buffer_t out_buffer;

            VideoCaptureCapability frameInfo;
            frameInfo.width = _currentWidth;
            frameInfo.height = _currentHeight;
            frameInfo.videoType = _captureVideoType;

            const int32_t width = frameInfo.width;
            const int32_t height = frameInfo.height;

            int32_t stride_y = width;
            int32_t stride_u = (width + 1) / 2;
            int32_t stride_v = (width + 1) / 2;
            int32_t stride_uv = (width + 1) / 2;
            int32_t target_width = width;
            int32_t target_height = abs(height);
            static int32_t print_num = 0;
            char file_path[128] = {0};

            std::lock_guard<std::mutex> guard(capture_lock_);
            if (quit_)
            {
                info("quit is true,capture stop\n");
                return false;
            }

            if (_captureStarted)
            {
                ret = HB_VIN_GetChnFrame(0, 0, &out_buffer, 50); // 50ms
                if (ret != 0)
                {
                    error("final HB_VIN_GetChnFrame fail ret:%d\r\n",ret);
                    return true;
                }

                int size = out_buffer.img_addr.stride_size * out_buffer.img_addr.height;
                if (print_num++ % 75 == 0)
                    info("X3 Frame stride_size:%d width:%d height:%d\n", out_buffer.img_addr.stride_size, out_buffer.img_addr.width, out_buffer.img_addr.height);
#if 0
                    snprintf(file_path, sizeof(file_path), "/data/test_x3_%d_capture.yuv", print_num);
                    IUtilMemToFile2plane(file_path, out_buffer.img_addr.addr[0], out_buffer.img_addr.addr[1], size, size / 2);
#endif
                video_common::VideoFrameBuffer *buffer = new video_common::VideoFrameBuffer(target_width, target_height, stride_y, stride_uv);
                //info("buffer StrideY:%d StrideUV:%d\n", buffer->StrideY(), buffer->StrideUV());

                // 实际测试下来发现 x3 芯片拷贝原图 1920*960*3/2,大概2M数据,一次拷贝和分多次拷贝,耗时大概1-3毫秒,不需要优化
                // 保留整个拷贝和带stride的循环拷贝的实现部分,这里优先启用libyuv里面的memcpy函数

#if 0
                    if(buffer->StrideY() == target_width && buffer->StrideUV() == target_width)
                    {
                        memcpy(buffer->MutableDataY(), out_buffer.img_addr.addr[0], size);
                        memcpy(buffer->MutableDataUV(), out_buffer.img_addr.addr[1], size / 2);
                    }
                    else
                    {
                        /* Y分量拷贝 */
                        for (int row = 0; row < target_height; row++)
                        {
                            memcpy(buffer->MutableDataY() + row * buffer->StrideY(), out_buffer.img_addr.addr[0] + row * target_width, target_width);
                        }
                        /* UV分量拷贝 */
                        for (int row = 0; row < target_height / 2; row++)
                        {
                            memcpy(buffer->MutableDataUV() + row * buffer->StrideUV(), out_buffer.img_addr.addr[1] + row * target_width, target_width);
                        }
                    }
#endif
                libyuv::NV21Copy(out_buffer.img_addr.addr[0], out_buffer.img_addr.stride_size, out_buffer.img_addr.addr[1],
                                 out_buffer.img_addr.stride_size, buffer->MutableDataY(), buffer->StrideY(),
                                 buffer->MutableDataUV(), buffer->StrideUV(), target_width, target_height);

                IncomingFrame(buffer);

#if 0
                    info("buffer StrideY:%d StrideU:%d StrideV:%d\n",buffer->StrideY(),buffer->StrideU(),buffer->StrideV());
                    const int conversionResult = libyuv::NV12ToI420(
                        out_buffer.img_addr.addr[0], out_buffer.img_addr.stride_size, out_buffer.img_addr.addr[1], out_buffer.img_addr.stride_size,
                        buffer->MutableDataY(), buffer->StrideY(), buffer->MutableDataU(), buffer->StrideU(), buffer->MutableDataV(),
                        buffer->StrideV(), target_width, target_height);
                    if (conversionResult < 0)
                    {
                        error("Failed to convert capture frame from type %d to I420.\r\n", static_cast<int>(frameInfo.videoType));
                        delete buffer;
                        ret = HB_VIN_ReleaseChnFrame(0, 0, &out_buffer);
                        if (ret != 0)
                        {
                            error("HB_VIN_ReleaseChnFrame Failed. ret = %d\n", ret);
                        }
                        return -1;
                    }
                    else
                    {
                        if (0 == print_num++ % 1)
                            info("Successed to convert capture frame from type %d to I420.\r\n", static_cast<int>(frameInfo.videoType));
                    }
#endif
                ret = HB_VIN_ReleaseChnFrame(0, 0, &out_buffer);
                if (ret != 0)
                {
                    error("HB_VIN_ReleaseChnFrame Failed. ret = %d\n", ret);
                }
#if 0
                    if(60 == print_num++)
                    {
                        delete buffer;
                        return false;//for test;
                    }

                    snprintf(file_path, sizeof(file_path), "/data/cover_%d_capture.yuv", print_num);
                    IUtilImageToFile(buffer->MutableDataY(), buffer->MutableDataUV(), NULL, target_width, target_height,
                                     buffer->StrideY(), buffer->StrideUV(), 0, 1, file_path);
#endif
            }

            IntelliUtilSleep(5 * 1000);

            return true;
        }

        int32_t VideoCaptureModuleX3::CaptureSettings(VideoCaptureCapability &settings)
        {
            settings.width = _currentWidth;
            settings.height = _currentHeight;
            settings.maxFPS = _currentFrameRate;
            settings.videoType = _captureVideoType;
            return 0;
        }

        int32_t VideoCaptureModuleX3::X3VideoGetCamParamPtr(int32_t hdr, char *camera_type)
        {
            if (!camera_type)
                return -1;
            int32_t i;
            char *pPtr = camera_type;
            if (hdr)
            {
                for (i = 0; i < sizeof(_x3cameraVinVec_Hdr) / sizeof(_x3cameraVinVec_Hdr[0]); i++)
                {
                    if (_x3cameraVinVec_Hdr[i].camera_type && !strcmp(_x3cameraVinVec_Hdr[i].camera_type, pPtr))
                    {

                        _x3VinAttr = &_x3cameraVinVec_Hdr[i];
                        info("i=%d, current cameratype=%s\n", i, _x3VinAttr->camera_type);
                        return 0;
                    }
                }
                if (i >= sizeof(_x3cameraVinVec_Hdr) / sizeof(_x3cameraVinVec_Hdr[0]))
                {
                    error("can not find this camera_type=%s\n", pPtr);
                    return -1;
                }
            }
            else
            {
                for (i = 0; i < sizeof(_x3cameraVinVec) / sizeof(_x3cameraVinVec[0]); i++)
                {
                    if (_x3cameraVinVec[i].camera_type && !strcmp(_x3cameraVinVec[i].camera_type, pPtr))
                    {

                        _x3VinAttr = &_x3cameraVinVec[i];
                        info("i=%d, current cameratype=%s\n", i, _x3VinAttr->camera_type);
                        return 0;
                    }
                }
                if (i >= sizeof(_x3cameraVinVec) / sizeof(_x3cameraVinVec[0]))
                {
                    error("can not find this camera_type=%s\n", pPtr);
                    return -1;
                }
            }
            return -1;
        }

        int32_t VideoCaptureModuleX3::iot_sensor_stop(int devId)
        {
            int ret = 0;

            ret = HB_MIPI_UnresetSensor(devId);
            info("HB_MIPI_UnresetSensor\n");
            if (ret < 0)
            {
                error("HB_MIPI_UnresetSensor error!\n");
                return ret;
            }
            return ret;
        }

        int32_t VideoCaptureModuleX3::iot_mipi_stop(int mipiIdx)
        {
            int ret = 0;
            info("hb_mipi_stop======\n");
            ret = HB_MIPI_UnresetMipi(mipiIdx);
            info("HB_MIPI_UnresetMipi\n");
            if (ret < 0)
            {
                error("HB_MIPI_UnresetMipi error!\n");
                return ret;
            }
            return ret;
        }

        void VideoCaptureModuleX3::iot_vin_stop(int pipeId, SYS_VIN_VPS_MODE_E mode)
        {
            int mipi_host_id = 0;
            mipi_host_id = x3_vin_info.mipi_sensor_info.sensorInfo.entry_index;
            if (mode != VIN_FEEDBACK_ISP_ONLINE_VPS_ONLINE)
            {
                iot_sensor_stop(x3_vin_info.dev_id);
                iot_mipi_stop(mipi_host_id);
            }
            HB_VIN_DisableDev(pipeId); // thread stop && sif stop

            HB_VIN_StopPipe(pipeId); // isp stop

            HB_VIN_DisableChn(pipeId, 1); // dwe stop
            info("HB_VIN_DisableChn\n");
            return;
        }

        int32_t VideoCaptureModuleX3::iot_sensor_deinit(int devId)
        {
            int ret = 0;

            ret = HB_MIPI_DeinitSensor(devId);
            printf("HB_MIPI_DeinitSensor\n");
            if (ret < 0)
            {
                error("HB_MIPI_DeinitSensor error!\n");
                return ret;
            }
            return ret;
        }

        int32_t VideoCaptureModuleX3::iot_mipi_deinit(int mipiIdx)
        {
            int ret = 0;

            info("hb_sensor_deinit end==mipiIdx %d====\n", mipiIdx);
            ret = HB_MIPI_Clear(mipiIdx);
            printf("HB_MIPI_Clear\n");
            if (ret < 0)
            {
                error("HB_MIPI_Clear error!\n");
                return ret;
            }
            info("hb_mipi_deinit end======\n");
            return ret;
        }

        int32_t VideoCaptureModuleX3::iot_sensor_init(int dev_id)
        {
            int ret = 0;
            ret = HB_MIPI_InitSensor(dev_id, &x3_vin_info.mipi_sensor_info);
            info("HB_MIPI_InitSensor\n");
            if (ret < 0)
            {
                error("HB_MIPI_InitSensor error!\n");
                return ret;
            }
            info("HB_MIPI_InitSensor Success\n");
            return ret;
        }

        int32_t VideoCaptureModuleX3::iot_mipi_init(int mipi_host_id)
        {
            int ret = 0;
            ret = HB_MIPI_SetMipiAttr(mipi_host_id, &x3_vin_info.mipi_attr);
            info("HB_MIPI_SetMipiAttr\n");
            if (ret < 0)
            {
                error("HB_MIPI_SetDevAttr error!\n");
                return ret;
            }
            info("HB_MIPI_SetDevAttr Success\n");
            return ret;
        }

        void VideoCaptureModuleX3::iot_vin_deinit(int pipeId, SYS_VIN_VPS_MODE_E mode)
        {
            int mipi_host_id = 0;
            mipi_host_id = x3_vin_info.mipi_sensor_info.sensorInfo.entry_index;
            if (mode != VIN_FEEDBACK_ISP_ONLINE_VPS_ONLINE)
            {
                iot_sensor_deinit(x3_vin_info.dev_id);
                iot_mipi_deinit(mipi_host_id);
            }
            HB_VIN_DestroyDev(pipeId);    // sif deinit && destroy
            HB_VIN_DestroyChn(pipeId, 1); // dwe deinit
            HB_VIN_DestroyPipe(pipeId);   // isp deinit && destroy
            info("HB_VIN_DestroyPipe\n");
        }

        int32_t VideoCaptureModuleX3::iot_vin_init(uint32_t pipe_id, SYS_VIN_VPS_MODE_E mode)
        {
            int ret = 0;

            int mipi_host_id = 0;
            MIPI_SENSOR_MODE_E work_mode;

            // copy camera parameters
            info("iot_vin_init:%p,%p,%p,%p\n", _x3VinAttr->vin_pipe, _x3VinAttr->vin_dev, _x3VinAttr->mipi, _x3VinAttr->mipi_sensor);
            info("iot_vin_init:%p,%p,%p,%p\n", &PIPE_ATTR_H65_LINEAR_BASE, &DEV_ATTR_H65_LINEAR_BASE, &MIPI_1LANE_SENSOR_H65_30FPS_10BIT_NORMAL_ATTR, &SENSOR_1LANE_H65_30FPS_10BIT_LINEAR_INFO);
            memset(&x3_vin_info, 0, sizeof(X3_VIN_INFO));
            x3_vin_info.dev_id = 0; // The dev_id should start from 0
#ifdef DOL2
            memcpy(&x3_vin_info.vin_pipe_attr, &PIPE_ATTR_F37_DOL2_BASE,
                   sizeof(VIN_PIPE_ATTR_S));
#else
            memcpy(&x3_vin_info.vin_pipe_attr, _x3VinAttr->vin_pipe,
                   sizeof(VIN_PIPE_ATTR_S));
#endif
            if (mode == VIN_FEEDBACK_ISP_ONLINE_VPS_ONLINE)
            {
                memcpy(&x3_vin_info.vin_dev_attr, &DEV_ATTR_F37_LINEAR_FEEDBACK,
                       sizeof(VIN_DEV_ATTR_S));
            }
            else
            {
#ifdef DOL2
                memcpy(&x3_vin_info.vin_dev_attr, &DEV_ATTR_F37_DOL2_BASE,
                       sizeof(VIN_DEV_ATTR_S));
#else
                memcpy(&x3_vin_info.vin_dev_attr, _x3VinAttr->vin_dev,
                       sizeof(VIN_DEV_ATTR_S));
#endif
            }
            memcpy(&x3_vin_info.vin_dis_attr, &DIS_ATTR_BASE, sizeof(VIN_DIS_ATTR_S));
            memcpy(&x3_vin_info.vin_ldc_attr, &LDC_ATTR_BASE, sizeof(VIN_LDC_ATTR_S));
#ifdef DOL2
            memcpy(&x3_vin_info.mipi_attr,
                   &MIPI_1LANE_SENSOR_F37_30FPS_10BIT_DOL2_ATTR,
                   sizeof(MIPI_ATTR_S));
            memcpy(&x3_vin_info.mipi_sensor_info,
                   &SENSOR_1LANE_F37_30FPS_10BIT_DOL2_INFO,
                   sizeof(MIPI_SENSOR_INFO_S));
#else
            memcpy(&x3_vin_info.mipi_attr,
                   _x3VinAttr->mipi,
                   sizeof(MIPI_ATTR_S));
            memcpy(&x3_vin_info.mipi_sensor_info,
                   _x3VinAttr->mipi_sensor,
                   sizeof(MIPI_SENSOR_INFO_S));
#endif

            mipi_host_id = x3_vin_info.mipi_sensor_info.sensorInfo.entry_index;
            ret = HB_VIN_CreatePipe(pipe_id, &x3_vin_info.vin_pipe_attr); // isp init
            info("HB_VIN_CreatePipe\n");
            if (ret < 0)
            {
                error("HB_MIPI_InitSensor error!\n");
                return ret;
            }
            ret = HB_VIN_SetMipiBindDev(pipe_id, mipi_host_id);
            info("HB_VIN_SetMipiBindDev\n");
            if (ret < 0)
            {
                error("HB_VIN_SetMipiBindDev error!\n");
                return ret;
            }
            // 0 -- mipi virtual channel 0. Bind dev to virtual channel 0.
            // In linear mode, only uses virtual channel 0.
            // In DOL2 mode, virtual channel 1 should add to bind to the device too.
            // In DOL3 mode, virtual channel 2 should add to bind to the device too.
            work_mode = x3_vin_info.mipi_sensor_info.sensorInfo.sensor_mode;
            ret = HB_VIN_SetDevVCNumber(pipe_id, 0);
            info("HB_VIN_SetDevVCNumber\n");
            if (ret < 0)
            {
                error("HB_VIN_SetDevVCNumber error!\n");
                return ret;
            }
            if (work_mode >= DOL2_M)
            {
                ret = HB_VIN_AddDevVCNumber(pipe_id, 1);
                info("HB_VIN_AddDevVCNumber\n");
                if (ret < 0)
                {
                    error("HB_VIN_AddDevVCNumber error!\n");
                    return ret;
                }
            }
            if (work_mode >= DOL3_M)
            {
                ret = HB_VIN_AddDevVCNumber(pipe_id, 2);
                info("HB_VIN_AddDevVCNumber\n");
                if (ret < 0)
                {
                    error("HB_VIN_AddDevVCNumber error!\n");
                    return ret;
                }
            }

            ret = HB_VIN_SetDevAttr(pipe_id, &x3_vin_info.vin_dev_attr); // sif init
            info("HB_VIN_SetDevAttr\n");
            if (ret < 0)
            {
                error("HB_MIPI_InitSensor error!\n");
                return ret;
            }
            ret = HB_VIN_SetPipeAttr(pipe_id, &x3_vin_info.vin_pipe_attr); // isp init
            info("HB_VIN_SetPipeAttr\n");
            if (ret < 0)
            {
                error("HB_VIN_SetPipeAttr error!\n");
                return ret;
            }

            ret = HB_VIN_SetChnDISAttr(pipe_id, 1, &x3_vin_info.vin_dis_attr); //  dis init
            info("HB_VIN_SetChnDISAttr\n");
            if (ret < 0)
            {
                error("HB_VIN_SetChnDISAttr error!\n");
                return ret;
            }

            ret = HB_VIN_SetChnLDCAttr(pipe_id, 1, &x3_vin_info.vin_ldc_attr); //  ldc init
            info("HB_VIN_SetChnLDCAttr\n");
            if (ret < 0)
            {
                error("HB_VIN_SetChnLDCAttr error!\n");
                return ret;
            }
            ret = HB_VIN_SetChnAttr(pipe_id, 1); //  dwe init
            info("HB_VIN_SetChnAttr\n");
            if (ret < 0)
            {
                error("HB_VIN_SetChnAttr error!\n");
                return ret;
            }
            HB_VIN_SetDevBindPipe(pipe_id, pipe_id); //  bind init
            info("HB_VIN_SetDevBindPipe\n");

            if (mode != VIN_FEEDBACK_ISP_ONLINE_VPS_ONLINE)
            {
                ret = iot_sensor_init(pipe_id);
                if (ret)
                    return ret;
                ret = iot_mipi_init(mipi_host_id);
                if (ret)
                    return ret;
            }

            return ret;
        }

        int32_t VideoCaptureModuleX3::OriginVideoInit()
        {
            SYS_VIN_VPS_MODE_E mode = VIN_OFFLINE_VPS_OFFINE;
            int32_t ret = 0;

            HB_SYS_Init();

            ret = HB_SYS_SetVINVPSMode(0, mode);
            if (ret < 0)
            {
                HB_SYS_Exit();
                error("%d HB_SYS_SetVINVPSMode %d error! ret:%d\n", mode, ret);
                return -1;
            }

            ret = iot_vin_init(0, mode);
            if (ret < 0)
            {
                HB_SYS_Exit();
                error("iot_vin_init ret: %d error!\n", ret);
                return ret;
            }

            // iot_vin_start
            ret = HB_VIN_EnableChn(0, 0); // dwe start

            if (ret < 0)
            {
                iot_vin_deinit(0, mode);

                HB_SYS_Exit();
                error("HB_VIN_EnableChn error! ret:%d\n", ret);
                return ret;
            }

            // isp start
            ret = HB_VIN_StartPipe(0);
            if (ret < 0)
            {
                // dwe stop
                HB_VIN_DisableChn(0, 1);
                iot_vin_deinit(0, mode);
                HB_SYS_Exit();
                error("HB_VIN_StartPipe error! ret:%d\n", ret);
                return ret;
            }

            // sif start
            ret = HB_VIN_EnableDev(0);
            if (ret < 0)
            {
                HB_VIN_StopPipe(0);
                HB_VIN_DisableChn(0, 1);
                iot_vin_deinit(0, mode);
                HB_SYS_Exit();
                error("HB_VIN_EnableDev error! ret:%d\n", ret);
                return ret;
            }

            int32_t mipi_host_id = 0;
            if (mode != VIN_FEEDBACK_ISP_ONLINE_VPS_ONLINE) // data from DDR instead of mipi
            {
                mipi_host_id = x3_vin_info.mipi_sensor_info.sensorInfo.entry_index;

                ret = HB_MIPI_ResetSensor(x3_vin_info.dev_id);
                if (ret < 0)
                {

                    HB_VIN_DisableDev(0);

                    HB_VIN_StopPipe(0);

                    HB_VIN_DisableChn(0, 1);
                    iot_vin_deinit(0, mode);

                    HB_SYS_Exit();
                    error("HB_MIPI_ResetSensor error!\n");
                    return ret;
                }

                info("Start HB_MIPI_ResetMipi\n");
                ret = HB_MIPI_ResetMipi(mipi_host_id);
                if (ret < 0)
                {
                    HB_MIPI_UnresetSensor(x3_vin_info.dev_id);
                    HB_VIN_DisableDev(0);
                    HB_VIN_StopPipe(0);
                    HB_VIN_DisableChn(0, 1);
                    iot_vin_deinit(0, mode);
                    HB_SYS_Exit();
                    error("HB_MIPI_ResetMipi error! ret:%d\n", ret);
                    return ret;
                }
            }
            ret = HB_ISP_GetSetInit();
            if (ret < 0)
            {
                error("HB_ISP_GetSetInit error! ret:%d\n", ret);
                return ret;
            }
            info("X3 Origin Video Init Success\n");
            return 0;
        }

    } // namespace videocapturemodule

} // namespace ecovacs
