# track

设备上的**进程守护与系统资源监控**工具。

监控关键业务进程（audio、video）的存活状态，进程异常退出时自动拉起；连续快速崩溃达到阈值时触发系统级重启，保障设备可用性。同时周期性采样 CPU / 内存 / Flash 资源，超阈值时告警并留存 TOP N 现场快照；全量采集内核日志（dmesg）到独立文件，便于事后追溯。

## 功能特性

- **进程存活检测**：支持 pidfile + comm 校验，无 pidfile 时回退到 `/proc` 扫描
- **自动拉起**：进程死亡后执行配置的 `restart_cmd` 重新启动
- **快速崩溃熔断**：连续快速崩溃达到阈值（默认 3 次）→ `sync` + `reboot`
- **父子进程模型**：父进程 `waitpid` 监控子进程，子进程崩溃立即重启；子进程 fork 后重新打开独立日志 fd
- **日志轮转**：按文件大小 + 保留份数轮转，防止日志撑爆 flash；`track.log` 与 `dmesg.log` 各自独立轮转、共用同一套大小/份数参数；父子进程 fd 在轮转后自动自愈对齐
- **CPU 监控**：基于 `/proc/stat` 差分计算整机 CPU%，整机超阈值即告警（无去抖）
- **内存监控**：基于 `/proc/meminfo` 的 MemAvailable，低于阈值即告警（无去抖）
- **Flash 监控**：基于 `statvfs` 的分区使用率，按文件系统去重、超阈值即告警（只读分区自动跳过）
- **内核日志采集**：全量采集 `/proc/kmsg` 到独立轮转文件 `dmesg.log`，不过滤、不丢弃；打开失败后 30s 退避重试
- **告警现场快照**：CPU / 内存 / Flash 告警时打印告警摘要并拍 TOP N 快照，**内联写入主日志 `track.log`**，不另建文件；告警持续期间按冷却重打快照，恢复时打一条恢复日志
- **正常态静默**：启动时各模块打印一行初始状态，之后正常运行不输出周期概览，只在告警时产生日志

## 架构

```
track (父进程, track-parent)
  └── fork
      └── track (子进程, track-child)  ← 实际监控逻辑
            ├── 进程存活检测 / 自动拉起 / 熔断重启
            ├── CPU 监控：采样 → 告警 → TOP N 快照（内联主日志）
            ├── 内存监控：采样 → 告警 → RSS TOP N 快照（内联主日志）
            ├── Flash 监控：采样 → 告警 → 一级子项 TOP N 快照（内联主日志）
            └── 内核日志采集：/proc/kmsg → dmesg.log
```

- **父进程**：`prctl(PR_SET_NAME, "track-parent")`，阻塞在 `waitpid()` 上。子进程退出后记录原因（`ERROR` 级别）并立即重新 fork。
- **子进程**：`prctl(PR_SET_NAME, "track-child")`，fork 后重新 `Logger::init` 打开独立日志 fd，运行主监控循环（进程检测 → 拉起 → CPU → 内存 → Flash → dmesg → sleep），每轮间隔 `check_interval`（出厂 1s）。
- 父进程自身崩溃时由 init 脚本（S99track）或上层监控负责拉起。

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

启动顺序依赖：S99track 在 init 脚本中字典序排在 S99media 之后，业务进程（video/audio）由 S99media 先拉起，track 启动时直接进入存活检测，无需启动宽限期。

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

### 告警评估

`eval_alert(st, over, val)` 统一告警边沿检测，三个监控共用：

- **超阈值第 1 个 tick** 即触发告警（无连续次数要求）
- **低于阈值第 1 个 tick** 即恢复告警（对称，无恢复延迟）
- 告警期间记录极值（CPU/Flash 记峰值 used%，内存记 MemAvailable 最小值），恢复日志打印该极值

> 设备资源变化由真实负载驱动，单次采样抖动概率低，故不设去抖以保证告警实时性。

### CPU 监控

`cpu_monitor_tick()` 每轮（`check_interval`，出厂 1s）在主循环中调用，纯读 `/proc` 与 sysfs：

- **采样**：`/proc/stat` 差分算整机 total%；cpufreq 频率、thermal zone 温度、`/proc/<pid>/stat` 进程 tick 同期采集。首轮只建 baseline 不算率，差分窗口恒等于 `check_interval`，整机 total% 与快照进程 CPU% 可对齐诊断。
- **告警**：唯一触发条件为整机 total% ≥ `cpu_total_threshold`，无去抖（1 次即告警 / 恢复），告警期间记录峰值。
- **快照**：告警时拍全系统 TOP N 进程快照，**内联写入 `track.log`**（前缀 `cpu: SNAPSHOT` / `cpu: SNAP`），不生成独立文件；告警持续期间按 `snapshot_cooldown`（默认 5s）重打一次，恢复后停。
- 进程 CPU% 依赖 baseline，每轮 `scan_all` 覆盖 baseline；读不到 `/proc/<pid>/stat` 的进程本次跳过（不进快照），下个 tick 自动重试。

### 内存监控

`mem_monitor_tick()` 每轮调用，纯读 `/proc/meminfo`：

- **告警指标**：`MemAvailable`（绝对值 MB），低于 `mem_available_threshold_mb` 告警。用 MemAvailable 而非 MemFree——它是内核估算的"可立即分配"内存（含可回收 page cache），更贴近 OOM 边界。
- **无去抖**：低于阈值第 1 个 tick 即告警，恢复第 1 个 tick 即解除；告警期间记录 MemAvailable 最小值，恢复日志打印该最小值。
- **快照**：告警时扫描 `/proc` 一次，按 RSS 降序取 TOP N，**内联写入 `track.log`**（前缀 `mem: SNAPSHOT` / `mem: SNAP`）。RSS 是瞬时值无需 baseline，只在快照时扫描，稳态零额外开销；告警持续期间按 `snapshot_cooldown`（默认 5s）重打。
- 内存为瞬时值（非差分），首轮即可参与告警，不需要建 baseline。

### Flash 监控

`flash_monitor_tick()` 每轮调用，分区整体用 `statvfs`，快照时进程内递归遍历：

- **告警指标**：分区 `used% > flash_used_pct_threshold`（用 `f_bavail` 计算，反映非 root 真实可写空间）。
- **按文件系统去重**：同一 `f_fsid` 只告警一次；只读分区（`ST_RDONLY`，如 squashfs 的 `/`）自动跳过。多个不同 f_fsid 的分区（如 `/userdata`、`/oem`）各自独立告警 / 恢复 / 快照，快照冷却按 `f_fsid` 独立。
- **无去抖**：超阈值第 1 个 tick 即告警，恢复第 1 个 tick 即解除；告警期间记录 used% 峰值。
- **快照**：告警时统计触发分区挂载目录下**一级子项**的占用（进程内递归 `lstat` 累加表观大小，等价 `du -sbx`，跨挂载点跳过），取最大的前 N 个，**内联写入 `track.log`**（前缀 `flash: SNAPSHOT` / `flash: SNAP`），快速定位"哪个大目录占满了分区"。大小单位 MB 保留 1 位小数。

### 内核日志采集（dmesg）

`dmesg_monitor_tick()` 每轮调用，把 `/proc/kmsg` **全量采集**到独立文件 `dmesg.log`（始终开启，无配置开关）：

- **增量读取**：`/proc/kmsg` 带全局消费游标，`open` 后首次 `drain` 把开机以来全部日志 dump 进去，之后每轮 `read` 拿到的就是增量，无需自己记录偏移。非阻塞读（`O_NONBLOCK`），没数据立刻 `EAGAIN` 不阻塞主循环。
- **半行拼接保护**：非阻塞 read 可能返回半行，维护拼接缓冲，只把以 `\n` 结尾的完整行写出，避免一行被拆到两个文件（轮转边界）；单行超长（>64KB）强制 flush 防内存膨胀。
- **退避重试**：首次 `open /proc/kmsg` 失败（如被 klogd 临时占用，`EBUSY`）后，每 30s 重试一次，成功即恢复采集，不永久放弃；子进程崩溃重启时重置为重试干净状态。
- **不做过滤 / 不去重**：用户明确选择全量保留，OOM / IO error / segfault 等任何内核异常都可事后追溯。
- **轮转**：复用与 `track.log` 相同的 `log_size` / `log_count` 参数，独立轮转，历史备份同样 gzip 压缩。`dmesg.log` 原样写入内核日志（不带 track.log 的时间戳 / 级别前缀），与 `dmesg` 命令输出一致。
- **注意**：`/proc/kmsg` 是单消费者，track 占用后不要再 `cat /proc/kmsg`（会共享游标导致双方数据不完整）；查内核日志用 `dmesg` 命令或读 `dmesg.log`。

### 日志轮转与多进程 fd 隔离

`RotatingFile` 通用类实现按大小轮转，`track.log`（logger）与 `dmesg.log`（dmesg_monitor）各持一个实例：

- 写入用 `O_APPEND` 追加，不覆盖已有内容。
- 写满 `log_size` 时：close → 依次 rename 移位（`.1.gz→.2.gz…`），删除最旧的 `.gz` → 当前文件改名为 `.1` → 新建空文件继续写 → 把 `.1` gzip 压缩为 `.1.gz`。当前文件始终明文（可 `tail -f`），仅历史备份压缩；gzip 失败时该份保留明文，不丢数据。最多保留 `log_count` 份。
- **父子进程 fd 自愈**：track 是父子进程模型，fork 后子进程重新 `Logger::init` 打开独立 fd；每次写前 `RotatingFile` 会比对自身 fd 的 inode 与当前路径 inode，若不一致（对方进程刚完成轮转）则自动 close + open 重开，保证日志始终落到当前 `track.log` 而非已轮转走的旧文件。`current_size_` 每次按 `fstat` 取真实大小，跨进程写入时轮转阈值不漂移。

## 配置文件

路径：`/etc/track/track.conf`

### 全局配置

| 字段 | 默认值 | 出厂值 | 说明 |
|------|--------|--------|------|
| `log_dir` | `/var/log/track` | `/var/log/track` | 日志目录 |
| `log_size` | `1024` | `1024` | 单个日志文件大小（KB）；track.log 与 dmesg.log 共用 |
| `log_count` | `5` | `5` | 日志保留份数；track.log 与 dmesg.log 共用 |
| `check_interval` | `2` | `1` | 主循环间隔（秒）；CPU 采样 / 扫描 / 快照窗口也跟随此间隔 |
| `rapid_death_window` | `30` | `30` | 快速崩溃判定窗口（秒），拉起后此时间内死亡算快速崩溃 |
| `circuit_break_threshold` | `3` | `3` | 连续快速崩溃多少次后重启系统 |
| `snapshot_cooldown` | `5` | `5` | 告警期间快照重打间隔（秒），cpu/mem/flash 共用；flash 按 f_fsid 独立计时，防刷盘 |
| `snapshot_top_n` | `5` | `5` | 快照取前 N 项（CPU/RSS 进程、flash 一级子项），cpu/mem/flash 共用 |

### CPU 配置

| 字段 | 默认值 | 出厂值 | 说明 |
|------|--------|--------|------|
| `cpu_total_threshold` | `90` | `85` | 整机归一化% 告警阈值（0~100），唯一告警条件 |

### 内存配置

| 字段 | 默认值 | 出厂值 | 说明 |
|------|--------|--------|------|
| `mem_available_threshold_mb` | `40` | `40` | MemAvailable 低于此值（MB）告警；512MB 设备 OOM 边界约 10-20MB |

### Flash 配置

| 字段 | 默认值 | 出厂值 | 说明 |
|------|--------|--------|------|
| `flash_paths` | `/userdata /oem` | `/userdata /oem` | 监控路径（空格分隔）；同 f_fsid 只告警一次，只读分区自动跳过 |
| `flash_used_pct_threshold` | `90` | `90` | 已用百分比告警阈值（0~100，按非 root 可用空间 f_bavail 计算） |

### 被监控进程

每个进程一个 `[process:name]` 段：

```ini
[process:video]
pidfile=/var/run/video.pid      # pidfile 路径（可选，空则按 comm 检测）
comm=video                       # /proc/<pid>/comm 匹配的进程名
restart_cmd=/usr/bin/video.sh restart   # 拉起命令
restart_delay=1                  # 拉起后等待秒数，再校验是否拉起成功

[process:audio]
pidfile=/var/run/audio.pid
comm=audio
restart_cmd=/usr/bin/audio.sh restart
restart_delay=1
```

## 目录结构

```
track/
├── Makefile                      # 构建脚本（BOARD=mr536_sz2537 交叉编译）
├── VERSION                       # 版本号（git short hash）
└── src/track/
    ├── include/
    │   ├── config.h              # TrackConfig / ProcessConfig 结构体
    │   ├── logger.h              # 日志器（委托 RotatingFile 轮转）
    │   ├── rotating_file.h       # 通用按大小轮转文件类（含跨进程 fd 自愈）
    │   ├── monitor_common.h      # 告警状态机 + read_pid_stat + 时钟/裁剪工具
    │   ├── process_monitor.h     # 进程检测与拉起接口
    │   ├── cpu_monitor.h         # CPU 监控接口
    │   ├── mem_monitor.h         # 内存监控接口
    │   ├── flash_monitor.h       # Flash 监控接口
    │   └── dmesg_monitor.h       # 内核日志采集接口
    ├── src/
    │   ├── config.cpp            # INI 配置解析
    │   ├── logger.cpp            # 日志实现
    │   ├── rotating_file.cpp     # 轮转实现（track.log / dmesg.log 共用）
    │   ├── process_monitor.cpp   # 进程检测（pidfile + /proc 扫描）与拉起
    │   ├── cpu_monitor.cpp       # CPU 采样 / 告警 / TOP N 快照
    │   ├── mem_monitor.cpp       # 内存采样 / 告警 / RSS TOP N 快照
    │   ├── flash_monitor.cpp     # Flash 采样 / 告警 / 一级子项 TOP N 快照
    │   ├── dmesg_monitor.cpp     # /proc/kmsg 全量采集 → dmesg.log（失败退避重试）
    │   └── track.cpp             # 主程序：fork + waitpid + 监控循环
    └── conf/
        ├── track.conf            # 配置文件
        └── S99track              # init 脚本（start/stop/restart/status）
```

## 构建

```sh
make            # 交叉编译，产物 build/track
make clean
make install    # 安装到 staging 目录（含 S99track 与 track.conf）
```

## 日志

日志位于 `/var/log/track/`：

```
track.log        track.log.1  ...  track.log.4   # 主日志（含告警快照），轮转最多 5 份
dmesg.log        dmesg.log.1  ...  dmesg.log.4   # 内核日志全量采集，轮转最多 5 份
```

- `track.log`：进程事件 + 启动摘要 + CPU / 内存 / Flash 告警与快照。告警快照（`cpu:` / `mem:` / `flash:` 的 `SNAPSHOT` / `SNAP` 行）内联写在主日志里，不另建文件。
- `dmesg.log`：内核日志原样写入，无 track.log 的时间戳 / 级别前缀。
- 日志前缀时间戳用本地时间（`CLOCK_REALTIME`）；设备无 RTC 时 wall clock 停在 1970，不影响功能——内部计时（退避、冷却、崩溃窗口）全用 `CLOCK_MONOTONIC`。

### 日志行为

- **启动**：各模块打印一行初始状态（`INFO`），之后正常运行**静默**。
- **告警**：第 1 行提示（`WARN`）→ 第 2 行告警摘要（`WARN`）→ 紧接 TOP N 快照（`WARN`）；告警未恢复期间按冷却重打快照。
- **恢复**：告警解除时打印一行恢复日志（`INFO`），含告警期间极值，之后停止快照。
- **进程事件**：死亡 / 拉起失败 / 熔断为 `ERROR`；拉起成功为 `INFO`。
- **父子进程**：子进程异常退出时父进程打 `ERROR` 日志并 respawn。

### 日志内容示例

```
[1970-01-01 08:00:00.000] [INFO]  track start
[1970-01-01 08:00:00.000] [INFO]  track child start
[1970-01-01 08:00:00.000] [INFO]  cpu: total=0.0% thr=85% freq=1.80GHz gov=schedutil ncpu=4 temp=36.6C
[1970-01-01 08:00:00.000] [INFO]  mem: avail=320.5MB thr=40MB total=481.0MB used=33.4%
[1970-01-01 08:00:00.000] [INFO]  flash: /userdata used=45.2% (220MB/487MB avail=267MB) thr=90%
[1970-01-01 08:00:00.000] [INFO]  flash: /oem used=12.0% (30MB/250MB avail=220MB) thr=90%
[1970-01-01 08:00:00.000] [INFO]  dmesg: capturing kernel log to /var/log/track/dmesg.log
（正常运行期间无输出）

[1970-01-01 08:05:12.456] [ERROR] process 'video' died: last_pid=1234 crash_count=1
[1970-01-01 08:05:13.678] [INFO]  'video' restarted: pid=1289 downtime=1.0s

[1970-01-01 08:06:03.597] [WARN]  cpu: CPU usage has exceeded the threshold
[1970-01-01 08:06:03.597] [WARN]  cpu: total=75.6% thr=85% freq=1.80GHz gov=schedutil ncpu=4 temp=36.6C
[1970-01-01 08:06:03.597] [WARN]  cpu: SNAPSHOT top=5 val=75.6%
[1970-01-01 08:06:03.597] [WARN]  cpu: SNAP rank pid    comm             st cpu%      user%    sys%     norm%    threads rssKB
[1970-01-01 08:06:03.597] [WARN]  cpu: SNAP 1    1234   sh               R      100.0     98.0      2.0     25.0       1   512
[1970-01-01 08:07:03.874] [INFO]  cpu: RECOVERED total=2.2% thr=85% peak=75.6%

[1970-01-01 08:10:00.000] [WARN]  mem: memory usage has exceeded the threshold
[1970-01-01 08:10:00.000] [WARN]  mem: avail=35.2MB thr=40MB total=481.0MB used=92.7%
[1970-01-01 08:10:00.000] [WARN]  mem: SNAPSHOT uptime=600s avail=35.2MB total=481.0MB used=92.7% thr=40MB top=5
[1970-01-01 08:10:00.000] [WARN]  mem: SNAP rank pid    comm             st rssMB    threads
[1970-01-01 08:10:00.000] [WARN]  mem: SNAP 1    1234   sh               R      120.5       1
[1970-01-01 08:10:30.000] [INFO]  mem: RECOVERED avail=120.5MB thr=40MB min=30.1MB

[1970-01-01 08:20:00.000] [WARN]  flash: flash usage has exceeded the threshold
[1970-01-01 08:20:00.000] [WARN]  flash: path=/userdata used=92.5% thr=90% (451MB/487MB avail=36MB)
[1970-01-01 08:20:00.000] [WARN]  flash: SNAPSHOT trigger=/userdata used=92.5% thr=90% (451MB/487MB avail=36MB) top=5
[1970-01-01 08:20:00.000] [WARN]  flash: SNAP rank sizeMB    path
[1970-01-01 08:20:00.000] [WARN]  flash: SNAP 1        200.5 /userdata/var
[1970-01-01 08:21:00.000] [INFO]  flash: RECOVERED path=/userdata used=80.1% thr=90% peak=92.5%
```

子进程崩溃时父进程日志：

```
[1970-01-01 08:30:00.000] [ERROR] child killed by signal 11, respawning
```
