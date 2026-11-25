#ifndef TRACK_MEM_MONITOR_H
#define TRACK_MEM_MONITOR_H

#include "config.h"

// 内存监控主入口：主循环每轮调用一次，非阻塞（纯读 /proc/meminfo，不 sleep、不 fork）。
// 告警口径：MemAvailable < mem_available_threshold_mb（绝对阈值，非百分比）。
// 原因：512MB 设备上 OOM 发生在 MemAvailable ~10-20MB，绝对阈值直接映射真实风险边界；
//       /tmp 为 tmpfs，其膨胀会自然拉低 MemAvailable，一并被覆盖。
// 观察式告警：仅日志 + 快照，不自动清理/不重启。
void mem_monitor_tick(const TrackConfig& cfg);

#endif // TRACK_MEM_MONITOR_H
