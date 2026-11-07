# 术语表

状态：面向[六层源码模型](./source-layer-analysis.md)的术语归纳。同步英文表是 [Glossary](../glossary.md)。引用基于 `2b57842dd513b3153a7795afbc2af16939609c49`。

| 术语 | English | 定义 | 位置 |
|---|---|---|---|
| 进程角色 | Process role | 在传输启动前选定的七个可运行服务身份之一。 | `include/mistlane/roles.hpp:6-16` |
| 进程配置 | Node configuration | 描述一个进程的已校验字段及 CLI/config-file 输入。 | `include/mistlane/node.hpp:18-70` |
| 配置文件 | Config file | 展开成与命令行相同 flag 的带版本 `KEY=VALUE` 文件。 | `src/main.cpp:103-125` |
| 监听器 | Listener | node 进程持有的一个已配置 TCP、WebSocket 或 HTTP 端点。 | `src/main.cpp:84-101` |
| MLC1 | MLC1 | gateway 跨服路由与准入控制协议。 | `src/node.cpp:1226-1348` |
| MLC2 | MLC2 | 增加请求标识的可选载荷关联协议。 | `src/node.cpp:1226-1348` |
| MLS1 | MLS1 | 用于认证内部服务链路的服务身份握手。 | `src/node.cpp:1478-1523` |
| MLM1 | MLM1 | 在一条认证上游上承载逻辑会话的复用协议。 | `src/node.cpp:1668-1894` |
| MLR1 | MLR1 | 成员、路由、放置与迁移的 registry 控制协议。 | `src/node.cpp:2214-2793` |
| MLE1 | MLE1 | 承载类型化请求、响应和关联流量的实体端点 envelope。 | `include/mistlane/entity_endpoint.hpp:32-52` |
| MLD1 | MLD1 | 加载、提交、栅栏和确认的帧式归属记录协议。 | `include/mistlane/record.hpp:178-341` |
| MLA1 | MLA1 | 带防重放受理语义的签名外部 API 协议。 | `include/mistlane/api.hpp:18-115` |
| MLT1 | MLT1 | 随加入路径上行的短期签名登录凭据格式。 | `include/mistlane/entry_ticket.hpp:15-70` |
| PROBE1 | PROBE1 | probe 监听器提供的就绪与过载探测协议。 | `src/node.cpp:296-436` |
| RegistrySession | RegistrySession | 解析并执行 registry 动词的 coordinator 侧会话。 | `src/node.cpp:2214-2793` |
| RegistryMember | RegistryMember | 向 registry 注册并刷新服务的成员侧客户端。 | `src/node.cpp:2795-2922` |
| RegistryClient | RegistryClient | 查询成员或放置路由的客户端。 | `src/node.cpp:2926-3010` |
| MigrationTransaction | MigrationTransaction | 归属迁移的类型化 prepare/event/commit/abort/retire 状态机。 | `include/mistlane/migration.hpp:116-364` |
| HomeGenerationAuthority | HomeGenerationAuthority | coordinator 侧归属代次来源，用于拒绝过期写入者。 | `include/mistlane/migration.hpp:72-104` |
| WriterLease | WriterLease | 标明允许写入者和过期时间的有界归属凭据。 | `include/mistlane/migration.hpp:104-114` |
| LuaService | LuaService | node 持有的沙箱 Lua VM 及其宿主 API 面。 | `include/mistlane/lua_service.hpp:91-105` |
| RedisRuntime | RedisRuntime | 脚本 API 使用的已配置 Redis 连接包装。 | `src/lua_service.cpp:187-210` |
| Lua 沙箱 | Lua sandbox | 托管脚本外层的封闭 Lua 库集、指令上限和线程模型。 | `src/lua_service.cpp:110-159` |
| EntityContract | EntityContract | 实体定义的解析器、兼容锁、digest、XSD 和绑定生成器门面。 | `include/mistlane/entity_contract.hpp:94` |
| SprotoCodec | SprotoCodec | 基于 sproto 的实体载荷编解码实现。 | `include/mistlane/entity_codec.hpp:23` |
| EntityEndpoint | EntityEndpoint | 运行期 `MLE1` entity envelope 的解码与应答器。 | `include/mistlane/entity_endpoint.hpp:42` |
| PropertyStore | PropertyStore | 实体属性值与脏跟踪的内存原型存储。 | `include/mistlane/entity_property.hpp:56` |
| SyncReceiver | SyncReceiver | 应用有序实体同步事件的接收器原型。 | `include/mistlane/entity_sync.hpp:55` |
| PropertyChannel | PropertyChannel | 发出属性更新并跟踪关注范围的通道原型。 | `include/mistlane/entity_sync.hpp:157` |
| PersistentStore | PersistentStore | 实体持久化接口与字节序列化边界的原型。 | `include/mistlane/entity_persist.hpp:45` |
| EntityRuntime | EntityRuntime | 把 entity 派发绑定进宿主的进程侧运行时缝。 | `include/mistlane/entity_runtime.hpp:12` |
| 网关实体关联 | GatewayEntityCorrelation | 把外层传输关联号改写为新内层实体 ID 的有界表。 | `include/mistlane/entity_gateway.hpp:23` |
| RecordStore | RecordStore | 带版本的归属记录缓存与持久后端接口。 | `include/mistlane/record.hpp:178-341` |
| HomeSequenceAuthority | HomeSequenceAuthority | 按角色强制连续记录序号的游标权威。 | `include/mistlane/record.hpp:125-160` |
| ReturnSendQueue | ReturnSendQueue | 只有收到持久确认后才释放变更的按角色有序队列。 | `include/mistlane/record.hpp:342-452` |
| MigrationJournal | MigrationJournal | 保存迁移、归属、放置、记录和栅栏持久性的 SQLite 后端。 | `src/journal.cpp:342-398` |
| ApiVerifier | ApiVerifier | 验证 MLA1 签名、时间和命令的组件。 | `include/mistlane/api.hpp:66` |
| ApiAcceptanceStore | ApiAcceptanceStore | 防重放外部命令受理的原子 create-or-read 接口。 | `include/mistlane/api.hpp:98-115` |
| TicketVerifier | TicketVerifier | 对签名并会过期的 MLT1 凭据做常量时间验证的组件。 | `include/mistlane/entry_ticket.hpp:56` |
| TicketIssuer | TicketIssuer | 使用配置密钥签署 MLT1 凭据的无状态签发器。 | `include/mistlane/entry_ticket.hpp:197` |
| Diagnostics | Diagnostics | 进程级 Crashpad、Prometheus 和脚本指标注册中心。 | `include/mistlane/diagnostics.hpp:22` |
| ProbeServer | ProbeServer | 监听器侧的就绪与过载应答器。 | `src/node.cpp:296-436` |
| DashboardServer | DashboardServer | 面向 node 的内嵌 HTTP 页面和指标发布器。 | `src/node.cpp:438-560` |
