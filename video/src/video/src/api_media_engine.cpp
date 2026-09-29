#include <memory>
#include <cstring>
#include <string>
#include <mutex>
#include <map>
#include "osp.h"
#include "bus.h"
#include "log.h"
#include "cjson/cJSON.h"
#include "api_media_engine.h"
#ifdef ENABLE_ISP
#include "api_rk_isp_manager.h"
#endif
#if defined(AW_PLATFORM)
#include "api_allwinner_isp.h"
#endif

namespace ecovacs
{
    static MediaEngine *g_media_engine_instance = NULL;
    static std::mutex g_media_engine_mutex;

    // ========== Element Create 方法 ==========
    std::unique_ptr<VideoCaptureElement> VideoCaptureElement::Create()
    {
        return std::make_unique<VideoCaptureElement>();
    }

    std::unique_ptr<VideoH264EncodeElement> VideoH264EncodeElement::Create()
    {
        return std::make_unique<VideoH264EncodeElement>();
    }

    std::unique_ptr<VideoPushElement> VideoPushElement::Create()
    {
        return std::make_unique<VideoPushElement>();
    }
     
    std::unique_ptr<VideoRtspPushElement> VideoRtspPushElement::Create()
    {
        return std::make_unique<VideoRtspPushElement>();
    }

#if defined(ENABLE_LIBPEER)
    std::unique_ptr<VideoLibpeerPushElement> VideoLibpeerPushElement::Create()
    {
        return std::make_unique<VideoLibpeerPushElement>();
    }
#endif

    std::unique_ptr<VideoAiProcessElement> VideoAiProcessElement::Create()
    {
        return std::make_unique<VideoAiProcessElement>();
    }

    std::unique_ptr<VideoPhotographElement> VideoPhotographElement::Create()
    {
        return std::make_unique<VideoPhotographElement>();
    }

    std::unique_ptr<JpegPicUploadElement> JpegPicUploadElement::Create()
    {
        return std::make_unique<JpegPicUploadElement>();
    }

    std::unique_ptr<RawVideoSaveElement> RawVideoSaveElement::Create()
    {
        return std::make_unique<RawVideoSaveElement>();
    }

    std::unique_ptr<JpegPicPublishElement> JpegPicPublishElement::Create()
    {
        return std::make_unique<JpegPicPublishElement>();
    }

    MediaEngine *MediaEngine::MediaEngineInstance()
    {
        std::lock_guard<std::mutex> guard(g_media_engine_mutex);
        if (NULL == g_media_engine_instance)
            g_media_engine_instance = new MediaEngine();
        return g_media_engine_instance;
    }

    VideoCaptureElement *MediaEngine::FindCaptureElem(std::string src_name)
    {
        for (auto &source : video_capture_sources_)
        {
            if (std::string::npos != source->device_capture_capability_.stream_property.find(src_name))
            {
                return source.get();
            }
        }
        return nullptr;
    }

    int32_t MediaEngine::ReadConfig(const char *cfgFile, ConfigModule::MediaEngineConfig *config)
    {
        char *ps8String = NULL;

        if (0 != IUtilCheckFileExist(cfgFile))
        {
            ump_error("capture_device_cfg[%s] is not exist\r\n", cfgFile);
            return -1;
        }
        else
        {
            uint32_t u32Size = 0;
            if (0 != IUtilFileToMem(cfgFile, &u32Size, (uint8_t **)&ps8String))
            {
                ump_error("IUtilFileToMem[%s] failed\r\n", cfgFile);
                return -1;
            }
        }

        cJSON *ptObject = cJSON_Parse(ps8String);
        if (NULL == ptObject)
        {
            ump_error("cJSON_Parse ps8String[%s] failed\r\n");
            if (NULL != ps8String)
            {
                IntelliUtilFree(ps8String);
                ps8String = NULL;
            }
            return -1;
        }

        ump_info("ps8String[%s]\r\n", ps8String);
        cJSON *ptUnixPath = cJSON_GetObjectItem(ptObject, "unix_sock_path");
        int path_array_len = sizeof(config->unix_socket_path);
        if (ptUnixPath)
            strncpy(config->unix_socket_path, ptUnixPath->valuestring,
                    path_array_len - 1);
        else
            strncpy(config->unix_socket_path, "/tmp/ubus.sock", path_array_len - 1);
        config->unix_socket_path[path_array_len - 1] = '\0';

        cJSON *ptArrayItem = cJSON_GetObjectItem(ptObject, "captrue_cfg");
        config->capture_cfg_.device_number_ = cJSON_GetArraySize(ptArrayItem);
        ump_info("device_number_[%d]\r\n", config->capture_cfg_.device_number_);
        for (uint32_t idx = 0; idx < config->capture_cfg_.device_number_; idx++)
        {
            cJSON *ptJsonTemp = cJSON_GetArrayItem(ptArrayItem, idx);
            ConfigModule::CaptureCapability capture_capability;
            capture_capability.width = cJSON_GetObjectItem(ptJsonTemp, "width")->valueint;
            capture_capability.height = cJSON_GetObjectItem(ptJsonTemp, "height")->valueint;
            capture_capability.fps = cJSON_GetObjectItem(ptJsonTemp, "fps")->valueint;
            capture_capability.device_index = cJSON_GetObjectItem(ptJsonTemp, "device_index")->valueint;
            cJSON *ptCsiId = cJSON_GetObjectItem(ptJsonTemp, "csi_id");
            if (ptCsiId)
                capture_capability.csi_id = ptCsiId->valueint;
            else
                capture_capability.csi_id = -1;  // 默认自动
            capture_capability.camera_type = cJSON_GetObjectItem(ptJsonTemp, "camera_type")->valuestring;
            capture_capability.stream_property = cJSON_GetObjectItem(ptJsonTemp, "stream_property")->valuestring;

            // stream_type: "fr"（默认）或 "ds"
            cJSON *ptStreamType = cJSON_GetObjectItem(ptJsonTemp, "stream_type");
            if (ptStreamType && ptStreamType->valuestring)
                capture_capability.stream_type = ptStreamType->valuestring;
            else
                capture_capability.stream_type = "fr";
            ump_info("ReadConfig: capture[%u] stream_type=%s\n", idx, capture_capability.stream_type.c_str());

            // image_type: "YUYV"（默认）/"RGB"/"NV12"
            cJSON *ptImageType = cJSON_GetObjectItem(ptJsonTemp, "image_type");
            if (ptImageType && ptImageType->valuestring)
                capture_capability.image_type = ptImageType->valuestring;
            else
                capture_capability.image_type = "YUYV";
            ump_info("ReadConfig: capture[%u] image_type=%s\n", idx, capture_capability.image_type.c_str());

#ifdef ENABLE_ISP
            cJSON *ptCamId = cJSON_GetObjectItem(ptJsonTemp, "cam_id");
            if (ptCamId)
                capture_capability.cam_id = ptCamId->valueint;
            else
                capture_capability.cam_id = capture_capability.device_index;

            cJSON *ptIspPath = cJSON_GetObjectItem(ptJsonTemp, "isp_path");
            if (ptIspPath)
                capture_capability.isp_path = ptIspPath->valuestring;
            else
                capture_capability.isp_path = "mainpath";

            cJSON *ptIqFile = cJSON_GetObjectItem(ptJsonTemp, "iq_file");
            if (ptIqFile)
                capture_capability.iq_file = ptIqFile->valuestring;
            else
                capture_capability.iq_file = "/etc/iqfiles/" + capture_capability.camera_type + ".json";
#endif

            config->capture_cfg_.capture_capabilitys_.push_back(capture_capability);
        }

        cJSON *pEncoderItem = cJSON_GetObjectItem(ptObject, "encoder_cfg");
        if (pEncoderItem)
        {
            config->encoder_cfg_.width = cJSON_GetObjectItem(pEncoderItem, "width")->valueint;
            config->encoder_cfg_.height = cJSON_GetObjectItem(pEncoderItem, "height")->valueint;
            config->encoder_cfg_.init_qp = cJSON_GetObjectItem(pEncoderItem, "init_qp")->valueint;
            config->encoder_cfg_.min_qp = cJSON_GetObjectItem(pEncoderItem, "min_qp")->valueint;
            config->encoder_cfg_.max_qp = cJSON_GetObjectItem(pEncoderItem, "max_qp")->valueint;
            config->encoder_cfg_.init_bps = cJSON_GetObjectItem(pEncoderItem, "init_bps")->valueint;
            config->encoder_cfg_.min_bps = cJSON_GetObjectItem(pEncoderItem, "min_bps")->valueint;
            config->encoder_cfg_.max_bps = cJSON_GetObjectItem(pEncoderItem, "max_bps")->valueint;
            config->encoder_cfg_.gop = cJSON_GetObjectItem(pEncoderItem, "gop")->valueint;
            config->encoder_cfg_.max_fps = cJSON_GetObjectItem(pEncoderItem, "max_fps")->valueint;

            cJSON *ptCropX = cJSON_GetObjectItem(pEncoderItem, "crop_x");
            if (ptCropX)
                config->encoder_cfg_.crop_x = ptCropX->valueint;
            cJSON *ptCropY = cJSON_GetObjectItem(pEncoderItem, "crop_y");
            if (ptCropY)
                config->encoder_cfg_.crop_y = ptCropY->valueint;
        }

        // ----- pusher_cfg -----
        cJSON *pPusherItem = cJSON_GetObjectItem(ptObject, "pusher_cfg");
        if (pPusherItem)
        {
            cJSON *ptNoiseLvl = cJSON_GetObjectItem(pPusherItem, "audio_noise_level");
            if (ptNoiseLvl)
                config->pusher_cfg_.audio_noise_level = ptNoiseLvl->valueint;
            else
                config->pusher_cfg_.audio_noise_level = 0;

            // AEC 相关配置（可选，无则用默认值）
            cJSON *ptAecEnabled = cJSON_GetObjectItem(pPusherItem, "aec_enabled");
            config->pusher_cfg_.aec_enabled = ptAecEnabled ? ptAecEnabled->valueint : 0;

            cJSON *ptAudioDevice = cJSON_GetObjectItem(pPusherItem, "audio_device");
            if (ptAudioDevice && ptAudioDevice->valuestring)
                config->pusher_cfg_.audio_device = ptAudioDevice->valuestring;
            else
                config->pusher_cfg_.audio_device = "default";

            cJSON *ptCaptureCh = cJSON_GetObjectItem(pPusherItem, "audio_capture_channels");
            config->pusher_cfg_.audio_capture_channels =
                ptCaptureCh ? ptCaptureCh->valueint : 8;

            cJSON *ptMicCh = cJSON_GetObjectItem(pPusherItem, "audio_mic_channel");
            config->pusher_cfg_.audio_mic_channel = ptMicCh ? ptMicCh->valueint : 0;

            cJSON *ptRefCh = cJSON_GetObjectItem(pPusherItem, "audio_ref_channel");
            config->pusher_cfg_.audio_ref_channel = ptRefCh ? ptRefCh->valueint : 5;
        }
        else
        {
            config->pusher_cfg_.audio_noise_level = 0;
            config->pusher_cfg_.aec_enabled = 0;
        }
        ump_info("pusher_cfg: audio_noise_level=[%d]\r\n",
                 config->pusher_cfg_.audio_noise_level);

        if (NULL != ps8String)
        {
            IntelliUtilFree(ps8String);
            ps8String = NULL;
        }
        if (NULL != ptObject)
        {
            cJSON_Delete(ptObject);
            ptObject = NULL;
        }

        return 0;
    }

    int32_t MediaEngine::Init(const char *cfgFile)
    {
        std::lock_guard<std::mutex> guard(api_lock_);

        if (0 != ReadConfig(cfgFile, &media_engine_cfg_))
        {
            ump_error("ReadConfig failed\r\n");
            return -1;
        }

#ifdef ENABLE_ISP
        std::map<int32_t, std::pair<int32_t, int32_t>> cam_paths;
        std::map<int32_t, std::string> cam_iq_files;
        std::map<int32_t, std::string> cam_types;
        for (auto &cap : media_engine_cfg_.capture_cfg_.capture_capabilitys_)
        {
            int32_t cam_id = cap.cam_id;
            cam_iq_files[cam_id] = cap.iq_file;
            cam_types[cam_id] = cap.camera_type;
            if (cap.isp_path == "mainpath")
                cam_paths[cam_id].first = cap.device_index;
            else if (cap.isp_path == "selfpath")
                cam_paths[cam_id].second = cap.device_index;
        }
        IspManager &isp_mgr = IspManager::GetInstance();
        for (auto &pair : cam_paths)
        {
            isp_mgr.InitHardware(pair.first, pair.second.first, pair.second.second,
                                 cam_types[pair.first], cam_iq_files[pair.first]);
        }
#endif

        for (int32_t idx = 0; idx < media_engine_cfg_.capture_cfg_.device_number_; idx++)
        {
            auto video_capture_source = VideoCaptureElement::Create();
            if (!video_capture_source)
            {
                ump_error("video_capture_source create failed\r\n");
                return -1;
            }

            video_capture_source->device_capture_capability_ = media_engine_cfg_.capture_cfg_.capture_capabilitys_[idx];
#ifdef ENABLE_ISP
            video_capture_source->cam_id_ = video_capture_source->device_capture_capability_.cam_id;
            video_capture_source->isp_path_ = video_capture_source->device_capture_capability_.isp_path;
#endif

            int32_t width = video_capture_source->device_capture_capability_.width;
            int32_t height = video_capture_source->device_capture_capability_.height;
            int32_t target_fps = video_capture_source->device_capture_capability_.fps;
            int32_t capture_device_index = video_capture_source->device_capture_capability_.device_index;
            int32_t csi_id = video_capture_source->device_capture_capability_.csi_id;
            std::string stream_type = video_capture_source->device_capture_capability_.stream_type;
            std::string image_type = video_capture_source->device_capture_capability_.image_type;
            ump_info("OpenCapture: stream_property=%s stream_type=%s image_type=%s csi_id=%d\n",
                     video_capture_source->device_capture_capability_.stream_property.c_str(),
                     stream_type.c_str(), image_type.c_str(), csi_id);

            auto capture = VideoCapture::Create(width, height, target_fps, capture_device_index,
                                                 csi_id, stream_type, image_type);
            if (!capture)
            {
                ump_error("VideoCapture::Create failed\r\n");
                return -1;
            }
            video_capture_source->video_capture_ = std::shared_ptr<VideoCapture>(capture.release());

            video_capture_source->video_capture_broadcaster_ = VideoCaptureBroadcaster::Create();
            if (!video_capture_source->video_capture_broadcaster_)
            {
                ump_error("video_capture_broadcaster_ create failed\r\n");
                return -1;
            }

            video_capture_source->video_capture_->RegisterCaptureDataCallback(
                video_capture_source->video_capture_broadcaster_.get());

            video_capture_sources_.push_back(std::move(video_capture_source));
        }

        quit_.store(false);
        event_thread_ = std::thread([this]
                                    { while (EventProcess()); });

        // 初始化所有元件为 nullptr
        video_photograph_element_ = nullptr;
        jpeg_pic_upload_element_ = nullptr;
        video_jpeg_publish_element_ = nullptr;
        jpeg_pic_publish_element_ = nullptr;
        video_encode_element_ = nullptr;
        video_push_element_ = nullptr;
        video_ai_process_element_ = nullptr;
        video_raw_video_save_element_ = nullptr;

        bus_init();

        return 0;
    }

    std::string MediaEngine::GetBusEventMsg()
    {
        char *message = NULL;
        int lenght = 0;
        bus_get_message(&message, &lenght);
        if (NULL != message)
        {
            ump_info("bus_get_message %s\n", message);
            std::string tmp_msg = message;
            IntelliUtilFree(message);
            return tmp_msg;
        }
        return "";
    }

    bool MediaEngine::EventProcess()
    {
        if (quit_.load())
        {
            ump_info("EventThread is quit\n");
            return false;
        }

        std::string msg = GetBusEventMsg();
        if (!msg.empty())
        {
            cJSON *root = cJSON_Parse(msg.c_str());
            if (NULL == root)
            {
                ump_error("msg is bad json format\n");
                return true;
            }

            const char *tmpCStr = cJSON_GetObjectItem(root, "cmd")->valuestring;
            if (NULL == tmpCStr)
            {
                ump_error("get cmd error\n");
                cJSON_Delete(root);
                return true;
            }

            if (0 == strcmp(tmpCStr, "picture_loss"))
            {
                if (video_encode_element_ && video_encode_element_->video_h264_encode_)
                {
                    if (video_encode_element_->video_h264_encode_->EncodeStarted())
                        video_encode_element_->video_h264_encode_->RequestKeyFrame();
                }
            }
            else if (0 == strcmp(tmpCStr, "stop_grab_pic"))
            {
                StopTakePhotograph();
                grap_status_ = GrapStatus::FINISH;
            }
            else if (0 == strcmp(tmpCStr, "stop_save_raw_video"))
            {
                StopSaveRawVideo();
            }
            else if (0 == strcmp(tmpCStr, "capture_select_timeout"))
            {
                if (video_push_element_ && video_push_element_->video_push_)
                    if (video_push_element_->video_push_->PushStarted())
                    {
                        StopPush();
                        StartPush();
                    }
                if (video_ai_process_element_ && video_ai_process_element_->video_ai_)
                    if (video_ai_process_element_->video_ai_->AIStarted())
                    {
                        StopAI();
                        StartAI();
                    }
            }

            cJSON_Delete(root);
        }

        IntelliUtilSleepMs(50);
        return true;
    }

    // ========== 推流相关 ==========
    int32_t MediaEngine::StartPush()
    {
        std::lock_guard<std::mutex> guard(api_lock_);

        VideoCaptureElement *capture_element = FindCaptureElem("living");
        if (!capture_element)
            return -1;

#ifdef ENABLE_ISP
        IspManager::GetInstance().AddStream(capture_element->cam_id_);
#endif

        ecovacs::video_common::VideoCodecConfig video_codec_config;
        // 编码器 frameType 需与 IncomingFrame 输出的 buffer 类型一致：
        //   YUYV → 直通 kYUY2
        //   NV12 → 直通 kNV12
        //   RGB/I420/UYVY → IncomingFrame 转成 kNV12
#if defined(AM311X_PLATFORM)
        const std::string &img_type = capture_element->device_capture_capability_.image_type;
        if (img_type == "YUYV" || img_type == "YUY2") {
            video_codec_config.frameType = video_common::FrameType::kYUY2;
        } else {
            video_codec_config.frameType = video_common::FrameType::kNV12;
        }
#else
        video_codec_config.frameType = video_common::FrameType::kNV12;
#endif
        video_codec_config.codecType = video_common::VideoCodecType::kVideoCodecH264;
        video_codec_config.complexity_ = video_common::VideoCodecComplexity::kComplexityNormal;
        video_codec_config.width = media_engine_cfg_.encoder_cfg_.width;
        video_codec_config.height = media_engine_cfg_.encoder_cfg_.height;
        video_codec_config.crop_x = media_engine_cfg_.encoder_cfg_.crop_x;
        video_codec_config.crop_y = media_engine_cfg_.encoder_cfg_.crop_y;
        video_codec_config.maxFramerate = media_engine_cfg_.encoder_cfg_.max_fps;
        video_codec_config.maxBitrate = media_engine_cfg_.encoder_cfg_.max_bps;
        video_codec_config.minBitrate = media_engine_cfg_.encoder_cfg_.min_bps;
        video_codec_config.initBitrate = media_engine_cfg_.encoder_cfg_.init_bps;
        video_codec_config.codec_specific_.H264.init_qp = media_engine_cfg_.encoder_cfg_.init_qp;
        video_codec_config.codec_specific_.H264.min_qp = media_engine_cfg_.encoder_cfg_.min_qp;
        video_codec_config.codec_specific_.H264.max_qp = media_engine_cfg_.encoder_cfg_.max_qp;
        video_codec_config.codec_specific_.H264.gop = media_engine_cfg_.encoder_cfg_.gop;
        video_codec_config.codec_specific_.H264.profile = ecovacs::video_common::VideoCodecProfile::kProfileBase;

        if (!video_encode_element_)
        {
            video_encode_element_ = VideoH264EncodeElement::Create();
            if (!video_encode_element_)
                return -1;

            auto encode = VideoEncode::Create(&video_codec_config);
            if (!encode)
                return -1;
            video_encode_element_->video_h264_encode_ = std::shared_ptr<VideoEncode>(encode.release());

            video_encode_element_->video_encode_broadcaster_ = VideoEncodeBroadcaster::Create();
            if (!video_encode_element_->video_encode_broadcaster_)
                return -1;

            video_encode_element_->video_h264_encode_->RegisterEncodeCompleteCallback(
                video_encode_element_->video_encode_broadcaster_.get());
        }

        if (!video_push_element_)
        {
            video_push_element_ = VideoPushElement::Create();
            if (!video_push_element_)
                return -1;

            auto push = VideoPush::Create();
            if (!push)
                return -1;
            video_push_element_->video_push_ = std::shared_ptr<VideoPush>(push.release());
        }

        capture_element->video_capture_broadcaster_->AddOrUpdateSink(video_encode_element_->video_h264_encode_);
        video_encode_element_->video_encode_broadcaster_->AddOrUpdateSink(video_push_element_->video_push_);

        if (!capture_element->video_capture_->CaptureStarted())
        {
            capture_element->video_capture_->StartCapture();
        }

       if (capture_element->video_capture_->CaptureStarted())
        {
#if defined(AW_PLATFORM)
            AllWinnerIsp::GetInstance().Start(
                capture_element->device_capture_capability_.device_index);
#endif
        }

        if (!video_encode_element_->video_h264_encode_->EncodeStarted())
        {
            video_encode_element_->video_h264_encode_->StartEncode();
        }

        if (!video_push_element_->video_push_->PushStarted())
        {
            video_push_element_->video_push_->StartPush();
        }

        return 0;
    }

    int32_t MediaEngine::StopPush()
    {
        std::lock_guard<std::mutex> guard(api_lock_);

        VideoCaptureElement *capture_element = FindCaptureElem("living");
        if (!capture_element)
            return -1;

#ifdef ENABLE_ISP
        int32_t cam_id = capture_element->cam_id_;
#endif

        if (video_encode_element_ && video_encode_element_->video_h264_encode_)
        {
            if (video_encode_element_->video_h264_encode_->EncodeStarted())
                video_encode_element_->video_h264_encode_->StopEncode();
        }

        if (video_push_element_ && video_push_element_->video_push_)
        {
            if (video_push_element_->video_push_->PushStarted())
                video_push_element_->video_push_->StopPush();
        }

        if (capture_element->video_capture_broadcaster_ && video_encode_element_)
        {
            capture_element->video_capture_broadcaster_->RemoveSink(video_encode_element_->video_h264_encode_);
        }
        if (video_encode_element_ && video_encode_element_->video_encode_broadcaster_ && video_push_element_)
        {
            video_encode_element_->video_encode_broadcaster_->RemoveSink(video_push_element_->video_push_);
        }

        video_encode_element_.reset();
        video_push_element_.reset();

        if (!capture_element->video_capture_broadcaster_->frame_wanted())
        {
            if (capture_element->video_capture_->CaptureStarted())
            {
                capture_element->video_capture_->StopCapture();
            }
#if defined(AW_PLATFORM)
            AllWinnerIsp::GetInstance().Stop(
                capture_element->device_capture_capability_.device_index);
#endif
        }

#ifdef ENABLE_ISP
        IspManager::GetInstance().RemoveStream(cam_id);
#endif

        return 0;
    }

  int32_t MediaEngine::StartRtspPush()
    {
        std::lock_guard<std::mutex> guard(api_lock_);

        VideoCaptureElement *capture_element = FindCaptureElem("living");
        if (!capture_element)
            return -1;

#ifdef ENABLE_ISP
        IspManager::GetInstance().AddStream(capture_element->cam_id_);
#endif

        ecovacs::video_common::VideoCodecConfig video_codec_config;
        // 编码器 frameType 需与 IncomingFrame 输出的 buffer 类型一致：
        //   YUYV → 直通 kYUY2
        //   NV12 → 直通 kNV12
        //   RGB/I420/UYVY → IncomingFrame 转成 kNV12
#if defined(AM311X_PLATFORM)
        const std::string &img_type = capture_element->device_capture_capability_.image_type;
        if (img_type == "YUYV" || img_type == "YUY2") {
            video_codec_config.frameType = video_common::FrameType::kYUY2;
        } else {
            video_codec_config.frameType = video_common::FrameType::kNV12;
        }
#else
        video_codec_config.frameType = video_common::FrameType::kNV12;
#endif
        video_codec_config.codecType = video_common::VideoCodecType::kVideoCodecH264;
        video_codec_config.complexity_ = video_common::VideoCodecComplexity::kComplexityNormal;
        video_codec_config.width = media_engine_cfg_.encoder_cfg_.width;
        video_codec_config.height = media_engine_cfg_.encoder_cfg_.height;
        video_codec_config.crop_x = media_engine_cfg_.encoder_cfg_.crop_x;
        video_codec_config.crop_y = media_engine_cfg_.encoder_cfg_.crop_y;
        video_codec_config.maxFramerate = media_engine_cfg_.encoder_cfg_.max_fps;
        video_codec_config.maxBitrate = media_engine_cfg_.encoder_cfg_.max_bps;
        video_codec_config.minBitrate = media_engine_cfg_.encoder_cfg_.min_bps;
        video_codec_config.initBitrate = media_engine_cfg_.encoder_cfg_.init_bps;
        video_codec_config.codec_specific_.H264.init_qp = media_engine_cfg_.encoder_cfg_.init_qp;
        video_codec_config.codec_specific_.H264.min_qp = media_engine_cfg_.encoder_cfg_.min_qp;
        video_codec_config.codec_specific_.H264.max_qp = media_engine_cfg_.encoder_cfg_.max_qp;
        video_codec_config.codec_specific_.H264.gop = media_engine_cfg_.encoder_cfg_.gop;
        video_codec_config.codec_specific_.H264.profile = ecovacs::video_common::VideoCodecProfile::kProfileBase;

        if (!video_encode_element_)
        {
            video_encode_element_ = VideoH264EncodeElement::Create();
            if (!video_encode_element_)
                return -1;

            auto encode = VideoEncode::Create(&video_codec_config);
            if (!encode)
                return -1;
            video_encode_element_->video_h264_encode_ = std::shared_ptr<VideoEncode>(encode.release());

            video_encode_element_->video_encode_broadcaster_ = VideoEncodeBroadcaster::Create();
            if (!video_encode_element_->video_encode_broadcaster_)
                return -1;

            video_encode_element_->video_h264_encode_->RegisterEncodeCompleteCallback(
                video_encode_element_->video_encode_broadcaster_.get());
        }

        if (!video_rtsp_push_element_)
        {
            video_rtsp_push_element_ = VideoRtspPushElement::Create();
            if (!video_rtsp_push_element_)
                return -1;

#if defined(USE_LIVE555_RTSP)
            auto push = VideoLive555Push::Create();
#else
            auto push = VideoRtspPush::Create();
#endif
            if (!push)
                return -1;

            // 从配置中读取降噪等级
            push->SetAudioNoiseLevel(media_engine_cfg_.pusher_cfg_.audio_noise_level);
            ump_info("RTSP push: audio_noise_level=[%d]\r\n",
                     media_engine_cfg_.pusher_cfg_.audio_noise_level);

            // AEC / 采集配置透传
            push->SetAudioDevice(media_engine_cfg_.pusher_cfg_.audio_device);
            push->SetAudioCaptureChannels(media_engine_cfg_.pusher_cfg_.audio_capture_channels);
            push->SetAudioMicChannel(media_engine_cfg_.pusher_cfg_.audio_mic_channel);
            push->SetAudioRefChannel(media_engine_cfg_.pusher_cfg_.audio_ref_channel);
            push->SetAecEnabled(media_engine_cfg_.pusher_cfg_.aec_enabled != 0);

#if defined(USE_LIVE555_RTSP)
            video_rtsp_push_element_->video_push_ = std::shared_ptr<VideoLive555Push>(push.release());
#else
            video_rtsp_push_element_->video_push_ = std::shared_ptr<VideoRtspPush>(push.release());
#endif
        }

        capture_element->video_capture_broadcaster_->AddOrUpdateSink(video_encode_element_->video_h264_encode_);
        video_encode_element_->video_encode_broadcaster_->AddOrUpdateSink(video_rtsp_push_element_->video_push_);

        if (!capture_element->video_capture_->CaptureStarted())
        {
            capture_element->video_capture_->StartCapture();
        }

       if (capture_element->video_capture_->CaptureStarted())
        {
#if defined(AW_PLATFORM)
            AllWinnerIsp::GetInstance().Start(
                capture_element->device_capture_capability_.device_index);
#endif
        }

        if (!video_encode_element_->video_h264_encode_->EncodeStarted())
        {
            video_encode_element_->video_h264_encode_->StartEncode();
        }

        if (!video_rtsp_push_element_->video_push_->PushStarted())
        {
            video_rtsp_push_element_->video_push_->StartPush();
        }

        return 0;
    }

    int32_t MediaEngine::StopRtspPush()
    {
        std::lock_guard<std::mutex> guard(api_lock_);

        VideoCaptureElement *capture_element = FindCaptureElem("living");
        if (!capture_element)
            return -1;

#ifdef ENABLE_ISP
        int32_t cam_id = capture_element->cam_id_;
#endif

        if (video_encode_element_ && video_encode_element_->video_h264_encode_)
        {
            if (video_encode_element_->video_h264_encode_->EncodeStarted())
                video_encode_element_->video_h264_encode_->StopEncode();
        }

        if (video_rtsp_push_element_ && video_rtsp_push_element_->video_push_)
        {
            if (video_rtsp_push_element_->video_push_->PushStarted())
                video_rtsp_push_element_->video_push_->StopPush();
        }

        if (capture_element->video_capture_broadcaster_ && video_encode_element_)
        {
            capture_element->video_capture_broadcaster_->RemoveSink(video_encode_element_->video_h264_encode_);
        }
        if (video_encode_element_ && video_encode_element_->video_encode_broadcaster_ && video_rtsp_push_element_)
        {
            video_encode_element_->video_encode_broadcaster_->RemoveSink(video_rtsp_push_element_->video_push_);
        }

        video_encode_element_.reset();
        video_rtsp_push_element_.reset();

        if (!capture_element->video_capture_broadcaster_->frame_wanted())
        {
            if (capture_element->video_capture_->CaptureStarted())
            {
                capture_element->video_capture_->StopCapture();
            }
#if defined(AW_PLATFORM)
            AllWinnerIsp::GetInstance().Stop(
                capture_element->device_capture_capability_.device_index);
#endif
        }

#ifdef ENABLE_ISP
        IspManager::GetInstance().RemoveStream(cam_id);
#endif

        return 0;
    }

#if defined(ENABLE_LIBPEER)
    // ========== Libpeer 推流 ==========
    int32_t MediaEngine::StartLibpeerPush()
    {
        std::lock_guard<std::mutex> guard(api_lock_);

        VideoCaptureElement *capture_element = FindCaptureElem("living");
        if (!capture_element)
            return -1;

#ifdef ENABLE_ISP
        IspManager::GetInstance().AddStream(capture_element->cam_id_);
#endif

        ecovacs::video_common::VideoCodecConfig video_codec_config;
        // 编码器 frameType 需与 IncomingFrame 输出的 buffer 类型一致：
        //   YUYV → 直通 kYUY2
        //   NV12 → 直通 kNV12
        //   RGB/I420/UYVY → IncomingFrame 转成 kNV12
#if defined(AM311X_PLATFORM)
        const std::string &img_type = capture_element->device_capture_capability_.image_type;
        if (img_type == "YUYV" || img_type == "YUY2") {
            video_codec_config.frameType = video_common::FrameType::kYUY2;
        } else {
            video_codec_config.frameType = video_common::FrameType::kNV12;
        }
#else
        video_codec_config.frameType = video_common::FrameType::kNV12;
#endif
        video_codec_config.codecType = video_common::VideoCodecType::kVideoCodecH264;
        video_codec_config.complexity_ = video_common::VideoCodecComplexity::kComplexityNormal;
        video_codec_config.width = media_engine_cfg_.encoder_cfg_.width;
        video_codec_config.height = media_engine_cfg_.encoder_cfg_.height;
        video_codec_config.crop_x = media_engine_cfg_.encoder_cfg_.crop_x;
        video_codec_config.crop_y = media_engine_cfg_.encoder_cfg_.crop_y;
        video_codec_config.maxFramerate = media_engine_cfg_.encoder_cfg_.max_fps;
        video_codec_config.maxBitrate = media_engine_cfg_.encoder_cfg_.max_bps;
        video_codec_config.minBitrate = media_engine_cfg_.encoder_cfg_.min_bps;
        video_codec_config.initBitrate = media_engine_cfg_.encoder_cfg_.init_bps;
        video_codec_config.codec_specific_.H264.init_qp = media_engine_cfg_.encoder_cfg_.init_qp;
        video_codec_config.codec_specific_.H264.min_qp = media_engine_cfg_.encoder_cfg_.min_qp;
        video_codec_config.codec_specific_.H264.max_qp = media_engine_cfg_.encoder_cfg_.max_qp;
        video_codec_config.codec_specific_.H264.gop = media_engine_cfg_.encoder_cfg_.gop;
        video_codec_config.codec_specific_.H264.profile = ecovacs::video_common::VideoCodecProfile::kProfileBase;

        if (!video_encode_element_)
        {
            video_encode_element_ = VideoH264EncodeElement::Create();
            if (!video_encode_element_)
                return -1;

            auto encode = VideoEncode::Create(&video_codec_config);
            if (!encode)
                return -1;
            video_encode_element_->video_h264_encode_ = std::shared_ptr<VideoEncode>(encode.release());

            video_encode_element_->video_encode_broadcaster_ = VideoEncodeBroadcaster::Create();
            if (!video_encode_element_->video_encode_broadcaster_)
                return -1;

            capture_element->video_capture_broadcaster_->AddOrUpdateSink(
                video_encode_element_->video_h264_encode_);
            video_encode_element_->video_h264_encode_->RegisterEncodeCompleteCallback(
                video_encode_element_->video_encode_broadcaster_.get());
        }

        if (!video_libpeer_push_element_)
        {
            video_libpeer_push_element_ = VideoLibpeerPushElement::Create();
            if (!video_libpeer_push_element_)
                return -1;

            auto push = VideoLibpeerPush::Create();
            if (!push)
                return -1;
            video_libpeer_push_element_->video_push_ = std::shared_ptr<VideoLibpeerPush>(push.release());
        }

        capture_element->video_capture_broadcaster_->AddOrUpdateSink(
            video_encode_element_->video_h264_encode_);
        video_encode_element_->video_encode_broadcaster_->AddOrUpdateSink(
            video_libpeer_push_element_->video_push_);

        if (!capture_element->video_capture_->CaptureStarted())
        {
            capture_element->video_capture_->StartCapture();
        }

        if (capture_element->video_capture_->CaptureStarted())
        {
#if defined(AW_PLATFORM)
            AllWinnerIsp::GetInstance().Start(
                capture_element->device_capture_capability_.device_index);
#endif
        }

        if (!video_encode_element_->video_h264_encode_->EncodeStarted())
        {
            video_encode_element_->video_h264_encode_->StartEncode();
        }

        if (!video_libpeer_push_element_->video_push_->PushStarted())
        {
            video_libpeer_push_element_->video_push_->StartPush();
        }

        return 0;
    }

    int32_t MediaEngine::StopLibpeerPush()
    {
        std::lock_guard<std::mutex> guard(api_lock_);

        // 停 libpeer 推流
        if (video_libpeer_push_element_ && video_libpeer_push_element_->video_push_) {
            if (video_libpeer_push_element_->video_push_->PushStarted())
                video_libpeer_push_element_->video_push_->StopPush();
        }

        // 从 encode broadcaster 移除 libpeer sink
        if (video_encode_element_ && video_encode_element_->video_encode_broadcaster_ && video_libpeer_push_element_) {
            video_encode_element_->video_encode_broadcaster_->RemoveSink(
                video_libpeer_push_element_->video_push_);
        }

        video_libpeer_push_element_.reset();

        // 如果 RTSP 也停了（只剩 libpeer 一路用 capture/encode），就一起停
        bool encode_shared = (video_rtsp_push_element_ != nullptr);
        if (!encode_shared && video_encode_element_ && video_encode_element_->video_h264_encode_) {
            if (video_encode_element_->video_h264_encode_->EncodeStarted())
                video_encode_element_->video_h264_encode_->StopEncode();
        }

        // 只有 RTSP 和 libpeer 都停了才停 capture
        VideoCaptureElement *capture_element = FindCaptureElem("living");
        bool any_push = (video_rtsp_push_element_ != nullptr) || (video_libpeer_push_element_ != nullptr);
        if (!any_push && capture_element && !capture_element->video_capture_broadcaster_->frame_wanted()) {
            if (capture_element->video_capture_->CaptureStarted())
                capture_element->video_capture_->StopCapture();
        }

        return 0;
    }
#endif

    // ========== AI 相关 ==========
    int32_t MediaEngine::StartAI()
    {
        std::lock_guard<std::mutex> guard(api_lock_);

        VideoCaptureElement *capture_element = FindCaptureElem("ai");
        if (!capture_element)
            return -1;

#ifdef ENABLE_ISP
        IspManager::GetInstance().AddStream(capture_element->cam_id_);
#endif

        if (!video_ai_process_element_)
        {
            video_ai_process_element_ = VideoAiProcessElement::Create();
            if (!video_ai_process_element_)
                return -1;

            auto ai = VideoAI::Create();
            if (!ai)
                return -1;
            video_ai_process_element_->video_ai_ = std::shared_ptr<VideoAI>(ai.release());
        }

        capture_element->video_capture_broadcaster_->AddOrUpdateSink(video_ai_process_element_->video_ai_);

        if (!capture_element->video_capture_->CaptureStarted())
        {
            capture_element->video_capture_->StartCapture();
        }

        if (capture_element->video_capture_->CaptureStarted())
        {
#if defined(AW_PLATFORM)
            AllWinnerIsp::GetInstance().Start(
                capture_element->device_capture_capability_.device_index);
#endif
        }

        if (!video_ai_process_element_->video_ai_->AIStarted())
        {
            video_ai_process_element_->video_ai_->StartAI();
#ifdef ENABLE_WEBSOCKETS
            video_ai_process_element_->video_ai_->SetJpegParams(85, true);
            video_ai_process_element_->video_ai_->StartWebSocketServer();
#endif
        }

        return 0;
    }

    int32_t MediaEngine::StopAI()
    {
        std::lock_guard<std::mutex> guard(api_lock_);

        VideoCaptureElement *capture_element = FindCaptureElem("ai");
        if (!capture_element)
            return -1;

#ifdef ENABLE_ISP
        int32_t cam_id = capture_element->cam_id_;
#endif

        // 1. 先停止 AI 模块
        if (video_ai_process_element_ && video_ai_process_element_->video_ai_)
        {
            if (video_ai_process_element_->video_ai_->AIStarted())
            {
                video_ai_process_element_->video_ai_->StopAI();
                ump_info("AI stopped\n");
            }
        }

        // 2. 再从广播器移除
        if (capture_element->video_capture_broadcaster_ && video_ai_process_element_)
        {
            capture_element->video_capture_broadcaster_->RemoveSink(video_ai_process_element_->video_ai_);
            ump_info("Removed AI from capture broadcaster\n");
        }

        // 3. 最后释放资源
        if (video_ai_process_element_)
        {
            video_ai_process_element_->video_ai_.reset();
            video_ai_process_element_.reset();
        }

        // 4. 停止采集（仅当没有其他消费者时）
        if (!capture_element->video_capture_broadcaster_->frame_wanted())
        {
            if (capture_element->video_capture_->CaptureStarted())
            {
                capture_element->video_capture_->StopCapture();
                ump_info("Capture stopped\n");
            }
#if defined(AW_PLATFORM)
            AllWinnerIsp::GetInstance().Stop(
                capture_element->device_capture_capability_.device_index);
#endif
        }

#ifdef ENABLE_ISP
        IspManager::GetInstance().RemoveStream(cam_id);
#endif

        return 0;
    }

    // ========== 抓拍相关 ==========
    int32_t MediaEngine::StartTakePhotograph(video_common::VideoJpegGrap grap_config)
    {
        std::lock_guard<std::mutex> guard(api_lock_);

        VideoCaptureElement *capture_element = FindCaptureElem("living");
        if (!capture_element)
            return -1;

#ifdef ENABLE_ISP
        IspManager::GetInstance().AddStream(capture_element->cam_id_);
#endif

        ecovacs::video_common::VideoCodecConfig video_codec_config;
#if defined(AM311X_PLATFORM)
        // JPEG 编码用 living 路，frameType 需与 IncomingFrame 输出一致
        const std::string &img_type = capture_element->device_capture_capability_.image_type;
        if (img_type == "YUYV" || img_type == "YUY2") {
            video_codec_config.frameType = video_common::FrameType::kYUY2;
        } else {
            video_codec_config.frameType = video_common::FrameType::kNV12;
        }
#else
        video_codec_config.frameType = video_common::FrameType::kNV12;
#endif
        video_codec_config.codecType = video_common::VideoCodecType::kVideoCodecJPEG;
        video_codec_config.complexity_ = video_common::VideoCodecComplexity::kComplexityNormal;
        video_codec_config.width = media_engine_cfg_.encoder_cfg_.width;
        video_codec_config.height = media_engine_cfg_.encoder_cfg_.height;
        video_codec_config.crop_x = media_engine_cfg_.encoder_cfg_.crop_x;
        video_codec_config.crop_y = media_engine_cfg_.encoder_cfg_.crop_y;
        video_codec_config.maxFramerate = media_engine_cfg_.encoder_cfg_.max_fps;
        video_codec_config.maxBitrate = media_engine_cfg_.encoder_cfg_.max_bps;
        video_codec_config.minBitrate = media_engine_cfg_.encoder_cfg_.min_bps;
        video_codec_config.initBitrate = media_engine_cfg_.encoder_cfg_.init_bps;

        if (!video_photograph_element_)
        {
            video_photograph_element_ = VideoPhotographElement::Create();
            if (!video_photograph_element_)
                return -1;

            auto encode = VideoEncode::Create(&video_codec_config);
            if (!encode)
                return -1;
            video_photograph_element_->video_jpeg_encode_ = std::shared_ptr<VideoEncode>(encode.release());

            video_photograph_element_->video_encode_broadcaster_ = VideoEncodeBroadcaster::Create();
            if (!video_photograph_element_->video_encode_broadcaster_)
                return -1;

            video_photograph_element_->video_jpeg_encode_->RegisterEncodeCompleteCallback(
                video_photograph_element_->video_encode_broadcaster_.get());
        }

        if (!jpeg_pic_upload_element_)
        {
            jpeg_pic_upload_element_ = JpegPicUploadElement::Create();
            if (!jpeg_pic_upload_element_)
                return -1;

            auto upload = JpegUpload::Create();
            if (!upload)
                return -1;
            jpeg_pic_upload_element_->jpeg_upload_ = std::shared_ptr<JpegUpload>(upload.release());
        }

        capture_element->video_capture_broadcaster_->AddOrUpdateSink(video_photograph_element_->video_jpeg_encode_);
        video_photograph_element_->video_encode_broadcaster_->AddOrUpdateSink(jpeg_pic_upload_element_->jpeg_upload_);

        // 记录 FR 流是否在抓拍前已经运行
        bool fr_already_running = capture_element->video_capture_->CaptureStarted();

        if (!fr_already_running)
        {
            capture_element->video_capture_->StartCapture();
        }

        if (capture_element->video_capture_->CaptureStarted())
        {
#if defined(AW_PLATFORM)
            AllWinnerIsp::GetInstance().Start(
                capture_element->device_capture_capability_.device_index);
#endif
        }

        if (!video_photograph_element_->video_jpeg_encode_->EncodeStarted())
        {
#if defined(AM311X_PLATFORM)
            if (!fr_already_running) {
                // AM311x: FR 流刚启动，ISP 有预热帧（绿色），跳过前 30 帧再编码
                video_photograph_element_->video_jpeg_encode_->SetSkipFrames(30);
                ump_info("StartTakePhotograph: FR just started, skip 30 warmup frames\n");
            } else {
                // FR 流已在运行（推流常开），直接编码，实时抓拍
                video_photograph_element_->video_jpeg_encode_->SetSkipFrames(0);
                ump_info("StartTakePhotograph: FR already running, realtime capture\n");
            }
#else
            video_photograph_element_->video_jpeg_encode_->SetSkipFrames(0);
#endif
            video_photograph_element_->video_jpeg_encode_->StartEncode();
        }

        if (!jpeg_pic_upload_element_->jpeg_upload_->UploadStarted())
        {
            jpeg_pic_upload_element_->jpeg_upload_->StartUpload(grap_config);
        }

        return 0;
    }

    int32_t MediaEngine::StopTakePhotograph()
    {
        std::lock_guard<std::mutex> guard(api_lock_);

        VideoCaptureElement *capture_element = FindCaptureElem("living");
        if (!capture_element)
            return -1;

#ifdef ENABLE_ISP
        int32_t cam_id = capture_element->cam_id_;
#endif

        if (video_photograph_element_ && video_photograph_element_->video_jpeg_encode_)
        {
            if (video_photograph_element_->video_jpeg_encode_->EncodeStarted())
                video_photograph_element_->video_jpeg_encode_->StopEncode();
        }

        if (jpeg_pic_upload_element_ && jpeg_pic_upload_element_->jpeg_upload_)
        {
            if (jpeg_pic_upload_element_->jpeg_upload_->UploadStarted())
                jpeg_pic_upload_element_->jpeg_upload_->StopUpload();
        }

        if (capture_element->video_capture_broadcaster_ && video_photograph_element_)
        {
            capture_element->video_capture_broadcaster_->RemoveSink(video_photograph_element_->video_jpeg_encode_);
        }
        if (video_photograph_element_ && video_photograph_element_->video_encode_broadcaster_ && jpeg_pic_upload_element_)
        {
            video_photograph_element_->video_encode_broadcaster_->RemoveSink(jpeg_pic_upload_element_->jpeg_upload_);
        }

        video_photograph_element_.reset();
        jpeg_pic_upload_element_.reset();

        if (!capture_element->video_capture_broadcaster_->frame_wanted())
        {
            if (capture_element->video_capture_->CaptureStarted())
            {
                capture_element->video_capture_->StopCapture();
            }
#if defined(AW_PLATFORM)
            AllWinnerIsp::GetInstance().Stop(
                capture_element->device_capture_capability_.device_index);
#endif
        }

#ifdef ENABLE_ISP
        IspManager::GetInstance().RemoveStream(cam_id);
#endif

        return 0;
    }

    // ========== 裸视频保存相关 ==========
    int32_t MediaEngine::StartSaveRawVideo()
    {
        std::lock_guard<std::mutex> guard(api_lock_);

        VideoCaptureElement *capture_element = FindCaptureElem("living");
        if (!capture_element)
            return -1;

#ifdef ENABLE_ISP
        IspManager::GetInstance().AddStream(capture_element->cam_id_);
#endif

        if (!video_raw_video_save_element_)
        {
            video_raw_video_save_element_ = RawVideoSaveElement::Create();
            if (!video_raw_video_save_element_)
                return -1;

            auto save = RawVideoSave::Create();
            if (!save)
                return -1;
            video_raw_video_save_element_->raw_video_save_ = std::shared_ptr<RawVideoSave>(save.release());
        }

        capture_element->video_capture_broadcaster_->AddOrUpdateSink(video_raw_video_save_element_->raw_video_save_);

        if (!capture_element->video_capture_->CaptureStarted())
        {
            capture_element->video_capture_->StartCapture();
        }

        if (capture_element->video_capture_->CaptureStarted())
        {
#if defined(AW_PLATFORM)
            AllWinnerIsp::GetInstance().Start(
                capture_element->device_capture_capability_.device_index);
#endif
        }

        if (!video_raw_video_save_element_->raw_video_save_->SaveStarted())
        {
            video_raw_video_save_element_->raw_video_save_->StartSave();
        }

        return 0;
    }

    int32_t MediaEngine::StopSaveRawVideo()
    {
        std::lock_guard<std::mutex> guard(api_lock_);

        VideoCaptureElement *capture_element = FindCaptureElem("living");
        if (!capture_element)
            return -1;

#ifdef ENABLE_ISP
        int32_t cam_id = capture_element->cam_id_;
#endif

        // 1. 先停止保存模块
        if (video_raw_video_save_element_ && video_raw_video_save_element_->raw_video_save_)
        {
            if (video_raw_video_save_element_->raw_video_save_->SaveStarted())
            {
                video_raw_video_save_element_->raw_video_save_->StopSave();
                ump_info("Raw video save stopped\n");
            }
        }

        // 2. 再从广播器移除
        if (capture_element->video_capture_broadcaster_ && video_raw_video_save_element_)
        {
            capture_element->video_capture_broadcaster_->RemoveSink(video_raw_video_save_element_->raw_video_save_);
            ump_info("Removed raw save from capture broadcaster\n");
        }

        // 3. 最后释放资源
        if (video_raw_video_save_element_)
        {
            video_raw_video_save_element_->raw_video_save_.reset();
            video_raw_video_save_element_.reset();
        }

        // 4. 停止采集（仅当没有其他消费者时）
        if (!capture_element->video_capture_broadcaster_->frame_wanted())
        {
            if (capture_element->video_capture_->CaptureStarted())
            {
                capture_element->video_capture_->StopCapture();
                ump_info("Capture stopped\n");
            }
#if defined(AW_PLATFORM)
            AllWinnerIsp::GetInstance().Stop(
                capture_element->device_capture_capability_.device_index);
#endif
        }

#ifdef ENABLE_ISP
        IspManager::GetInstance().RemoveStream(cam_id);
#endif

        return 0;
    }

    // ========== JPEG 发布相关 ==========
    int32_t MediaEngine::StartJpegPublish()
    {
        std::lock_guard<std::mutex> guard(api_lock_);

        VideoCaptureElement *capture_element = FindCaptureElem("living");
        if (!capture_element)
            return -1;

#ifdef ENABLE_ISP
        IspManager::GetInstance().AddStream(capture_element->cam_id_);
#endif

        ecovacs::video_common::VideoCodecConfig video_codec_config;
#if defined(AM311X_PLATFORM)
        // JPEG 编码用 living 路，frameType 需与 IncomingFrame 输出一致
        const std::string &img_type = capture_element->device_capture_capability_.image_type;
        if (img_type == "YUYV" || img_type == "YUY2") {
            video_codec_config.frameType = video_common::FrameType::kYUY2;
        } else {
            video_codec_config.frameType = video_common::FrameType::kNV12;
        }
#else
        video_codec_config.frameType = video_common::FrameType::kNV12;
#endif
        video_codec_config.codecType = video_common::VideoCodecType::kVideoCodecJPEG;
        video_codec_config.complexity_ = video_common::VideoCodecComplexity::kComplexityNormal;
        video_codec_config.width = media_engine_cfg_.encoder_cfg_.width;
        video_codec_config.height = media_engine_cfg_.encoder_cfg_.height;
        video_codec_config.crop_x = media_engine_cfg_.encoder_cfg_.crop_x;
        video_codec_config.crop_y = media_engine_cfg_.encoder_cfg_.crop_y;
        video_codec_config.maxFramerate = media_engine_cfg_.encoder_cfg_.max_fps;
        video_codec_config.maxBitrate = media_engine_cfg_.encoder_cfg_.max_bps;
        video_codec_config.minBitrate = media_engine_cfg_.encoder_cfg_.min_bps;
        video_codec_config.initBitrate = media_engine_cfg_.encoder_cfg_.init_bps;

        if (!video_jpeg_publish_element_)
        {
            video_jpeg_publish_element_ = VideoPhotographElement::Create();
            if (!video_jpeg_publish_element_)
                return -1;

            auto encode = VideoEncode::Create(&video_codec_config);
            if (!encode)
                return -1;
            video_jpeg_publish_element_->video_jpeg_encode_ = std::shared_ptr<VideoEncode>(encode.release());

            video_jpeg_publish_element_->video_encode_broadcaster_ = VideoEncodeBroadcaster::Create();
            if (!video_jpeg_publish_element_->video_encode_broadcaster_)
                return -1;

            video_jpeg_publish_element_->video_jpeg_encode_->RegisterEncodeCompleteCallback(
                video_jpeg_publish_element_->video_encode_broadcaster_.get());
        }

        if (!jpeg_pic_publish_element_)
        {
            jpeg_pic_publish_element_ = JpegPicPublishElement::Create();
            if (!jpeg_pic_publish_element_)
                return -1;

            auto publish = JpegPublish::Create();
            if (!publish)
                return -1;
            jpeg_pic_publish_element_->jpeg_publish_ = std::shared_ptr<JpegPublish>(publish.release());
        }

        capture_element->video_capture_broadcaster_->AddOrUpdateSink(video_jpeg_publish_element_->video_jpeg_encode_);
        video_jpeg_publish_element_->video_encode_broadcaster_->AddOrUpdateSink(jpeg_pic_publish_element_->jpeg_publish_);

        if (!capture_element->video_capture_->CaptureStarted())
        {
            capture_element->video_capture_->StartCapture();
        }

        if (capture_element->video_capture_->CaptureStarted())
        {
#if defined(AW_PLATFORM)
            AllWinnerIsp::GetInstance().Start(
                capture_element->device_capture_capability_.device_index);
#endif
        }

        if (!video_jpeg_publish_element_->video_jpeg_encode_->EncodeStarted())
        {
            video_jpeg_publish_element_->video_jpeg_encode_->StartEncode();
        }

        if (!jpeg_pic_publish_element_->jpeg_publish_->PublishStarted())
        {
            jpeg_pic_publish_element_->jpeg_publish_->StartPublish();
        }

        return 0;
    }

    int32_t MediaEngine::StopJpegPublish()
    {
        std::lock_guard<std::mutex> guard(api_lock_);

        VideoCaptureElement *capture_element = FindCaptureElem("living");
        if (!capture_element)
            return -1;

#ifdef ENABLE_ISP
        int32_t cam_id = capture_element->cam_id_;
#endif

        // 1. 先停止发布模块
        if (jpeg_pic_publish_element_ && jpeg_pic_publish_element_->jpeg_publish_)
        {
            if (jpeg_pic_publish_element_->jpeg_publish_->PublishStarted())
            {
                jpeg_pic_publish_element_->jpeg_publish_->StopPublish();
                ump_info("JPEG publish stopped\n");
            }
        }

        // 2. 停止编码器
        if (video_jpeg_publish_element_ && video_jpeg_publish_element_->video_jpeg_encode_)
        {
            if (video_jpeg_publish_element_->video_jpeg_encode_->EncodeStarted())
            {
                video_jpeg_publish_element_->video_jpeg_encode_->StopEncode();
                ump_info("JPEG encoder stopped\n");
            }
        }

        // 3. 再从广播器移除
        if (capture_element->video_capture_broadcaster_ && video_jpeg_publish_element_)
        {
            capture_element->video_capture_broadcaster_->RemoveSink(video_jpeg_publish_element_->video_jpeg_encode_);
            ump_info("Removed JPEG encoder from capture broadcaster\n");
        }
        if (video_jpeg_publish_element_ && video_jpeg_publish_element_->video_encode_broadcaster_ && jpeg_pic_publish_element_)
        {
            video_jpeg_publish_element_->video_encode_broadcaster_->RemoveSink(jpeg_pic_publish_element_->jpeg_publish_);
            ump_info("Removed publish from encoder broadcaster\n");
        }

        // 4. 最后释放资源
        if (video_jpeg_publish_element_)
        {
            video_jpeg_publish_element_->video_jpeg_encode_.reset();
            video_jpeg_publish_element_.reset();
        }
        if (jpeg_pic_publish_element_)
        {
            jpeg_pic_publish_element_->jpeg_publish_.reset();
            jpeg_pic_publish_element_.reset();
        }

        // 5. 停止采集（仅当没有其他消费者时）
        if (!capture_element->video_capture_broadcaster_->frame_wanted())
        {
            if (capture_element->video_capture_->CaptureStarted())
            {
                capture_element->video_capture_->StopCapture();
                ump_info("Capture stopped\n");
            }
#if defined(AW_PLATFORM)
            AllWinnerIsp::GetInstance().Stop(
                capture_element->device_capture_capability_.device_index);
#endif
        }

#ifdef ENABLE_ISP
        IspManager::GetInstance().RemoveStream(cam_id);
#endif

        return 0;
    }

    int32_t MediaEngine::StartRecord() { return 0; }
    int32_t MediaEngine::StopRecord() { return 0; }
    int32_t MediaEngine::StartQrIdentify() { return 0; }
    int32_t MediaEngine::StopQrIdentify() { return 0; }

    int32_t MediaEngine::UnInit()
    {
        std::lock_guard<std::mutex> guard(api_lock_);

        StopPush();
        StopRtspPush();
        StopTakePhotograph();
        StopAI();
        StopSaveRawVideo();
        StopJpegPublish();

        video_capture_sources_.clear();

        quit_.store(true);
        if (event_thread_.joinable())
            event_thread_.join();

#ifdef ENABLE_ISP
        IspManager::GetInstance().UninitAll();
#endif

        bus_deinit();

        return 0;
    }

} // namespace ecovacs
