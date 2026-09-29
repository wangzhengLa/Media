#ifndef API_RAW_VIDEO_SAVE_H_
#define API_RAW_VIDEO_SAVE_H_

#include <thread>
#include <memory>
#include <mutex>
#include <atomic>
#include "video_common_defines.h"
#include "queue.h"

#define MAX_RAW_FRAME_QUEUE_SIZE (1)

namespace ecovacs
{
    class RawVideoSave : public video_common::VideoSinkInterface
    {
    public:
        RawVideoSave();  // 构造函数改为 public
        ~RawVideoSave() override;

        static std::unique_ptr<RawVideoSave> Create();

        int32_t Init();
        int32_t UnInit();
        int32_t StartSave();
        int32_t StopSave();
        bool SaveStarted();

        void OnFrame(video_common::VideoFrameBuffer *frame_buffer) override;

    private:
        bool SaveProcess();

        struct FrameBuffer
        {
            video_common::VideoFrameBuffer *frame;
        };

        TIntelliBufHndl frame_buf_hndl_;
        FrameBuffer frame_buf_[MAX_RAW_FRAME_QUEUE_SIZE];
        std::mutex api_lock_;
        std::thread saveThread_;
        std::atomic<bool> quit_{false};
        std::atomic<bool> saveStarted_{false};
        std::atomic<bool> save_raw_video_flag_{false};
    };

} // namespace ecovacs

#endif // API_RAW_VIDEO_SAVE_H_