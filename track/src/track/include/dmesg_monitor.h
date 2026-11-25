#ifndef TRACK_DMESG_MONITOR_H
#define TRACK_DMESG_MONITOR_H

#include "config.h"

// 内核日志采集：读 /proc/kmsg（O_NONBLOCK），全量原样写入 log_dir/dmesg.log。
//   首次调用：打开 /proc/kmsg 与 dmesg.log，把当前内核缓冲全部 dump 进去；
//   之后每次调用：读取自上次以来的增量，追加到 dmesg.log（无增量则不动）。
// 轮转复用 track.conf 的 log_size / log_count，规则与 track.log 完全一致。
// 注意：/proc/kmsg 是单消费者接口，track 读取后勿再 cat /proc/kmsg（改用 dmesg 或看 dmesg.log）。
void dmesg_monitor_tick(const TrackConfig& cfg);
void dmesg_monitor_close();

#endif // TRACK_DMESG_MONITOR_H
