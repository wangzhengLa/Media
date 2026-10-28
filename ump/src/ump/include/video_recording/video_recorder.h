#ifndef UMP_VIDEO_RECORDING_VIDEO_RECORDER_H_
#define UMP_VIDEO_RECORDING_VIDEO_RECORDER_H_

#include <cstdint>
#include <string>
#include <mutex>

namespace ump {
namespace video_recording {

/**
 * @brief 基于 mp4v2 封装的 MP4 录像器
 *
 * 接收 H.264 裸流（Annex-B 格式，带 00 00 00 01 起始码），
 * 自动解析 SPS/PPS 并写入 MP4 文件。
 */
class Mp4Recorder
{
public:
    Mp4Recorder();
    ~Mp4Recorder();

    /**
     * @brief 开始录像，创建 MP4 文件并添加 H.264 track
     *
     * @param filename 输出文件路径
     * @param width    视频宽度
     * @param height   视频高度
     * @param fps      帧率
     * @return 0 成功，<0 失败
     */
    int Start(const std::string &filename, int width, int height, int fps);

    /**
     * @brief 写入一帧 H.264 数据
     *
     * @param data   H.264 NALU 数据（Annex-B 格式，带 00 00 00 01 起始码）
     * @param size   数据长度
     * @param is_idr 是否为 IDR 帧
     * @return 0 成功，<0 失败
     */
    int WriteH264Frame(const uint8_t *data, size_t size, bool is_idr);

    /**
     * @brief 停止录像，关闭 MP4 文件
     */
    void Stop();

    /**
     * @brief 是否正在录像
     */
    bool IsRecording() const { return mp4_file_ != nullptr; }

private:

    void *mp4_file_;       // MP4FileHandle
    int    video_track_id_;
    int    width_;
    int    height_;
    int    fps_;
    bool   sps_pps_set_;   // 是否已设置 SPS/PPS

    std::mutex mutex_;
};

} // namespace video_recording
} // namespace ump

#endif // UMP_VIDEO_RECORDING_VIDEO_RECORDER_H_
