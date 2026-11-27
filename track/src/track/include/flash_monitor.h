#ifndef TRACK_FLASH_MONITOR_H
#define TRACK_FLASH_MONITOR_H

#include "config.h"

// Flash 监控主入口：主循环每轮调用一次，非阻塞（纯 statvfs，不 sleep、不 fork）。
void flash_monitor_tick(const TrackConfig& cfg);

#endif // TRACK_FLASH_MONITOR_H
