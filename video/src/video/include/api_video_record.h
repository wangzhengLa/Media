#ifndef API_VIDEO_RECORD_H_
#define API_VIDEO_RECORD_H_

#include <memory>
#include <mutex>
#include <atomic>
#include <string>
#include <thread>

#include "video_common_defines.h"
#include "video_recorder.h"
#include "queue.h"

namespace ecovacs
{
    /**
     * @brief 视频录像模块
     *
     * 作为编码器的 sink，接收 H.264 编码帧并写入 MP4 文件。
     * OnFrame 仅做入队，写文件在独立线程中完成，避免阻塞编码器回调线程。
     */
    class VideoRecord : public video_common::EncodedImageSinkInterface
    {
    public:
        VideoRecord();
        ~VideoRecord() override;

        static std::unique_ptr<VideoRecord> Create();

        int32_t Init();
        int32_t UnInit();

        /**
         * @brief 开始录像
         *
         * @param filename 输出文件路径
         * @param width    视频宽度
         * @param height   视频高度
         * @param fps      帧率
         * @return 0 成功，<0 失败
         */
        int32_t StartRecord(const std::string &filename, int width, int height, int fps);

        /**
         * @brief 停止录像
         */
        int32_t StopRecord();

        /**
         * @brief 是否正在录像
         */
        bool IsRecording();

        /**
         * @brief 编码帧回调，入队后立即返回
         */
        void OnFrame(video_common::EncodedImageBuffer *encoded_image,
                     const video_common::CodecSpecificInfo *codec_specific_info) override;

    private:
        static constexpr int kMaxQueueSize = 32;

        struct TRecordBuffer {
            video_common::EncodedImageBuffer *h264_frame = nullptr;  // AddRef'd, RelRef by consumer
        };

        // Release all buffered FULL frames' refs and return buffers to EMPTY.
        void DrainQueue();
        void WriteThread();

        std::mutex api_lock_;
        std::unique_ptr<ump::video_recording::Mp4Recorder> recorder_;
        std::atomic<bool> recording_{false};

        TIntelliBufHndl buf_hndl_ = {};
        TRecordBuffer record_buf_[kMaxQueueSize];

        std::thread write_thread_;
        std::atomic<bool> quit_{false};
    };

} // namespace ecovacs

#endif // API_VIDEO_RECORD_H_
