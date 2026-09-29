#ifndef __TOPIC_NAMES_H__
#define __TOPIC_NAMES_H__

namespace topics
{
    constexpr char CAMERA_IMAGE_JPEG[] = "/camera/image/compressed_jpeg";
    constexpr char CAMERA_IMAGE_H264[] = "/camera/image/compressed_h264";
    constexpr char CAMERA_IMAGE_NV12[] = "/camera/image/raw_nv12";
    constexpr char CAMERA_IMAGE_QR_RESULT[] = "camera/image/qr_result";

} // namespace topics

#endif // __TOPIC_NAMES_H__