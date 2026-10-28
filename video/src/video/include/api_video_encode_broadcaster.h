#ifndef API_VIDEO_ENCODE_BROADCASTER_H_
#define API_VIDEO_ENCODE_BROADCASTER_H_

#include <memory>
#include "video_common_defines.h"

namespace ecovacs
{
    class VideoEncodeBroadcaster : public video_common::EncodedImageSourceInterface, public video_common::EncodedImageSinkInterface
    {
    public:
        static std::unique_ptr<VideoEncodeBroadcaster> Create();

        VideoEncodeBroadcaster();
        ~VideoEncodeBroadcaster() override;

        void OnFrame(video_common::EncodedImageBuffer *encoded_image, const video_common::CodecSpecificInfo *codec_specific_info) override;

        // 修改为接受 shared_ptr
        void AddOrUpdateSink(std::shared_ptr<video_common::EncodedImageSinkInterface> sink) override;
        void RemoveSink(std::shared_ptr<video_common::EncodedImageSinkInterface> sink) override;

        bool frame_wanted() const;

        void OnDiscardedFrame() override;

    private:
        video_common::EncodedImageBroadcaster broadcaster_;
    };

} // namespace ecovacs

#endif // API_VIDEO_ENCODE_BROADCASTER_H_