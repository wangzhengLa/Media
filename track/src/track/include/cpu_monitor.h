#ifndef TRACK_CPU_MONITOR_H
#define TRACK_CPU_MONITOR_H

#include "config.h"

// CPU 监控主入口：主循环每轮调用一次，内部按时间节拍决定是否采样，
// 非阻塞（纯读 /proc 与 sysfs，不 sleep、不 fork 外部命令）。
void cpu_monitor_tick(const TrackConfig& cfg);

#endif // TRACK_CPU_MONITOR_H
