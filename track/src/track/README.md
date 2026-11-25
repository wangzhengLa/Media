# track

设备上的**进程守护与系统监控**工具。

监控关键业务进程（audio、video）的存活状态，进程异常退出时自动拉起；连续快速崩溃达到阈值时触发系统级重启，保障设备可用性。同时周期性采样整机 / 每核 CPU 负载，超阈值时告警并留存 TOP N 进程快照，便于事后追溯。

## 功能特性

- **进程存活检测**：支持 pidfile + comm 校验，无 pidfile 时回退到 `/proc` 扫描
- **自动拉起**：进程死亡后执行配置的 `restart_cmd` 重新启动
- **快速崩溃熔断**：连续快速崩溃达到阈值（默认 3 次）→ `sync` + `reboot`
- **父子进程模型**：父进程 `waitpid` 监控子进程，子进程崩溃立即重启
- **启动宽限期**：启动后一段时间内只检测不拉起，避免与系统 init 脚本抢服务
- **日志轮转**：按文件大小 + 保留份数轮转，防止日志撑爆 flash
- **心跳日志**：周期性输出所有被监控进程的存活状态和运行时长
- **CPU 负载监控**：基于 `/proc/stat` 差分计算整机 / 每核 CPU%，整机超阈值告警（对称去抖）
- **告警现场快照**：CPU 告警时打印全量信息（每核 / ctxt / 频率 / 温度 / 负载）并拍 TOP N 进程快照，只保留最新一份防刷盘
- **周期概览日志**：正常态周期性输出整机 CPU% + 每核 + 温度 / 频率 / 负载，告警中自动加密输出

## 架构

```
track (父进程, track-parent)
  └── fork
      └── track (子进程, track-child)  ← 实际监控逻辑
            ├── 进程存活检测
            ├── 自动拉起 / 熔断重启
            ├── 心跳日志
            └── CPU 监控：采样 → 告警 → TOP N 快照
```

- **父进程**：`prctl(PR_SET_NAME, "track-parent")`，阻塞在 `waitpid()` 上。子进程退出后记录原因并立即重新 fork。
- **子进程**：`prctl(PR_SET_NAME, "track-child")`，运行主监控循环（进程检测 → 拉起 → 心跳 → CPU 监控 → sleep），每轮间隔 `check_interval`（出厂 1s）。

父进程自身崩溃时由 init 脚本（S55track）或上层监控负责拉起。

## 核心机制

### 进程存活检测

`check_process()` 按以下顺序检测：

1. **pidfile 模式**（配置了 `pidfile`）：
   - 读取 pidfile 获取 pid
   - `kill(pid, 0)` 判断进程是否存在
   - 读 `/proc/<pid>/comm` 与配置的 `comm` 比对，防止 pid 复用误判
   - 校验失败则回退到 /proc 扫描

2. **/proc 扫描模式**（未配置 pidfile）：
   - 遍历 `/proc/*/comm`，匹配 `comm` 字段

### 快速崩溃与熔断

```
进程死亡 → crash_count++ → 执行 restart_cmd
                              │
                              ├─ crash_count < threshold → 正常拉起
                              │
                              └─ crash_count ≥ threshold → sync(); sleep(1); system("reboot")
```

- `crash_count` 仅在进程**稳定运行超过 `rapid_death_window`**（默认 30s）后才重置为 0
- 即：拉起后 30s 内再次死亡 → 算快速崩溃，计数累加；活过 30s → 计数清零

### 启动宽限期

- `startup_grace`（默认 15s）：track 启动后的这段时间内，**只检测进程状态，不执行拉起**
- 目的：给系统 init（如 S99media）留出启动业务进程的时间，避免抢拉

### CPU 监控

`cpu_monitor_tick()` 每轮（`check_interval`，出厂 1s）在主循环中调用，纯读 `/proc` 与 sysfs，不阻塞主循环：

- **采样**：`/proc/stat` 差分算整机 total% 与每核 core%；`/proc/loadavg`、thermal zone 温度、cpufreq 频率、`/proc/<pid>/stat` 进程 tick 同期采集。首轮只建 baseline 不算率，差分窗口恒等于 `check_interval`，整机 total% 与快照进程 CPU% 可对齐诊断。
- **告警**：唯一触发条件为整机 total% ≥ `cpu_total_threshold`，`cpu_alert_duration`（默认 3）次对称去抖（超阈值连续 3 次告警、低于阈值连续 3 次恢复），告警期间记录峰值。
- **快照**：告警时拍全系统 TOP N 进程快照（`cpu_snapshot_<ts>.log`），冷却 `cpu_snapshot_cooldown`（默认 60s），只保留最新一份防 flash 打满。
- **周期概览**：正常态每 `cpu_log_interval`（默认 300s）一条，告警中自动降为 60s 一条，便于观察恢复。


## 配置文件

路径：`/etc/track/track.conf`

### 全局配置

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `log_dir` | `/var/log/track` | 日志目录 |
| `log_size` | `1024` | 单个日志文件大小（KB） |
| `log_count` | `5` | 日志保留份数 |
| `check_interval` | `2` | 存活检测间隔（秒）；CPU 采样 / 扫描 / 快照窗口也跟随此间隔 |
| `heartbeat_interval` | `60` | 心跳日志间隔（秒），0 表示每轮都打 |
| `startup_grace` | `15` | 启动宽限期（秒） |
| `rapid_death_window` | `30` | 快速崩溃判定窗口（秒） |
| `circuit_break_threshold` | `3` | 连续快速崩溃多少次后重启系统 |
| `cpu_monitor_enable` | `1` | 是否开启 CPU 监控 |
| `cpu_log_interval` | `300` | 正常态周期概览日志间隔（秒），告警中自动降为 60s |
| `cpu_total_threshold` | `90` | 整机归一化% 告警阈值（0~100）|
| `cpu_alert_duration` | `3` | 连续几次超阈值才告警 / 恢复（去抖） |
| `cpu_snapshot_on_alert` | `1` | 告警时是否拍 TOP N 进程快照 |
| `cpu_snapshot_cooldown` | `60` | 两次快照最小间隔（秒），防刷盘 |
| `cpu_top_n` | `5` | 快照取 CPU 最高的前 N 个进程 |


### 被监控进程

每个进程一个 `[process:name]` 段：

```ini
[process:video]
pidfile=/var/run/video.pid      # pidfile 路径（可选，空则按 comm 检测）
comm=video                       # /proc/<pid>/comm 匹配的进程名
restart_cmd=/usr/bin/video.sh restart   # 拉起命令
restart_delay=1                  # 拉起后等待秒数

[process:audio]
pidfile=/var/run/audio.pid
comm=audio
restart_cmd=/usr/bin/audio.sh restart
restart_delay=1
```

## 目录结构

```
track/
├── Makefile                  # 构建脚本（fetchSrc / fetchPkg / build / install / PKG）
├── VERSION                   # 版本号（git short hash）
└── src/track/
    ├── include/
    │   ├── config.h          # TrackConfig / ProcessConfig 结构体
    │   ├── cpu_monitor.h     # CPU 监控接口
    │   ├── logger.h          # 日志器（带轮转）
    │   └── process.h         # 进程检测与拉起接口
    ├── src/
    │   ├── config.cpp        # INI 配置解析
    │   ├── cpu_monitor.cpp   # CPU 采样 / 告警 / TOP N 快照
    │   ├── logger.cpp        # 日志实现（大小 + 份数轮转）
    │   ├── process.cpp       # 进程检测（pidfile + /proc 扫描）与拉起
    │   └── track.cpp         # 主程序：fork + waitpid + 监控循环
    └── conf/
        ├── track.conf        # 配置文件
        └── S55track          # init 脚本（start/stop/restart/status）
```

## 编译

使用 MR536 交叉编译工具链（aarch64-openwrt-linux-gnu-g++），C++17。

```bash
cd track
make build          # 输出：build/track
make install       # 安装到 /usr/bin/
```


启动：

```bash
/etc/init.d/S55track start
```

## 日志

日志文件位于 `/var/log/track/track.log`，按大小轮转：

```
track.log
track.log.1
track.log.2
...
```

CPU 告警时留存的进程快照另存为 `cpu_snapshot_<ts>.log`（同一目录），只保留最新一份，避免刷爆 flash。

日志内容示例：

```
[2026-11-17 10:00:00] [INFO] track start: log_size=1024KB log_count=5
[2026-11-17 10:00:00] [INFO] track child start
[2026-11-17 10:00:00] [INFO] watch: name=video pidfile=/var/run/video.pid comm=video
[2026-11-17 10:00:00] [INFO] watch: name=audio pidfile=/var/run/audio.pid comm=audio
[2026-11-17 10:00:00] [INFO] startup grace: 15s (no restart during this period)
[2026-11-17 10:00:00] [INFO] cpu monitor: ncpu=4 hz=100 page=4KB sample=1s snap_window=1s total_thr=85%
[2026-11-17 10:01:00] [INFO] heartbeat: video=alive(pid=1234) audio=alive(pid=1235) uptime=60s
[2026-11-17 10:02:00] [WARN] audio dead, restarting (crash_count=1)
[2026-11-17 10:02:01] [INFO] audio restarted, new pid=1300
[2026-11-17 10:05:00] [INFO] cpu: total=12.3%(整机) cpu0=8.0 cpu1=15.0 cpu2=10.0 cpu3=16.0 temp=40.0C freq=1.80/1.80GHz gov=schedutil load=0.20/0.15/0.10 ctxt=210/s run=2/blk=0 ncpu=4
[2026-11-17 10:06:00] [WARN] cpu: ALERT total=88.5% total_thr=85% load=2.10/1.20/0.50 freq=1.80/1.80GHz gov=schedutil ncpu=4
[2026-11-17 10:06:00] [WARN] cpu: DETAIL total=88.5% thr=85% freq=1.80/1.80GHz gov=schedutil load=2.10/1.20/0.50 ctxt=380/s running=5 blocked=0 ncpu=4 uptime=360s
[2026-11-17 10:06:00] [WARN] cpu: DETAIL cores: cpu0=98.0 cpu1=76.0 cpu2=95.0 cpu3=85.0
[2026-11-17 10:06:00] [WARN] cpu: DETAIL temp_cpu=52.0C temp_npu=45.0C temp_ddr=43.0C hz=100 page=4KB
[2026-11-17 10:06:00] [INFO] cpu: snapshot saved=/var/log/track/cpu_snapshot_12345.log top=5 val=88.5%
[2026-11-17 10:07:00] [INFO] cpu: total=6.0%(整机) cpu0=4.0 cpu1=5.0 cpu2=8.0 cpu3=7.0 temp=41.0C freq=1.80/1.80GHz gov=schedutil load=0.30/1.00/0.60 ctxt=200/s run=2/blk=0 ncpu=4
[2026-11-17 10:07:03] [INFO] cpu: RECOVERED total=6.0% thr=85% peak=88.5%
```
