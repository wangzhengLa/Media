/**
 * @file    video_capture_linux.cpp
 * @brief   AM311x 平台的 VideoCaptureImpl::Create 实现
 *
 * 这个文件是链接时平台选择的关键点。CMake 只编译所选平台目录
 * 下的 video_capture_linux.cpp，每个平台都有同名的
 * VideoCaptureImpl::Create 静态函数，链接时自然只有一份。
 */

#include <cstring>
#include "log.h"
#include "video_capture_am311x.h"
#include "video_capture.h"

namespace ecovacs
{
    namespace videocapturemodule
    {

        VideoCaptureModule *VideoCaptureImpl::Create(const char *deviceUniqueId)
        {
            auto *implementation = new VideoCaptureModuleAm311x();
            if (implementation->Init(deviceUniqueId) != 0) {
                ump_error("VideoCaptureModuleAm311x::Create(%s) Init failed\n",
                          deviceUniqueId ? deviceUniqueId : "null");
                delete implementation;
                return nullptr;
            }
            return implementation;
        }

        VideoCaptureModule *VideoCaptureImpl::Create(int32_t deviceIndex)
        {
            auto *implementation = new VideoCaptureModuleAm311x();
            if (implementation->Init(deviceIndex, -1) != 0) {
                ump_error("VideoCaptureModuleAm311x::Create(%d) Init failed\n", deviceIndex);
                delete implementation;
                return nullptr;
            }
            return implementation;
        }

        VideoCaptureModule *VideoCaptureImpl::Create(int32_t deviceIndex, int32_t csi_id,
                                                      const char *stream_type, const char *image_type)
        {
            auto *implementation = new VideoCaptureModuleAm311x();
            if (implementation->Init(deviceIndex, csi_id, stream_type, image_type) != 0) {
                ump_error("VideoCaptureModuleAm311x::Create(%d, csi=%d, stream=%s, image=%s) Init failed\n",
                          deviceIndex, csi_id, stream_type ? stream_type : "null",
                          image_type ? image_type : "null");
                delete implementation;
                return nullptr;
            }
            return implementation;
        }

    } // namespace videocapturemodule

} // namespace ecovacs
