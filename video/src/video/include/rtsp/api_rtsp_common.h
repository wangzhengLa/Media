#ifndef API_RTSP_COMMON_H_
#define API_RTSP_COMMON_H_

// ============================================================================
// api_rtsp_common.h
//
// 由 AX_Projec/rtsp 适配而来：
//   - 去除 ax_base_type.h / ax_global_type.h 依赖，统一使用 stdint 标准类型
//   - 去除 w_log.h 依赖，统一使用项目 ump 日志（log.h）
// ============================================================================

#include <cstdint>
#include <log.h>

// 日志统一走项目 ump 通道，带 apirtsp 前缀便于过滤
#define API_RTSP_LOGI(fmt, ...) ump_info("[apirtsp] " fmt, ##__VA_ARGS__)
#define API_RTSP_LOGW(fmt, ...) ump_warn("[apirtsp] " fmt, ##__VA_ARGS__)
#define API_RTSP_LOGE(fmt, ...) ump_error("[apirtsp] " fmt, ##__VA_ARGS__)

namespace apirtsp {

// 单通道下的媒体类型
enum ApiRtspMediaType {
    API_RTSP_MEDIA_VIDEO = 0,
    API_RTSP_MEDIA_AUDIO = 1,
    API_RTSP_MEDIA_BUTT  = 2,
};

// 视频最大单帧：与 live555 OutPacketBuffer 上限保持一致（1MB），
// 防止大 IDR（SPS/PPS/IDR 整帧）被截断导致解码器报错。
static const uint32_t API_RTSP_VIDEO_MAX_FRAME_SIZE = 1024 * 1024;

// 音频最大单帧（G711 20ms@8kHz = 160B，留足余量）
static const uint32_t API_RTSP_AUDIO_MAX_FRAME_SIZE = 10 * 1024;

// 每路媒体环形缓冲槽位数（AX 原始实现为 10）
static const uint32_t API_RTSP_RING_ELEMENT_COUNT = 10;

} // namespace apirtsp

#endif // API_RTSP_COMMON_H_
