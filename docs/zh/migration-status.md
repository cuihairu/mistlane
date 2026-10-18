# 框架迁移状态

本清单区分“沿用架构思路”和“实际提取或重写的代码”。它描述 Mistlane 0.1 候选，不代表老游戏框架或整套游戏服务端已经迁移完成。

| 老框架能力 | Mistlane 状态 | 当前边界 / 后续决策 |
|---|---|---|
| 进程启动、监听器、信号和退出 | 重新实现 | 新生命周期由 `Node` 管理；不保留旧进程 API 或二进制兼容。 |
| 异步 TCP 传输 | 重新实现 | 使用新的四字节长度头协议，并有会话限制和期限；不支持旧分帧及客户端协议。 |
| WebSocket / 安全 WebSocket | 替换 | WS/WSS 由 Drogon/Trantor 处理，不迁移手写握手、掩码或控制帧解析。 |
| HTTP 服务端 | 替换 | Drogon 处理 HTTP/HTTPS 和 Lua 请求处理器；不保留旧 HTTP API。 |
| HTTP 客户端 | 替换 | 为 C++ 调用者提供有界同步 libcurl 适配器；不得在事件循环回调中阻塞调用。 |
| Lua VM 与回调 | 重新实现 | 用模块 table 及明确的 init/start/message/http/tick/stop/shutdown 钩子替代旧全局回调；单 VM 单线程执行。 |
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

## 状态定义

“重新实现”表示有新实现和明确契约，不代表旧协议/API 兼容或已经生产验证。“替换”表示由维护中的依赖实现协议或连接池，Mistlane 只提供适配。“部分替换”和“未迁移”代表明确缺口。历史跨服过载崩溃尚未复现或诊断；本地准入限制不能证明容量问题已解决。

## 可复用的上游设施

当前依赖 baseline 使用 Boost 1.92.0，因此可直接使用 Asio 定时器、固定大小线程池、UDP socket、Boost.Log 和 Boost.Redis。Boost.Redis 是 Asio 原生 RESP3 客户端，支持断线重连和 Sentinel；Mistlane 已启用它作为可选 Lua Streams 后端。Drogon 1.9.14 的 HTTP 工作线程池可用，但其 Redis client 继续关闭，避免维护两套客户端。Lua 生命周期、请求上限与退出清理由适配层负责。Drogon ORM/MySQL 继续关闭，因为项目已有 Boost.MySQL 连接池。

依据：[Boost.Asio thread_pool](https://www.boost.org/doc/libs/1_92_0/doc/html/boost_asio/reference/thread_pool.html)、[steady_timer](https://www.boost.org/doc/libs/1_92_0/doc/html/boost_asio/reference/steady_timer.html)、[UDP socket](https://www.boost.org/doc/libs/1_92_0/doc/html/boost_asio/basic_datagram_socket.html)、[Boost.Redis](https://www.boost.org/doc/libs/1_92_0/libs/redis/doc/html/redis/index.html)、[Boost.Redis Sentinel](https://www.boost.org/doc/libs/1_92_0/libs/redis/doc/html/redis/sentinel.html)、[Boost.Log](https://www.boost.org/doc/libs/1_92_0/libs/log/doc/html/log/design.html) 和 [Drogon Redis](https://github.com/drogonframework/drogon/wiki/ENG-18-Redis)。

## 文件命名与来源

公开包路径和文件名使用 Mistlane 命名空间，不沿用旧产品前缀。改名不改变来源事实：复用内容和改编的第三方文件仍在 `NOTICE` 与 `THIRD_PARTY.md` 中说明。不要为了描述设计参考而把旧源码文件加入导出包。
