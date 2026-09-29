#ifndef API_VIDEO_CAPTURE_BROADCASTER_H_
#define API_VIDEO_CAPTURE_BROADCASTER_H_

#include <memory>
#include "video_common_defines.h"

namespace ecovacs
{
    class VideoCaptureBroadcaster : public video_common::VideoSourceInterface, public video_common::VideoSinkInterface
    {
    public:
        static std::unique_ptr<VideoCaptureBroadcaster> Create();

        VideoCaptureBroadcaster();
        ~VideoCaptureBroadcaster() override;

        void OnFrame(video_common::VideoFrameBuffer *frame) override;

        // 修改为接受 shared_ptr
        void AddOrUpdateSink(std::shared_ptr<video_common::VideoSinkInterface> sink) override;
        void RemoveSink(std::shared_ptr<video_common::VideoSinkInterface> sink) override;

        bool frame_wanted() const;

    private:
        video_common::VideoBroadcaster broadcaster_;
    };

} // namespace ecovacs

#endif // API_VIDEO_CAPTURE_BROADCASTER_H_