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
| 服务连接注册、重连和排队投递 | 未迁移 | 当前会话直接连接上游；没有旧注册报文、共享多路复用连接、重连策略或持久队列。只有具体应用需求明确后再设计。 |
| 旧线程池 | 可复用现有库 | Boost.Asio 提供固定大小 `thread_pool`。如有阻塞任务，Mistlane 可直接使用；仍需在提交层定义准入/背压，因为线程池本身不替应用决定队列上限和关闭策略。 |
| Lua 定时器队列 | 可复用现有库 | Boost.Asio `steady_timer` 提供单调时钟的异步计时器。Lua 的 timeout/interval 适配仍需管理句柄、取消、回调错误和 VM 重载清理；默认不应移植旧时间轮。 |
| Lua 死循环监视线程 | 部分替换 | 已有指令计数 hook；没有照搬旧监视线程及其停止竞态。 |
| CPU/内存性能剖析器 | 未迁移 | 老框架和 Mistlane 都没有 CPU 采样、内存分配剖析或 Lua 函数级性能剖析。旧 Lua 超时 hook 不是 profiler。 |
| MySQL 接入 | 仅连接池层已替换 | 使用 Boost.MySQL `connection_pool` 并提供探针；未迁移游戏表结构、脏写缓存、SQL helper 或通用 Lua 异步数据库桥。 |
| Redis 客户端和 Lua 绑定 | 替换/部分迁移 | vcpkg 已启用 Boost.Redis 1.92。可选 `--redis` 提供有界异步 Lua Streams API 和 sorted-set 排行榜 API。TLS、通用命令接口和 Redis 自动化集成测试仍待实现。 |
| UDP 传输 | 可复用现有库 | Boost.Asio 已有异步 UDP socket。Mistlane 只需定义 datagram/session 契约及可选 Lua 适配；先有明确需求再加入公开 API。 |
| 日志服务和 Lua logger API | 可复用现有库 | Boost.Log 提供过滤器、属性、格式化器和 sink；Drogon 也有日志设施。先为各进程选择统一 C++ 日志门面，再向 Lua 暴露精简结构化 logger。远端聚合属于部署层。 |
| 指标、进程监控和系统信息 | 部分完成 | 可选私有 `/metrics` 端点导出就绪状态、活动会话、准入拒绝、Lua 回调耗时和 RSS。内部健康端点、完整流量/tick 指标、生产仪表盘和压测仍待完成。 |
| 崩溃报告 | 部分完成 | 可选启动 Crashpad handler，将转储写入本地数据库并关闭上传。转储生成验证、保留策略、符号归档和生产部署仍待验证。 |
| 加密、压缩、URL、序列化工具 | 未整体迁移 | 排除游戏专用工具和自定义协议；遇到明确需求时使用维护中的库及显式格式。 |
| 通用 Lua 工具 | 选择性提取 | 目前只审核并提取 MinHeap；其他工具逐个检查依赖、行为、验证和来源。 |
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
