#ifndef TRACK_FLASH_MONITOR_H
#define TRACK_FLASH_MONITOR_H

#include "config.h"

// Flash 监控主入口：主循环每轮调用一次，非阻塞（纯 statvfs，不 sleep、不 fork）。
// 告警口径：已用百分比 used% = (blocks - bavail) / blocks * 100，超过 flash_used_pct_threshold 告警。
//   - 用 f_bavail（非 root 可用）而非 f_bfree，反映实际可写空间。
//   - 按 f_fsid 去重：同一文件系统的多个路径只告警一次（如 overlay 路径与其上层同 fs），
//     但周期概览仍逐路径打印。
//   - 只读分区（f_flag & ST_RDONLY）自动跳过告警（如 squashfs 的 /），仅在概览中标注。
// 去抖：flash_alert_duration（默认 2，闪存变化慢）。
// 观察式：仅日志（告警时统计触发分区一级子项的占用 du，取前 N 写入主日志），不自动清理/不重启。
// 统一写入 track.log；cooldown 按 f_fsid 独立。
void flash_monitor_tick(const TrackConfig& cfg);

#endif // TRACK_FLASH_MONITOR_H
