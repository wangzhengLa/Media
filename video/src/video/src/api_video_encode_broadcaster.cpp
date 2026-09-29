#include <memory>
#include "api_video_encode_broadcaster.h"

namespace ecovacs
{
    VideoEncodeBroadcaster::VideoEncodeBroadcaster() = default;
    VideoEncodeBroadcaster::~VideoEncodeBroadcaster() = default;

    std::unique_ptr<VideoEncodeBroadcaster> VideoEncodeBroadcaster::Create()
    {
        return std::make_unique<VideoEncodeBroadcaster>();
    }

    void VideoEncodeBroadcaster::AddOrUpdateSink(std::shared_ptr<video_common::EncodedImageSinkInterface> sink)
    {
        broadcaster_.AddOrUpdateSink(std::move(sink));
    }

    void VideoEncodeBroadcaster::RemoveSink(std::shared_ptr<video_common::EncodedImageSinkInterface> sink)
    {
        broadcaster_.RemoveSink(std::move(sink));
    }

    void VideoEncodeBroadcaster::OnFrame(video_common::EncodedImageBuffer *encoded_image, const video_common::CodecSpecificInfo *codec_specific_info)
    {
        broadcaster_.OnFrame(encoded_image, codec_specific_info);
    }

    void VideoEncodeBroadcaster::OnDiscardedFrame()
    {
        broadcaster_.OnDiscardedFrame();
    }

} // namespace ecovacs