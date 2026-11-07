# 源码分层分析

状态：2026-11-07 基于 `2b57842dd513b3153a7795afbc2af16939609c49` 的源码归纳。以下六层是职责边界，不是目录边界。跨语言页面：[English](../source-layer-analysis.md)；术语表：[术语表](./glossary.md)。

## 分层模型

| 层 | 职责 | 代表实现 | 状态 |
|---|---|---|---|
| L1 进程与角色层 | 解析单进程配置，选择角色，校验角色相关组合，并分发到 node、bot、contract 或 test 命令。 | `src/main.cpp:195`、`src/main.cpp:336-448`、`include/mistlane/roles.hpp:6-16` | 无 |
| L2 传输与会话层 | 终结 TCP/WebSocket/HTTP 监听器，组帧，认证服务链路，复用上游链路，执行过载控制并转发会话载荷。 | `src/node.cpp:226-295`、`src/web.cpp:16-113`、`src/node.cpp:1146-1860` | 进程内存 |
| L3 协调与注册层 | 跟踪带作用域的成员和路由，回答放置查询，并运行类型化迁移事务。 | `src/node.cpp:2214-2793`、`src/node.cpp:2795-3010`、`include/mistlane/migration.hpp:72-364` | 内存；可选 SQLite journal |
| L4 服务执行层 | 每个脚本角色托管一个沙箱 Lua VM，暴露业务 API 和回调，并连接 Redis 与外呼 HTTP。 | `include/mistlane/lua_service.hpp:76-105`、`src/lua_service.cpp:136-1063`、`src/http_client.cpp:23-58` | 配置后使用 Redis；进程内存 |
| L5 实体运行时层 | 解析实体契约，编码 sproto 载荷，派发端点调用，跟踪属性、同步和原型 RPC。 | `include/mistlane/entity_contract.hpp:94`、`include/mistlane/entity_codec.hpp:23`、`include/mistlane/entity_runtime.hpp:12` | 进程内存 |
| L6 持久化、准入与可观测层 | 管理持久记录、回传队列和写入栅栏；验证 API/凭据准入；暴露指标、探测与仪表盘。 | `include/mistlane/record.hpp:125-497`、`include/mistlane/api.hpp:66-115`、`include/mistlane/diagnostics.hpp:22` | 配置后使用 SQLite/API store；其余内存 |

## 架构解说

### L1 进程与角色层

- **实现：** `main` 在 `src/main.cpp:195-203` 分发 `test`、`bot`、`contract`、内嵌 scaffold 命令和普通 node 路径；scaffold 模板在 `src/scaffold.cpp:88-180`。
- **配置 schema：** `Config` 声明在 `include/mistlane/node.hpp:18-70`；配置文件 `KEY` 到 flag 的映射在 `src/main.cpp:107-125`。优先级为文件值、`MISTLANE_<KEY>`、后续显式 flag（`src/main.cpp:103-106` 和 `src/main.cpp:206-215`）。角色相关组合校验在 `src/main.cpp:336-448`。
- **专属协议或文案：** 无；进程措辞在[进程职责与命名](./process-roles.md)和使用指南。
- **调用面：** 本层不持有网络 I/O。它构造配置好的 L2 node（或其他命令目标），最终经普通进程返回退出。

### L2 传输与会话层

- **实现：** TCP 组帧是 `src/node.cpp:226-295` 的 `TcpChannel`；Drogon Web 适配在 `src/web.cpp:16-113`；一个 `Session` 在 `src/node.cpp:1146-1860` 解析并分发协议控制与业务载荷；`MuxHub` 在 `src/node.cpp:1668-1894` 复用逻辑会话。通用传输缝在 `include/mistlane/transport.hpp:12-24`。
- **配置 schema：** listener 定义和 transport 选择来自 `Config.listeners`、`--listener`、`--transport`、证书、token、多路复用和过载字段，声明于 `include/mistlane/node.hpp:18-70`；listener 文件解析从 `src/main.cpp:84-101` 开始。
- **专属协议与文案：** `MLS1`、`MLC1`、`MLC2`、`MLM1` 汇总在[协议面分析](./protocol-analysis.md)；过载行为在[跨服与过载保护](./cross-server.md)。
- **调用面：** 客户端和浏览器连到本层。`MLC1` 询问 gateway 路由；`MLS1` 认证内部服务链路；`MLC2` 增加请求关联；`MLM1` 在一条认证上游上承载逻辑会话。业务载荷随后进入 L4；`MLE1` 被拦截送入 L5（`src/node.cpp:1601`、`src/node.cpp:1638-1648`）。
- **状态：** 会话缓冲、路由状态、关联表和令牌桶都是进程内存。

### L3 协调与注册层

- **实现：** `RegistrySession` 在 `src/node.cpp:2214-2793` 实现注册表线侧行为；`RegistryMember` 在 `src/node.cpp:2795-2922` 维护上游注册；`RegistryClient` 在 `src/node.cpp:2926-3010` 解析放置；事务模型是 `include/mistlane/migration.hpp:116-364` 的 `MigrationTransaction`，`HomeGenerationAuthority` 和 `WriterLease` 位于 `include/mistlane/migration.hpp:72-114`。
- **配置 schema：** `Config` 覆盖 `--coordinator`、`--namespace`、`--scope`、route epoch 和可选 `--journal`（`include/mistlane/node.hpp:18-70`、`src/main.cpp:109-125`）。
- **专属协议与文案：** `MLR1` 和 `MLR1 MIG` 动词记录在[协议面分析](./protocol-analysis.md)和进程职责页。
- **调用面：** 成员经认证 MLR1 注册；服务查询放置和路由；迁移 owner 驱动类型化 PREPARE/EVENT/COMMIT/ABORT/RETIRE 事务。这是独立于业务转发的控制面。
- **状态：** 成员、路由、迁移和归属状态默认仅内存；`--journal` 提供 `MigrationJournal` 时落 SQLite，建表见 `src/journal.cpp:342-392`。

### L4 服务执行层

- **实现：** `LuaService` 声明在 `include/mistlane/lua_service.hpp:91-105`，实现在 `src/lua_service.cpp:136-1063`。VM 在 `src/lua_service.cpp:147-151` 关闭到有限库，在 `src/lua_service.cpp:110-115` 应用指令预算，并在 `src/lua_service.cpp:152-159` 注册九个 API 族。`RedisRuntime` 在 `src/lua_service.cpp:187-210`；外呼 HTTP 使用 libcurl（`src/http_client.cpp:23-58`）。
- **配置 schema：** 脚本和 Redis 选择来自 `--script`、`--redis`、`MISTLANE_REDIS_*`；配置 KEY 映射在 `src/main.cpp:109-125`，运行时合法性由 L1 校验。
- **专属协议与文案：** Lua API 和生命周期在[Lua 与生命周期](./lua.md)；回调边界在[HTTP、WebSocket 与外部回调](./http.md)。
- **调用面：** L2 把会话载荷送入脚本回调；L5 调用绑定的 entity 表；脚本经注册 API 访问 Redis、HTTP、placement、ticket、return 和 metrics 适配器。
- **状态：** 脚本全局状态和异步工作是进程内存；配置后才存在 Redis；MySQL 仅作为示例适配缝出现在 `include/mistlane/mysql_pool.hpp:15-46`。

### L5 实体运行时层

- **实现：** 契约解析和生成集中在 `include/mistlane/entity_contract.hpp:94` 的 `EntityContract`；sproto 编码是 `include/mistlane/entity_codec.hpp:23` 的 `SprotoCodec`；在线端点解码是 `include/mistlane/entity_endpoint.hpp:42` 的 `EntityEndpoint`；运行时缝声明在 `include/mistlane/entity_runtime.hpp:12`；属性、同步和持久化原型在 `include/mistlane/entity_property.hpp:56`、`include/mistlane/entity_sync.hpp:55`、`include/mistlane/entity_sync.hpp:157`、`include/mistlane/entity_persist.hpp:45`。
- **配置 schema：** `--contract` 选择实体契约（`src/main.cpp:109-125`）；L1 会拒绝与所选角色不匹配的 contract 组合。
- **专属协议与文案：** 契约语法、SDK 生成、envelope 和错误在[实体契约与代码生成](./entity-contract.md)；`MLE1` 面也纳入协议面清单。
- **调用面：** 入站 `MLE1` 在 Lua 载荷前解码（`src/node.cpp:1338-1348`）；契约在 `src/node.cpp:3187-3208` 装入 node；解码后的 entity 工作到达 L4 绑定的派发表，属性与持久化实现仍是原型缝。
- **状态：** 契约、编解码器、属性、RPC 调用表和网关关联都是进程内存；本层没有 Redis 或 MySQL 状态。

### L6 持久化、准入与可观测层

- **实现：** 持久记录和回传语义在 `include/mistlane/record.hpp:125-497`：`HomeSequenceAuthority` 在 125 行、`RecordStore` 在 178 行、`ReturnSendQueue` 在 342 行、`WriterLease` 在 456 行；SQLite 后端是 `src/journal.cpp:342-398` 的 `MigrationJournal`。MLA1 验证与受理接口在 `include/mistlane/api.hpp:66-115`，SQLite 实现在 `src/api_store.cpp:64-110`；MLT1 原语在 `include/mistlane/entry_ticket.hpp:15-210`。`Diagnostics` 在 `include/mistlane/diagnostics.hpp:22`，`ProbeServer` 在 `src/node.cpp:296-436`，`DashboardServer` 在 `src/node.cpp:438-560`。
- **配置 schema：** journal 路径、metrics/probe/dashboard 监听器和准入密钥由 `--journal`、`--metrics-listen`、`--probe-listen`、`MISTLANE_MLA1_*`、`MISTLANE_TICKET_SECRETS`、`MISTLANE_PROBE_TOKEN` 表达；配置 KEY 见 `src/main.cpp:109-125`。
- **专属协议与文案：** `MLD1`、持久确认、MLA1 和服务准入由[跨服回传与唯一写入权协议](./cross-return-protocol.md)与[login、data 与 apiServer 契约](./service-contracts.md)覆盖。
- **调用面：** L2/L3 向本层询问探测和诊断；L4 经其签发 ticket、提交 return；MLA1/API 与 MLT1/ticket 调用方可跨进程或网络边界。MLD1 记录流量是帧式 TCP 链路。
- **状态：** SQLite 保存迁移/归属/放置、记录行/栅栏和持久 API 受理（`src/journal.cpp:354-378`、`src/api_store.cpp:64-110`）；未显式配置存储时，队列、租约、ticket 和计数器保持内存。

## 为什么这样拆

1. **进程选择先于协议行为。** 七个可运行角色是封闭表（`include/mistlane/roles.hpp:6-16`），`main` 在任何 socket 建立前校验角色相关的 script、upstream、secret、contract 和 journal 组合（`src/main.cpp:336-448`）。独立一层避免传输代码变成另一套配置解析器。
2. **线端终结与业务执行是不同故障域。** TCP 组帧隔离在 `TcpChannel`（`src/node.cpp:226-295`），会话路由和转发在 `Session`（`src/node.cpp:1226-1348`），VM 和 API 在 `LuaService`（`src/lua_service.cpp:136-1063`）。连接背压、组帧错误和脚本错误由此有不同 owner。
3. **registry 是控制面，不是业务中继。** `RegistrySession` 持有成员、路由和迁移动词（`src/node.cpp:2214-2793`），而 `Session` 转发玩法流量（`src/node.cpp:1226-1348`）。分开维护架构中的放置/回传区别，而不是把它埋进 gateway 路径。
4. **脚本托管是适配器边界。** Lua 库在 `src/lua_service.cpp:147-151` 关闭，指令在 `src/lua_service.cpp:110-115` 设上限，宿主能力经命名 API 注册进入（`src/lua_service.cpp:152-159`）。脚本不能自行扩展 I/O 面。
5. **实体元数据是生成期，端点行为是运行期。** XML 解析、lock/digest 生成和 C++/JS/C# 输出集中在 `EntityContract`（`include/mistlane/entity_contract.hpp:94`），小而独立的 `MLE1` 端点在 `include/mistlane/entity_endpoint.hpp:42`。schema 兼容和在线派发的变更节奏不同。
6. **持久与易失归属显式分开。** `RecordStore`、栅栏、回传队列和写入租约是独立类型（`include/mistlane/record.hpp:178`、`include/mistlane/record.hpp:342`、`include/mistlane/record.hpp:456`），SQLite 持久化只经 `MigrationJournal`（`src/journal.cpp:342-392`）。该边界防止把内存 ACK 描述成持久。
7. **准入是可验证、防重放的 I/O。** MLA1 分离验证、受理键和持久存储（`include/mistlane/api.hpp:66-115`）；MLT1 使用常量时间 HMAC 验证（`include/mistlane/entry_ticket.hpp:182-190`）。这些关注点需要独立于玩法路由的生命周期。
8. **指标和就绪服务于所有层，但不实现任何业务。** `Diagnostics` 集中 Prometheus 和 Crashpad 状态（`include/mistlane/diagnostics.hpp:22`），就绪由 `ProbeServer` 应答（`src/node.cpp:296-436`）。可观测因此是依赖方向，不是业务功能。
9. **模型也记录抽取债务，不只表达意图。** `src/node.cpp` 仍同时包含 L2、L3、probe 和 dashboard 实现（`1226-1894`、`2214-3010`、`296-560` 行）；`src/lua_service.cpp` 同时承载脚本运行时和多个适配器（`136-1063` 行）；`EntityContract` 把解析器和生成器放在一起（`include/mistlane/entity_contract.hpp:94`）。分层模型给未来拆分提供目标，不声称每个文件已经等于该层。

## 相关页面

- [术语表](./glossary.md)
- [English](../source-layer-analysis.md)
- [架构与迁移](./architecture.md)
- [协议面分析](./protocol-analysis.md)
