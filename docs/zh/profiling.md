# Profiler 工作流

[跨服与过载保护](./cross-server)中的 profiler 路线决定已经定调：从进程外部对运行中的节点采样。
放在 Asio 循环内部的采样器花的是它自己要测量的那份 CPU 预算，因此无法测出自己的开销。本页是可直接执行的
操作手册——访问路径、可重复压测场景、录制与符号化，以及在开发机上实测的采样影响。

## 访问路径：为什么用 `sudo perf`

开发机上 `kernel.perf_event_paranoid=4`，无 `CAP_PERFMON` 的进程无法录制事件；`kernel.yama.ptrace_scope=1`
又挡住了 strace/gdb 对非子进程的附加。特权路径仍然可用：持有 `CAP_PERFMON` 的进程（sudo）豁免于
`perf_event_paranoid`，因此 `sudo perf record` 可以对普通用户启动的节点开事件（2026-11-15 在
perf 7.0.14、kernel 7.0.0-43 上验证）。需要的就是 sudo 下的 `perf stat` / `perf record`：不用改任何
sysctl，不用给二进制加 capability。

## 可重复压测场景

与[跨服与过载保护](./cross-server)的跨服基准同一拓扑：一个承载脚本的 crossServer，前面一个 cross
网关，用随仓生成器同时驱动两层。

```
mistlane --role crossServer --listen 127.0.0.1:7210 \
  --script examples/cross.lua --metrics-listen 127.0.0.1:7213 &
mistlane --role gateway --routing cross --listen 127.0.0.1:7211 \
  --upstream 127.0.0.1:7210 --metrics-listen 127.0.0.1:7214 &
mistlane bot --url tcp://127.0.0.1:7211 \
  --join "MLC1 1 realm g bench" --clients 32 --count 500
```

生成器是闭环的（32 条串行连接），吞吐与单请求分位数直接取自 bot 的汇总行；节点侧看
`mistlane_event_loop_lag_max_seconds` 和 RSS（轮询 `/proc/PID/status`）。`--join` 每条连接执行一次
`MLC1` 握手并期望 `MLC1 OK 1`（src/bot.cpp）；`--clients` 要低于启动时打印的准入上限。

可比性纪律：

- 生成器参数、脚本与拓扑必须完全一致，否则数字就是噪声（见[跨服与过载保护](./cross-server)）。
- 每个进程都要绑核：两个节点都是单线程，crossServer、网关、bot 分别 `taskset -c 2/3/4`，
  把调度迁移这个变量消掉。
- 跑之前看负载门。在本共享机上，同一次扫描曾出现：两个 baseline 臂在协作席位的构建把 loadavg 推到
  169（14 核）时吞吐相差 3.2 倍，另有两臂的节点中途被压死——被饿死的节点会触发自身的准入与超时上界，
  客户端看到的是 `TCP: End of file`。只在同一次扫描、负载相当的条件下比较各臂；中途死掉的臂必须重跑。

## 录制与符号化

```
sudo perf record -e cycles:u -F 99 --call-graph=dwarf,4096 \
  -p $(cat cross.pid) -o perf.data &
# 启动 bot 压测；压测返回后：
sudo pkill -INT -f "perf record.*perf\.data"   # SIGINT 让 perf 落盘
sudo perf report -i perf.data --stdio --no-children --percent-limit 2 -f
```

- debug 构建带全量符号；用户态帧直接解析。
- perf 在数据文件里嵌 build id。要用录制时的同一份二进制出报告——录制与出报告之间重新编译会静默
  破坏归因（`perf buildid-list` 显示文件期望的 build id）。
- 内核态样本只有特权用户能解析（`kptr_restrict=1`）；只录用户态（`cycles:u`）让文件更小，而有意思的
  帧本来就在用户态。
- 数据文件由 root 写出、`chown` 之后回读时要加 `-f`（perf 默认拒绝打开不属于当前用户的文件）。

演示录制（2026-11-15，`-F 99` DWARF，上文跨服基准；75 个样本，0 丢失）的符号化输出——
用户态帧直接解析到二进制自身的符号，调用链完整：

```
     3.47%  mistlane  mistlane  [.] boost::asio::detail::conditionally_enabled_mutex<boost::asio::detail::futex_slim_mutex>::scoped_lock::unlock()
     3.06%  mistlane  mistlane  [.] luaS_hash
            ---luaS_hash
               internshrstr
               luaS_newlstr
               luaV_concat
               luaV_execute
               ccall
               luaD_callnoyield
               f_call
               luaD_rawrunprotected
               luaD_pcall
               lua_pcallk
```

前两项正是该场景应有的形态：Asio 会话管线，以及 echo 路径上的 Lua 字符串驻留
（`lua_pcallk` → `luaV_execute` → `luaV_concat` → `luaS_newlstr`）。

## 实测采样影响

开发机上的两次测量（2026-11-15，debug 构建，全程协作席位 loadavg 86–195 / 14 核；所有进程按下文绑核）。

**第一场——顺序臂**（同一时刻只跑一套节点，各臂参数完全一致）：宿主机本身就否定了顺序比较的效力。
同一次扫描里两个 baseline 臂吞吐相差 3.2 倍，另有两臂在协作负载冲顶时节点被压死（所有 bot 客户端报
`TCP: End of file`）：

| 臂 | completed | rps | p50 / p99 ms |
|---|---|---|---|
| baseline | 16000/16000 | 712 | 41.0 / 110.3 |
| 99 Hz DWARF | 16000/16000 | 949 | 29.1 / 95.0 |
| 99 Hz fp | 环境压死 | — | — |
| 999 Hz DWARF | 环境压死 | — | — |
| baseline（收尾） | 16000/16000 | 2298 | 7.5 / 64.4 |

**第二场——成对运行**（同一时刻起两套完全相同的栈，treatment 侧 crossServer 挂 sudo perf；协作负载
同时打在两套栈上，比值即把它消掉）。每条件三次重复；sanity 条件两侧都不挂采样器：

| 条件 | rps 比值 trt/ctrl | 被采样节点 CPU 增量 | RSS |
|---|---|---|---|
| sanity（无采样器） | 1.33 / 1.31 / 1.42 | +10%（1 个样本） | 平坦 ~17 MB |
| 99 Hz DWARF | 0.98 / 1.02（第三次被环境压死） | −1%（中位） | 平坦 |
| 999 Hz DWARF | 1.00 / 0.97 / 0.89 | **+20%**（+14/+20/+27） | 平坦 |
| 99 Hz fp | 1.14 / 1.48 / 0.88 | +13%（中位） | 平坦 |

观察：

- **RSS 平坦**（约 17 MB，全部 12 对加第一场）——采样器在进程外，不给节点增加常驻内存。
- **正确性不受影响**：每个有效重复都恰好完成 `planned` 个请求且 `failed_clients=0`。被打死的重复是
  **两套栈同时**死——环境击杀、对称发生，不是采样器故障。
- **sanity 对并非可互换**：其比值一致地大于 1（重协作负载下两组核并不对等），因此跨条件的绝对比值
  不可比。抗噪的信号是被采样节点自身的 CPU：**999 Hz DWARF 稳定带来 +14–27% CPU**，两种 99 Hz 模式
  都落在 sanity 对 +10% 的记账底线之内。墙上时钟趋势方向一致（999 Hz 比值下探到 0.89）。
- **持续 `-F 999` + DWARF 调用链还会触发内核自动节流**：一个持续 999 Hz 臂运行期间 dmesg 把
  `kernel.perf_event_max_sample_rate` 一路下调（100000 → 79000 → 61000 → 47000 → 37000），产出的
  数据文件几乎没有样本。在本机 999 Hz DWARF 只能当短脉冲工具，不能长时间挂着。
- **`-F 99` + DWARF 调用链低于本机的测量底线**——[跨服与过载保护](./cross-server)的默认配方成立，
  更安静的窗口只会把上界压得更小。

建议：按 `-F 99 --call-graph=dwarf,4096` 录制；999 Hz DWARF 只用于短脉冲（并预期内核节流）；只有当
DWARF 展开在目标机上实测过贵时才退到帧指针；共享主机上测采样开销要用成对运行加节点自身 CPU 时间，
绝不要用顺序的墙上时钟臂。
