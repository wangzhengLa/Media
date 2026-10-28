#include "video_recorder.h"

#include <mp4v2/mp4v2.h>

#include <vector>
#include <cstring>

#include "log.h"

namespace ump {
namespace video_recording {

// NALU 解析用的结构体
struct NaluUnit
{
    int type;
    int size;
    const unsigned char *data;
};

Mp4Recorder::Mp4Recorder()
    : mp4_file_(nullptr)
    , video_track_id_(MP4_INVALID_TRACK_ID)
    , width_(0)
    , height_(0)
    , fps_(0)
    , sps_pps_set_(false)
{
}

Mp4Recorder::~Mp4Recorder()
{
    Stop();
}

int Mp4Recorder::Start(const std::string &filename, int width, int height, int fps)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (mp4_file_) {
        ump_error("Mp4Recorder: already recording\n");
        return -1;
    }

    if (filename.empty() || width <= 0 || height <= 0 || fps <= 0) {
        ump_error("Mp4Recorder: invalid params filename=%s %dx%d@%d\n",
                  filename.c_str(), width, height, fps);
        return -1;
    }

    MP4FileHandle hFile = MP4Create(filename.c_str());
    if (hFile == MP4_INVALID_FILE_HANDLE) {
        ump_error("Mp4Recorder: MP4Create failed: %s\n", filename.c_str());
        return -1;
    }

    // 设置 timescale（90000），track 在收到 SPS 时再创建
    MP4SetTimeScale(hFile, 90000);

    mp4_file_     = hFile;
    video_track_id_ = MP4_INVALID_TRACK_ID;
    width_        = width;
    height_       = height;
    fps_          = fps;
    sps_pps_set_  = false;

    ump_info("Mp4Recorder: started %s %dx%d@%dfps (track will be created on SPS)\n",
             filename.c_str(), width, height, fps);
    return 0;
}

// 从 Annex-B buffer 中读取一个 NALU
// 返回本次读取消耗的字节数，0 表示没有更多 NALU
// 兼容 3 字节（00 00 01）和 4 字节（00 00 00 01）起始码
static int ReadOneNaluFromBuf(const unsigned char *buffer, unsigned int buf_size,
                              unsigned int offset, NaluUnit &nalu)
{
    unsigned int i = offset;
    while (i + 2 < buf_size) {
        // 检测起始码：4 字节 (00 00 00 01) 或 3 字节 (00 00 01)
        int start_code_len = 0;
        if (i + 3 < buf_size &&
            buffer[i] == 0x00 && buffer[i + 1] == 0x00 &&
            buffer[i + 2] == 0x00 && buffer[i + 3] == 0x01) {
            start_code_len = 4;
        } else if (buffer[i] == 0x00 && buffer[i + 1] == 0x00 &&
                   buffer[i + 2] == 0x01) {
            start_code_len = 3;
        }

        if (start_code_len > 0) {
            unsigned int nalu_start = i + start_code_len;
            // 找下一个起始码
            unsigned int pos = nalu_start;
            int next_start_len = 0;
            while (pos + 2 < buf_size) {
                if (pos + 3 < buf_size &&
                    buffer[pos] == 0x00 && buffer[pos + 1] == 0x00 &&
                    buffer[pos + 2] == 0x00 && buffer[pos + 3] == 0x01) {
                    next_start_len = 4;
                    break;
                } else if (buffer[pos] == 0x00 && buffer[pos + 1] == 0x00 &&
                           buffer[pos + 2] == 0x01) {
                    next_start_len = 3;
                    break;
                }
                pos++;
            }
            if (pos + 2 >= buf_size) {
                nalu.size = (int)(buf_size - nalu_start);
            } else {
                nalu.size = (int)(pos - nalu_start);
            }
            if (nalu.size <= 0) {
                return 0;
            }
            nalu.type = buffer[nalu_start] & 0x1F;
            nalu.data = &buffer[nalu_start];
            return (int)(nalu_start + nalu.size - offset);
        }
        i++;
    }
    return 0;
}

int Mp4Recorder::WriteH264Frame(const uint8_t *data, size_t size, bool is_idr)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!mp4_file_) {
        ump_error("Mp4Recorder: not started\n");
        return -1;
    }

    if (!data || size == 0) {
        return -1;
    }

    MP4FileHandle hFile = (MP4FileHandle)mp4_file_;
    uint32_t timescale = 90000;
    MP4Duration sample_duration = timescale / fps_;

    NaluUnit nalu;
    unsigned int pos = 0;
    int consumed;

    while ((consumed = ReadOneNaluFromBuf(data, (unsigned int)size, pos, nalu)) > 0) {
        if (nalu.type == 0x07) {
            // SPS：创建 H264 track，使用 SPS 中的 profile/level
            if (video_track_id_ == MP4_INVALID_TRACK_ID) {
                video_track_id_ = MP4AddH264VideoTrack(
                    hFile,
                    timescale,
                    sample_duration,
                    (uint16_t)width_,
                    (uint16_t)height_,
                    nalu.data[1], // AVCProfileIndication
                    nalu.data[2], // profile_compat
                    nalu.data[3], // AVCLevelIndication
                    3);           // 4 bytes length prefix

                if (video_track_id_ == MP4_INVALID_TRACK_ID) {
                    ump_error("Mp4Recorder: MP4AddH264VideoTrack failed\n");
                    return -1;
                }
                MP4SetVideoProfileLevel(hFile, 0x01); // Simple Profile @ Level 3
                ump_info("Mp4Recorder: track created profile=0x%02x level=0x%02x\n",
                         nalu.data[1], nalu.data[3]);
            }
            MP4AddH264SequenceParameterSet(hFile, video_track_id_, nalu.data, (uint16_t)nalu.size);
        }
        else if (nalu.type == 0x08) {
            // PPS
            if (video_track_id_ != MP4_INVALID_TRACK_ID) {
                MP4AddH264PictureParameterSet(hFile, video_track_id_, nalu.data, (uint16_t)nalu.size);
                sps_pps_set_ = true;
            }
        }
        else {
            // 视频帧（IDR/P）：去掉起始码，加 4 字节长度前缀
            if (video_track_id_ == MP4_INVALID_TRACK_ID) {
                ump_warn("Mp4Recorder: video frame before SPS, skip\n");
                pos += consumed;
                continue;
            }

            int datalen = nalu.size + 4;
            std::vector<uint8_t> buf(datalen);
            buf[0] = (uint8_t)(nalu.size >> 24);
            buf[1] = (uint8_t)(nalu.size >> 16);
            buf[2] = (uint8_t)(nalu.size >> 8);
            buf[3] = (uint8_t)(nalu.size & 0xff);
            memcpy(buf.data() + 4, nalu.data, nalu.size);

            bool sync_sample = (nalu.type == 5); // IDR
            if (!MP4WriteSample(hFile, video_track_id_, buf.data(), datalen,
                                MP4_INVALID_DURATION, 0, sync_sample)) {
                ump_error("Mp4Recorder: MP4WriteSample failed\n");
                return -1;
            }
        }

        pos += consumed;
    }

    return (int)pos;
}

void Mp4Recorder::Stop()
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (mp4_file_) {
        MP4Close((MP4FileHandle)mp4_file_);
        ump_info("Mp4Recorder: stopped\n");
    }
    mp4_file_      = nullptr;
    video_track_id_ = MP4_INVALID_TRACK_ID;
    sps_pps_set_   = false;
}


} // namespace video_recording
} // namespace ump
