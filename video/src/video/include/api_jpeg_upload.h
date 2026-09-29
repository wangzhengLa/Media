#ifndef API_JPEG_UPLOAD_H_
#define API_JPEG_UPLOAD_H_

#include <thread>
#include <memory>
#include <mutex>
#include <atomic>
#include "video_common_defines.h"
#include "queue.h"

#define MAX_JPEG_QUEUE_SIZE (10)

namespace ecovacs
{
    class JpegUpload : public video_common::EncodedImageSinkInterface
    {
    public:
        JpegUpload(); // 构造函数改为 public
        ~JpegUpload() override;

        static std::unique_ptr<JpegUpload> Create();

        int32_t Init();
        int32_t UnInit();

        int32_t StartUpload(video_common::VideoJpegGrap grap_conf);
        int32_t StopUpload();
        bool UploadStarted();

        void OnFrame(video_common::EncodedImageBuffer *encoded_image,
                     const video_common::CodecSpecificInfo *codec_specific_info) override;

        static void StartUrlUpload(std::string upload_cfg, int32_t iot_cfg_flag);

    private:
        bool UploadProcess();

        struct TJpegBuffer
        {
            video_common::EncodedImageBuffer *jpeg_frame;
        };

        TIntelliBufHndl jpeg_buf_hndl_;
        TJpegBuffer jpeg_buf_[MAX_JPEG_QUEUE_SIZE];
        std::mutex api_lock_;
        std::thread uploadThread_;
        video_common::VideoJpegGrap grap_config_;
        std::atomic<bool> quit_{false};
        std::atomic<bool> uploadStarted_{false};
    };

} // namespace ecovacs

#endif // API_JPEG_UPLOAD_H_