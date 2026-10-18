# 框架迁移状态

本清单区分“沿用架构思路”和“实际提取或重写的代码”。它描述 Mistlane 0.1 候选，不代表老游戏框架或整套游戏服务端已经迁移完成。

| 老框架能力 | Mistlane 状态 | 当前边界 / 后续决策 |
|---|---|---|
| 进程启动、监听器、信号和退出 | 重新实现 | 新生命周期由 `Node` 管理，逐项核对见[旧生命周期核对](#旧生命周期核对)；不保留旧进程 API 或二进制兼容。 |
| 异步 TCP 传输 | 重新实现 | 使用新的四字节长度头协议，并有会话限制和期限；不支持旧分帧及客户端协议。 |
| WebSocket / 安全 WebSocket | 替换 | WS/WSS 由 Drogon/Trantor 处理，不迁移手写握手、掩码或控制帧解析。 |
| HTTP 服务端 | 替换 | Drogon 处理 HTTP/HTTPS 和 Lua 请求处理器；不保留旧 HTTP API。 |
| HTTP 客户端 | 替换 | 为 C++ 调用者提供有界同步 libcurl 适配器；不得在事件循环回调中阻塞调用。 |
| Lua VM 与回调 | 重新实现 | 用模块 table 及明确的 init/start/message/http/tick/stop/shutdown 钩子替代旧全局回调，逐项映射见[Lua 回调核对](#旧-lua-回调核对)；单 VM 单线程执行。 |
| Lua 死循环检测 | 部分替换 | 指令计数 hook 能发现常见失控脚本，但不是硬 CPU 配额或内存沙箱。 |
| gateway / router / worker 拓扑 | 重新实现 | 有通用 TCP 转发和 worker 执行；没有账号身份、玩家亲和路由或旧服务注册协议。 |
| 跨服房间路由 | 新的最小实现 | `cross`/`shard` 提供 group/room 固定路由、epoch 检查和本地硬准入；不提供全局配额、服务发现或玩家状态迁移。 |
| 服务连接注册、重连和排队投递 | 未迁移 | 当前会话直接连接上游。旧握手已于 2026-10-18 追踪：主动方经 `L_FirstMsg`/`ServerService:handShakeToServer` 发送 `sp.pack(Protocol.HandShake, {name, sidx, tarName})`，被动方把首个原始报文交给 `L_Register`，首个回应后连接切换到帧模式（`L_ServerMsg`），握手前的写请求先排队，重连为固定 2 秒定时器加 `proxyOff` 钩子。该报文只做身份声明、无任何认证，重新实现不会带来安全性。维持静态 `UPSTREAMS` 加 MLC1 epoch 握手；只有出现动态发现或跨不可信网络互联的真实需求时，才一并设计注册与认证。旧发送队列也已追踪：实际生产者是 Lua `net_asyncWrite` 与 C++ `pushData`，向每连接的无界发送链表追加（超过 64 KiB 的载荷拆成带序号的分片，仅打告警），消费者是 `async_write` 链；断线直接销毁链表、不重投；握手前的 `pushQueue` 双端队列在旧代码树中无任何调用方；重连以固定 2 秒定时器无限重试。不引入有界可靠投递模块：旧实现本身没有可移植的投递保证，该模块必须依据具体应用需求再定义，且需明确字节/条数上限与溢出策略。 |
| 旧线程池 | 可复用现有库 | 旧并发点已于 2026-10-18 分类：`FogsProcess` 线程组属于 Asio I/O（`io_service::run` 加各 `IServer` 循环），`HttpRequestService` 是固定 pthread 池跑阻塞 `curl_easy` 调用，`d_push` 的 `TaskScheduler` 用 CPU 核数的 `ThreadPool` 处理阻塞推送任务，`d_world` 内嵌 Poco HTTP 服务线程池（阻塞），`DeadLoopMonitor` 看门狗线程以指令 hook 取代，旧 MySQL 经 libmysqlclient 在 server 线程上同步调用。Mistlane 热路径全部异步（Asio I/O、Boost.MySQL/Boost.Redis 池）；同步 curl 适配器仅供 CLI 使用并注明禁止在事件循环回调中调用，因此当前不实例化任何阻塞工作池。如出现阻塞任务需求，Boost.Asio `thread_pool` 仍是指定载体，并在提交层定义准入/背压，因为线程池本身不替应用决定队列上限和关闭策略。 |
| Lua 定时器队列 | 可复用现有库 | 旧 `game/Util/Timer.lua` 接口已于 2026-10-18 对比：`setTimeout`/`setInterval`/`remove` 建在纯 Lua 最小堆加"近期任务"数组上（上限 100 000 个任务、5 秒内任务分桶），由墙钟 `L_doTick` 驱动，`xpcall` 记录错误、周期任务更新时间戳后重新入堆，并以魔法返回值 `xx1` 移除出错周期任务。Boost.Asio `steady_timer` 在底层已提供单调异步计时器。决策：暂不增加可取消的 Lua `timeout`/`interval` API——框架内部没有消费者，旧设计的需求（游戏单位级别六位数量程的调度）属于应用域。日后应用确需时，基于 `steady_timer` 实现薄 timeout/interval 对，带注册句柄、显式取消、明确的回调错误策略（不用魔法哨兵）、VM 重载时挂起定时器随 VM 销毁以及有界任务数，并先按行为分别定义契约（见挂起定时器契约项）。 |
| Lua 死循环监视线程 | 部分替换 | 已有指令计数 hook；没有照搬旧监视线程及其停止竞态。 |
| CPU/内存性能剖析器 | 未迁移 | 老框架和 Mistlane 都没有 CPU 采样、内存分配剖析或 Lua 函数级性能剖析。旧 Lua 超时 hook 不是 profiler。 |
| MySQL 接入 | 仅连接池层已替换 | 使用 Boost.MySQL `connection_pool` 并提供探针；未迁移游戏表结构、脏写缓存、SQL helper 或通用 Lua 异步数据库桥。 |
| Redis 客户端和 Lua 绑定 | 替换/部分迁移 | vcpkg 已启用 Boost.Redis 1.92。可选 `--redis` 提供有界异步 Lua Streams API 和 sorted-set 排行榜 API。`MISTLANE_REDIS_TLS=on` 启用 TLS 并强制校验证书链和所拨地址（`MISTLANE_REDIS_CA` 指定信任锚）；明文模式保留为可信内网默认。集成测试覆盖消费组重复创建、服务端重启后的断线重连、热重载期间旧回调失效，以及 TLS 按 CA 与名称的接受/拒绝。通用命令接口仍待实现。 |
| UDP 传输 | 可复用现有库 | 旧 UDP 调用点已于 2026-10-18 盘点：唯一的服务端实例化是 `WorldServer::openUdpServer`，以 `C_openUdpServer(ip, port)` 暴露给 Lua，而其唯一 Lua 调用点（`WorldService.lua`）已被注释；`UdpClient` 侧与 `L_onUdpClientMsg`/`L_onUdpServerMsg` 回调在 net 层之外无任何调用方，旧代码树中 UDP 实际处于死代码状态。Boost.Asio 已有异步 UDP socket；不添加 datagram 契约或 Lua 适配，公开 API 出现前必须先确认真实用例。 |
| 日志服务和 Lua logger API | 可复用现有库 | 最小需求已于 2026-10-18 确定：每行含 UTC 时间戳、级别（error/warn/info，debug 需显式开启）、角色、监听地址和消息；`ready <role> <addr>` 保持机器可读契约不变；写入须对事件循环、web 工作线程和异步回调并发安全；轮转与采集默认交给进程监督方（stdout 交给 journald），文件 sink 和远端聚合留给部署层。Boost.Log 1.92（线程安全的过滤器/属性/格式化器/sink，已在 baseline 内）选定为跨角色统一 C++ 门面，待确需第二个 sink 或级别过滤时接入——当前约 24 处直接 `std::cout`/`std::cerr` 调用点继续有效，Drogon 与 Boost.Redis 保留各自日志器。在出现具体脚本需求前不向 Lua 暴露 logger API。 |
| 指标、进程监控和系统信息 | 部分完成 | 旧系统信息调用点已于 2026-10-18 盘点：`d_common/sysinfo` 的存活面只有六个 main 里的启动布尔 `initSysCounter()` 和给 d_push 阻塞线程池定容的 `gCpuNum`；CPU 利用率采样（`getCpuUtil`）无存活调用方（`gmGetSysInfo` 整体被注释）；`L_StateMsg`/`L_OmMsg` 承载应用状态与运营消息，不是系统指标。Mistlane 因此保留可选私有 `/metrics` 端点（就绪状态、活动会话、准入拒绝、Lua 回调耗时、RSS）作为 Prometheus 面；CPU/内存观测继续交给节点级采集（cgroup、node_exporter），在出现具体仪表盘需求前不引入 OpenTelemetry 适配或进程内 sysinfo API。内部健康端点、完整流量/tick 指标、生产仪表盘和压测仍待完成。 |
| 崩溃报告 | 部分完成 | 可选启动 Crashpad handler，将转储写入本地数据库并关闭上传。安装规则将 `mistlane` 与 `crashpad_handler` 并排安装（2026-10-18）：两者只需系统运行库，部署文档记录容器 `ptrace` 要求与同目录/数据库约定。转储生成已由自动化演练验证（2026-10-18）：`mistlane-crash-sample` 二进制经同一套管线复现受控空指针解引用，原生 `crashpad.minidump` 用例断言数据库落出带 `MDMP` 魔数的真实转储。保留策略和符号归档仍属演练工作。 |
| 加密、压缩、URL、序列化工具 | 未整体迁移 | 排除游戏专用工具和自定义协议；遇到明确需求时使用维护中的库及显式格式。 |
| 通用 Lua 工具 | 选择性提取 | 目前只审核并提取 MinHeap。其余 23 个 `game/Util` 候选已于 2026-10-18 逐个筛查：`JSON.lua` 为 GPL 授权的 JSON4Lua（与 Apache-2.0 不兼容，硬性排除）；`Aes.lua`、`Rsa.lua`、`SnapShotDump.lua`、`CacheCode.lua` 依赖外部 Lua 库（luacrypto、lua-openssl、snapshot、sproto `Data` 模块）或在源码中提交密钥；`Logger.lua`、`log.lua`、`SystemUtil.lua`、`Print.lua`、`MessageUtil.lua`、`StringUtil.lua`、`TableUtil.lua`、`Date.lua` 依赖旧 `C_*` 日志/时间全局（`Print` 还覆写全局 `print`）；`DropTableUtil.lua`、`ServerUtil.lua`、`TimeExecute.lua`、`Timer.lua`/`TimerQueue.lua` 属游戏领域、旧拓扑或已定案的 timer 面；`Math.lua` 需要 `bit32`（沙箱不开放）；`VerificationUtil.lua`、`TimeUtil.lua` 为应用层助手。当前不提取任何文件；`Base64`、`StringUtil`、`TableUtil`、`Sort` 等通用形状候选在出现具体消费方时再逐个带测试与来源审查。 |
| 玩法、账号、支付、寻路、缓存表、运维及 SDK | 排除 | 属于游戏应用、私有部署或第三方集成，不属于通用框架迁移目标。 |

## 旧生命周期核对

对照旧 `FogsProcess`/`d_*` main 与 Mistlane `Node` 的逐项核对（2026-10-18 双向核对两份源码）：

| 生命周期条目 | 旧行为 | Mistlane 行为 | 结论 |
|---|---|---|---|
| 启动入口 | `main()` 解析命令行（Lua 配置文件、日志配置、进程编号），配置 log4cxx，注册全局 `mysql_init`/`mysql_end`，再按顺序跑 `initGlobal`/`initSysCounter`/`checkProtocol` 布尔检查。 | CLI 解析与 `Node` 构造同步完成全部初始化；任一步失败即抛异常退出，不会留下半启动状态。Lua 配置文件与进程编号由实例 env 文件加 CLI 参数替代。 | 契约不同的重新实现 |
| 线程模型 | `FogsProcess::start` 为每个附加 `IServer` 起一个 `boost::thread`，另起一个 `io_service::run` 线程，期间屏蔽全部信号；随后 `main` 进入 `sigwait`。 | TCP 角色由单个 `io_context` 驱动到底；web 传输多一个工作线程。没有按 server 拆分线程。 | 简化 |
| 信号集 | `sigwait` 等待 `SIGINT`、`SIGQUIT`、`SIGTERM`；其余信号保持默认，仅在起线程窗口内持掩码。 | `asio::signal_set` 在事件循环上处理 `SIGINT`、`SIGTERM`、`SIGHUP`；`SIGQUIT` 刻意留给 Crashpad 作为崩溃路径。 | 契约不同 |
| 热重载 | 不存在。 | `SIGHUP` 只有在替换模块加载并初始化成功后才切换 Lua VM；失败保留旧 VM 与模块状态。 | 新能力 |
| 退出入口 | `sigwait` 返回后 `shutdown()` 置状态位、停主 server，然后每 100ms 轮询 `serverConns_` 排空，每秒打日志，超过 30 秒强制移除残余连接。 | 信号回调调用 `stop()`：清除就绪位、关闭 acceptor、停 Lua tick、停 Redis 与 web 栈，按顺序停止每个会话，再执行 Lua `shutdown` 回调；每个回调都有包装，单点失败只打日志并继续收尾。 | 重新实现；无固定强杀窗口 |
| IO/线程收尾 | 排空循环后 post `io_service::stop`，逐个对附加 server 调 `shutdown(0)`，最后 `join_all`。 | 会话先完成取消处理器再销毁 `Node` 状态；io context 自然排空，可选工作线程被 join。 | 顺序等价、机制不同 |
| 异常路径 | 初始化步骤逐级返回布尔并以 `EXIT_FAILURE` 退出；shutdown 期间的异常没有包装。 | 构造与 CLI 失败在 `main` 捕获后非零退出；web 启动异常被捕获、停机后重抛；stop/shutdown 回调捕获并打日志，不会从循环中抛穿。 | 强于旧实现 |

## 旧 Lua 回调核对

旧 `LuaCallHandler` 全局回调面到 Mistlane `LuaService` 钩子的逐项映射（2026-10-18 双向核对两份源码）：

| 旧全局回调 | 旧签名 | Mistlane 对应项 | 结论 |
|---|---|---|---|
| `L_initServer` | `(processIndex)`，启动时按进程编号调用一次 | 模块钩子 `init(context)`；进程身份由 `ServiceContext` table 提供（role、transport、route epoch），没有数字编号 | 契约不同的重新实现 |
| *启动阶段* | 不存在；监听器启动后直接开始 accept | `start()` 钩子在 bind 之后、首次 accept 之前执行 | 新增的时序能力 |
| `L_doTick` | `(wallClockMilliseconds)`，周期由调用方掌握 | `tick()` 绑定固定 100ms 的 steady 计时器，无参数 | 重新实现；接口面更窄 |
| `L_closeServer` | 在退出路径调用 | 拆为停机信号上的 `stop()` 与最终 Lua 收尾的 `shutdown()` | 拆成两个阶段 |
| `L_ClientMsg` / `L_ServerMsg` / `L_PreMsg` / `L_LoggerMsg` / `L_OMMsg` / `L_PayMsg` / `L_StateMsg` / `L_onRPC` | 按消息来源分派 `(id, payload)`，以整数码表示结果 | 单一 `message(payload, route)` 返回字符串；没有按来源分派，也没有 RPC 通道 | 以更小契约定义替换 |
| `L_clientOn` / `L_clientOff` / `L_connected` / `L_disConnected` | 带 lightuserdata 连接句柄的会话连接/断开事件 | 未迁移；会话由 C++ `Session` 对象持有，Lua 无逐连接回调 | 未迁移（刻意） |
| `L_Register` / `L_FirstMsg` / `L_Response` | 基于 lightuserdata 连接的服务间注册、首个报文与响应处理 | 未迁移；上游为直连，无注册协议 | 未迁移（见服务连接注册行） |
| `L_HttpRequest` | `(uri, params, ip)` 返回字符串，默认 `"{}"` | `http(request)` 返回响应 table（status/headers/body），并带 `before_request`/`on_error` 钩子 | 改为结构化 table |
| `L_payCallBack` | `(uri, params, ip)` | 排除；支付属于应用域 | 排除 |
| `L_onUdpClientMsg` / `L_onUdpServerMsg` | UDP 数据报回调 | 未迁移（见 UDP 行） | 未迁移 |
| `L_SaveTearDown` / `L_OnlineTime` | 退出前存档与在线时长上报 | 未迁移；持久化与运维域均排除 | 未迁移 |
| VM 与错误模型 | `LuaContext` 配 `debug.traceback` 错误处理器、末位整数码约定、`DeadLoopMonitor` 监视线程、`lua_newthread` 局部上下文 | 单个 VM 持有注册表引用的模块 table，用异常消息替代返回码，指令计数 hook，不使用 Lua 线程 | 替换 |
| 加载的库 | base、package、table、os，另有 `appendLibPath` | 仅 base、string、table、math | 受限沙箱 |
| 热重载 | 不存在 | `SIGHUP` 只有在新模块加载并初始化成功才重建替换 VM，否则保留旧 VM | 新能力 |

## 状态定义

“重新实现”表示有新实现和明确契约，不代表旧协议/API 兼容或已经生产验证。“替换”表示由维护中的依赖实现协议或连接池，Mistlane 只提供适配。“部分替换”和“未迁移”代表明确缺口。历史跨服过载崩溃尚未复现或诊断；本地准入限制不能证明容量问题已解决。

## 可复用的上游设施

当前依赖 baseline 使用 Boost 1.92.0，因此可直接使用 Asio 定时器、固定大小线程池、UDP socket、Boost.Log 和 Boost.Redis。Boost.Redis 是 Asio 原生 RESP3 客户端，支持断线重连和 Sentinel；Mistlane 已启用它作为可选 Lua Streams 后端。Drogon 1.9.14 的 HTTP 工作线程池可用，但其 Redis client 继续关闭，避免维护两套客户端。Lua 生命周期、请求上限与退出清理由适配层负责。Drogon ORM/MySQL 继续关闭，因为项目已有 Boost.MySQL 连接池。

依据：[Boost.Asio thread_pool](https://www.boost.org/doc/libs/1_92_0/doc/html/boost_asio/reference/thread_pool.html)、[steady_timer](https://www.boost.org/doc/libs/1_92_0/doc/html/boost_asio/reference/steady_timer.html)、[UDP socket](https://www.boost.org/doc/libs/1_92_0/doc/html/boost_asio/basic_datagram_socket.html)、[Boost.Redis](https://www.boost.org/doc/libs/1_92_0/libs/redis/doc/html/redis/index.html)、[Boost.Redis Sentinel](https://www.boost.org/doc/libs/1_92_0/libs/redis/doc/html/redis/sentinel.html)、[Boost.Log](https://www.boost.org/doc/libs/1_92_0/libs/log/doc/html/log/design.html) 和 [Drogon Redis](https://github.com/drogonframework/drogon/wiki/ENG-18-Redis)。

## 文件命名与来源

公开包路径和文件名使用 Mistlane 命名空间，不沿用旧产品前缀。改名不改变来源事实：复用内容和改编的第三方文件仍在 `NOTICE` 与 `THIRD_PARTY.md` 中说明。不要为了描述设计参考而把旧源码文件加入导出包。

截至 2026-10-18 没有任何通用模块获得迁移批准：MinHeap 已提取，其余旧工具均被上文筛查排除，因此没有需要拆分的实现任务。今后任何批准都必须先立独立任务，写明公开接口、许可证来源和独立验收条件，然后才写代码。
