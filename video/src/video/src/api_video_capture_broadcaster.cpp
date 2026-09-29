#include <memory>
#include "api_video_capture_broadcaster.h"

namespace ecovacs
{
    VideoCaptureBroadcaster::VideoCaptureBroadcaster() = default;
    VideoCaptureBroadcaster::~VideoCaptureBroadcaster() = default;

    std::unique_ptr<VideoCaptureBroadcaster> VideoCaptureBroadcaster::Create()
    {
        return std::make_unique<VideoCaptureBroadcaster>();
    }

    void VideoCaptureBroadcaster::AddOrUpdateSink(std::shared_ptr<video_common::VideoSinkInterface> sink)
    {
        broadcaster_.AddOrUpdateSink(std::move(sink));
    }

    void VideoCaptureBroadcaster::RemoveSink(std::shared_ptr<video_common::VideoSinkInterface> sink)
    {
        broadcaster_.RemoveSink(std::move(sink));
    }

    void VideoCaptureBroadcaster::OnFrame(video_common::VideoFrameBuffer *frame)
    {
        broadcaster_.OnFrame(frame);
    }

    bool VideoCaptureBroadcaster::frame_wanted() const
    {
        return broadcaster_.frame_wanted();
    }

} // namespace ecovacs