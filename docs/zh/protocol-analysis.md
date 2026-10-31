# 协议面分析

状态：2026-10-20 对照 `fe8b2a6`、`b35786a` 与下述迁移/API 第一片重新核查。本文接续实体、服务、跨服回传与验证分析，不把文档契约等同于运行时接线。清单区分线行为、已定设计和缺失的运行时工作。

## 协议面全景

| 分层 | 协议面与边界 | 出处 |
|---|---|---|
| 传输与监听 | TCP 使用四字节大端长度帧，载荷上限 1 MiB；WS 消息映射为单个载荷。HTTP/HTTPS 承载请求/响应；WS/WSS 监听器与升级分别进入游戏会话或 Lua HTTP 生命周期。每进程可配置多监听器。 | [architecture.md 执行与连接模型](../architecture.md#execution-and-connection-model)、[http.md 服务端、多监听器](../http.md#server) |
| 传输控制 | 可选 `MLC2` 在载荷内增加 u32 BE 请求 ID（响应回显、推送用 0）；可选 `MLS1` 认证内部服务链路；可选 `MLM1` 在认证后的上游复用逻辑会话。三者属于不同层；`MLC1` 跨服路由不与 MLC2/MLM1 模式混用。 | [architecture.md:66–70](../architecture.md#execution-and-connection-model)、[validation.md 服务身份/多路复用行](../validation.md) |
| 路由与跨服 | `MLC1` 是 gateway 跨服路由/准入握手，可选携带全有或全无的参与者后缀（`ch`、`gen`、`ret`）。它选择静态房间/组亲和与准入，不等于动态角色分配或迁移。`PROBE1` 提供就绪/过载检查。 | [cross-server.md](../cross-server.md)、[cross-return-protocol.md 参与者与返回上下文](../cross-return-protocol.md#参与者与返回上下文)、[architecture-comparison.md:53–60](../architecture-comparison.md) |
| coordinator 控制面 | `MLR1` 在带命名空间的作用域内提供成员注册、心跳、QUERY/PLACE 和 ROUTE SET/GET。注册携带就绪/负载；现有路由记录指向当前已注册成员。类型化迁移事务模型由同一注册表链路上的 `MLR1 MIG` OWNER/PREPARE/EVENT/COMMIT/ABORT/RETIRE/STATUS 驱动（MLS1 门禁、按请求 ID 幂等、注册表驱动的退役）；状态仍仅存于内存，无共享归属权威。 | [process-roles.md 当前实现](../process-roles.md#当前实现)、todo.md:48–51 |
| 回传与数据 | `MLD1 LOAD/SUBMIT/FENCE` 是 `data` 上的帧式 TCP 端点；它执行 generation 栅栏、按 `(realm, character)` 连续序号及重复回执。`FENCE` 冻结最终游标并把旧 generation 墓碑化；`RECEIVED` 只是进程内回执；带 `--journal FILE` 才有存储后端的 `ACK durable` 并跨重启恢复（含最终游标 pin），无存储时端点不声明持久。每参与者有序发送队列边界已实现但尚未接入 crossServer 运行时。 | [cross-return-protocol.md 回传与持久化确认](../cross-return-protocol.md#回传与持久化确认-mld1)、[service-contracts.md data](../service-contracts.md#data)、[validation.md MLD1 行](../validation.md) |
| 实体契约与同步 | XML `.def` 已生成 XSD/lock/digest、sproto 及 C++/JS/C# 原型绑定，`contract compat` 比较发布锁。已有上游线向量、派发器与进程内实体/RPC 夹具。服务端真实传输绑定（`--contract` 节点在脚本之前应答 `MLE1` 帧，脚本 `entities` 表绑定到派发器）于 2026-10-29 落地，普通网关关联号改写（`gateway` 节点经有界表把客户端外层传输关联号换成全新内层实体 id，表满或重复时不经转发直接答 `CAPACITY`）于 2026-10-30 落地；生产 codec、跨网关关联号改写与宿主路由、属性复制与持久化仍开放（缺口 7）。 | [entity-contract.md](./entity-contract.md)、[实现证据](./bigworld-like-protocol.md#已实现状态与证据)、[validation.md 契约/SDK 行](./validation.md) |
| 服务契约 | `login` 是面向客户端的脚本角色与 MLR1 分配客户端；`data` 是归属侧记录端点；可选 `apiServer` 是外部 HTTP 入口。MLS1 是服务链路身份。login 凭据已决定为 HMAC-SHA256；MLT1 的 C++ 签发/验证原语已实现、凭据随 `MLC1` 的 `tk` 尾缀上行，`MISTLANE_TICKET_SECRETS` 下的 login 节点经 `mistlane.ticket.issue` 签发，同密钥的 crossServer 在握手上验证并写 `context.verified`。 | [service-contracts.md](../service-contracts.md)、[process-roles.md 跨区数据归属](../process-roles.md#cross-region-data-ownership) |
| HTTP 与错误边界 | HTTP gateway 将请求/上游结果映射为 HTTP 状态；apiServer hooks 与 WebSocket callbacks 定义请求/会话级错误处理。EntityError 是另一套封闭 RPC 错误词表。MLR1/MLD1/MLS1/MLC1 错误各自属于协议，不能视为共享的 EntityError 枚举。 | [http.md 服务端与 Lua HTTP handlers](../http.md)、[entity-contract.md 调用 envelope 与错误协议](../entity-contract.md#invocation-envelope-and-error-protocol-decided-unimplemented) |

## 一致性核查

固定沿用原始审计 `a429664` 的编号：4 条矛盾、7 条缺口，含已解项，解决后不重排。原文中**归属权执行是缺口 4，coordinator 路由与归属是缺口 5**；本次两者与缺口 2、6、7 一并核查。A = 改文档消解；B = 改代码消解；C = 保留并写清未来工作。既有 B 实现只记对应提交，不重做。

**终态：矛盾 4 条（已解 4 / 未解 0）；缺口 7 条（已解 2 / 部分 5 / 未解 0）。** 消除矛盾承诺不等于实现崩溃恢复；部分/未解运行时工作仍受下述闸门约束。

### 矛盾

1. **序号顺序——已解，B（`b46be86`，此前已完成）。** 发送队列按 `(realm, character)` 串行发送连续序号，与端点 `stored + 1` 校验一致。`RECEIVED` 释放发送槽，变更仍保留到持久确认。证据：`include/mistlane/record.hpp:135–151,163–244`；`cross.return.queue`、`cross.record`。
2. **凭据状态——已解，A。** 原文：“入场凭据是否签名、由谁签发”仍待决定。改后：签发方已定为 login，以区服密钥签发短时效 HMAC-SHA256 凭据；C++ 签发/验证原语已作为 MLT1 实现（见下），线形已绑定到 `MLC1` 的 `tk` 尾缀，秘钥配置、login 侧签发与宿主验证已实现，持久声明表仍未实现，当前上下文仍未获持久写授权。证据：[参与者上下文](./cross-return-protocol.md#参与者与返回上下文)、[已定决策](./cross-return-protocol.md#已定决策与剩余工作)，与既有决定 `fe8b2a6:docs/service-contracts.md:10,23,26,58` 对齐。
3. **回传协议是否已定——已解，A。** 原文（`fe8b2a6:docs/zh/architecture.md:21`）：“具体协议与确认语义待实现”。改后：回传协议/确认语义已定；LOAD/SUBMIT/RECEIVED、有序队列、持久 ACK codec 与租约模型已实现，存储、持久 ACK 发出和运行时接线未实现。证据：[架构](./architecture.md#目标游戏服务架构)、`b46be86`（队列）、`fe8b2a6`（ACK/租约）、`include/mistlane/record.hpp:52–69,108–159,247–264`。
4. **崩溃/重启后的去重持久性——文字已解，A；持久恢复保留，C。** 原文：“归属侧重启后看到重复并按序号去重”，丢 ACK 重发后“队列排空”。改后：发送方崩溃会丢失进程内队列；仅当应用重建相同提交、同一 data 进程仍存活且 generation/最新序号匹配时，才去重为 `RECEIVED`。data 重启丢失载荷、游标与栅栏并答 `EMPTY`；更旧序号答 `ERR SEQ`。回执和崩溃均不意味着持久恢复。证据：[故障处理](./cross-return-protocol.md#故障处理)、`include/mistlane/record.hpp:121–159,163–244`；`src/test_cases.cpp:1867–1872` 只验证新 data 进程，未验证持久重启。**恢复未解·原因：**无归属侧持久事务及可恢复发送端重放来源。**解锁条件：**原子持久化状态/游标/generation/墓碑，以丢持久 ACK、data/发送端重启测试证明恢复后才扩大承诺（缺口 3）。

### 缺口

1. **身份/去重键中的 realm——已解，B（`b46be86`，此前已完成）。** 端点栅栏/去重与发送队列均按 `(realm, character)` 建键。现有状态易失，无在线迁移；未来持久 character-only 键须回填 realm 或解决冲突。证据：`include/mistlane/record.hpp:105–113,244`；`cross.record`、`cross.return.queue` 的跨区隔离断言。
2. **进入上下文完整性——契约已解，A。** 原文（`fe8b2a6:docs/zh/service-contracts.md:22`）：“来源区服、角色标识与返回路由”。改后：响应补齐归属 data 签发的承载 generation 与 login 凭据。[login 生命周期](./service-contracts.md#login)明确分配 → data 授权 → 凭据/响应 → 接收宿主验证：新键从 1 开始；重新分配/接管须分配严格更大 generation 并按交接顺序栅栏旧授权；中止分配不复用。凭据同时绑定 generation、realm/character/target/expiry/nonce。证据：`include/mistlane/cross.hpp:19,63`、`src/lua_service.cpp:229–234` 已传递 generation；`fe8b2a6` 已定 data 为 generation 权威。授权线消息与运行时有效授权校验仍归缺口 4（密码原语已作为 MLT1 落地），不冒充已实现。
3. **持久确认应答格式——部分解，既有 B（`fe8b2a6`）+ C。** 格式/编解码/发送端校验已解：`MLD1 ACK durable <realm> <character> gen <n> seq <m>`，丢 ACK 按原幂等键重放。证据：`include/mistlane/record.hpp:52–69,228–236`；`cross.return.queue`。**未解·原因：**存储后端端点已能发出已提交 ACK 并跨 data 重启重放，但发送端可恢复重放来源仍缺。**解锁条件：**可恢复的发送端重放来源及崩溃测试。存储部分已解（data 持久化切片 2026-10-27）。
4. **归属权执行与端点检查——部分解，A/C（既有模型 B：`fe8b2a6`）。** [交接设计](./cross-return-protocol.md#写入租约交接)已固定顺序、旧源晚到写、在途工作与计时器；`WriterLease` 仅测试本地转换（`include/mistlane/record.hpp`（`WriterLease`）、`src/test_cases.cpp`（`cross_return_queue`））。**未解·原因：**孤立模型不构成共享权威；MLD1 SUBMIT 仍可凭更大 generation 和下一序号被接受（`record.hpp:121–151`），不查 owner 授权；MLS1 只认服务链路。持久租约/墓碑未接入 coordinator/data/Avatar，授权/撤销/到期消息未编码。**解锁条件：**认证且幂等的授权/撤销/接管线消息与持久 home 权威，Avatar 处理器/计时器闸门接线，以及双宿主竞争、到期、旧/伪造 generation 和各阶段崩溃恢复测试。不得凭模型标全解。
5. **coordinator 路由与归属——部分解，B/C（第一片，2026-10-20；持久游标与栅栏后接管模型 2026-10-28；持久场景/放置 2026-11-01）。** 原文（`b35786a`）：“路由只存 scope/role/instance，没有 generation 或迁移事务。”改后：[类型化事务协议与模型](#coordinator-migration-transaction-first-slice)绑定 home 键/请求 ID/generation，比对预期 owner/route，实现 prepare/commit、中止/清理与检查点恢复，拒绝在源排空 → home 栅栏 → 目标确认之前发布路由；显式记录区服/跨服组作用域及角色/实例/场景；[SQLite journal 切片](#sqlite-journal-storage-first-persistence-slice-2026-10-27)在每次线上确认前持久化已接受事务，并连同持久最终序号/墓碑权威把交接跨重启保留为 `RECOVERING`；`takeover()` 是栅栏后以严格更大 generation 继续的显式半步，其注册表线动词 `MLR1 MIG TAKEOVER` 已接（`88c10ae`）；持久放置行现在同时携带承载场景与已发布 generation，崩溃或事务剪枝后 `ROUTE GET` 仍按同一场景/generation 作答。证据：`include/mistlane/migration.hpp`（`MigrationRequest`、`HomeGenerationAuthority`、`MigrationTransaction::retire_member`、`MigrationTransaction::takeover`）、`include/mistlane/record.hpp`（`WriterLease::drain_requests`、`WriterLease::takeover`）、`include/mistlane/journal.hpp`/`src/journal.cpp`（`MigrationJournal`、`PlacementRecord.scene`、v1→v2 放置迁移）、`src/test_cases.cpp` 十例 `coordinator.migration.commit/abort/recover/stale/order/retire/wire/journal/journal.recovery/authority` 加 `coordinator.placement.schema`。**未解·原因：**coordinator 意图、generation 消费、栅栏后的交接与场景/放置归属已跨重启保留，但真实 coordinator/data/Avatar 路由接线仍缺。**解锁条件：**真实 coordinator/data/Avatar 和路由接线，以及跨服务的崩溃/重复/成员退役/目标失效测试（home 栅栏前的退役/目标失效半片、栅栏后接管模型及其线上动词、data 记录半片均已落地）。MLR1 保持目录职责，home data 保持 generation 权威；路由不授予写租约。
6. **apiServer 回调受理——部分解，A/B（第一片与 HTTP 传输均为 2026-10-20；持久后端 2026-10-29）。** 原文：「可信网络与请求级处理；平台签名/规范字节、密钥轮换、防重放窗口、幂等键作用域、持久受理响应及归属查询/重试消息均未定；login HMAC 决策不能替代 HTTP 回调认证。」改后：[类型化外部 API 协议与模型](#external-api-surface-first-slice)已定——规范 `MLA1` 字节 + 按 `(principal, key_id)` 的 `HMAC-SHA256` 凭据（含 realm 授权）；绝对 ±300 秒时间窗与按 `(audience, principal, nonce)` 的重放声明（时钟回拨即 fail closed）；按 `(audience, principal, (realm, character), idempotency_key)` 的原子 create-or-read 受理，区分 `Inserted`/`Existing`/`Conflict` 且重试取回同一回执；只读归属查询 `(realm, character) → owner/generation/状态`；[HTTP 传输](#http-transport-first-slice-2026-10-20)——HTTP 监听器上的 `POST /mla1`、逐字节严格解析、五词框定应答、结论到状态码映射，凭据仅由环境变量注入；以及[持久受理后端](#http-transport-first-slice-2026-10-20)——`--journal FILE` 每条受理行先提交 SQLite 再应答 `DURABLE`，重启恢复后丢回复重放仍去重。证据：`src/api.cpp:30–47`（规范字节/HMAC）、`src/api.cpp:48–61`（重放声明）、`src/api.cpp:83–97`（原子受理）、`src/api.cpp`（`parse_api_body`/`api_reply_body`/`api_http_status`）、`src/node.cpp`（`/mla1` 拦截、`MISTLANE_MLA1_*` 加载）、`include/mistlane/migration.hpp:119–129`（归属事实）、`include/mistlane/api.hpp:22,33`（`ApiRequest`/`ApiCredential`）、`include/mistlane/api_store.hpp`（`ApiSqliteAcceptanceStore`）、`api.signature/tamper/replay/rotation/acceptance/acceptance.sqlite/ownership/wire/http`（`src/test_api.cpp`、`src/test_cases.cpp`）。**未解·原因：**验证器的重放声明仍为进程内易失状态——无共享/持久声明存储，归属查询在接入 home 事实来源前只能答 `NotFound`。**解锁条件：**多进程重放声明、接入 home 事实的归属来源与 owner 迁移后的重试测试。HTTP 2xx 不能证明业务持久，但 `DURABLE` 应答可以。
7. **实体发布闸门——部分解，A/C（既有实现 `3326fd4`、`1048bcd`、`c0d9d82`、`49c59c2`、`dec204a`）。** 原文：“没有发布兼容流程”，没有运行时/黄金验收物。改后：`contract build` 已确定性生成 XSD/lock/digest/sproto/C++/JS/C#；`contract compat OLD.lock NEW.lock` 检查新增/删除/改分配（`src/contract_tool.cpp:57–77,123–143`）；已有上游 sproto 向量及派发器/实体/RPC 夹具（`include/mistlane/entity_runtime.hpp:11–72`、[验证记录](./validation.md)）；invocation envelope/error 规范编解码已进程内实现并带 hosting-generation 栅栏（`include/mistlane/entity_envelope.hpp`），INT64 边界/缺省/未知字段/畸形向量、嵌套结构/整数数组/空数组向量已在 C++/Lua/JS/C# 四语言锁定（JS/C# codec 已 schema 驱动），含上游 `todword` 解码缺陷的复现与规避（2026-10-20，[验证记录](./validation.md)）。**未解·原因：**参数/标量向量不覆盖完整 invocation/error envelope；EntityRef 参数仍映射为整数（`entity_dispatch.hpp:74–79`）；网络加载/路由、JS/C# UTF-8/bytes/BOOL codec 覆盖、嵌套复制与持久化仍缺失。**解锁条件：**冻结规范 envelope/error/EntityRef 字节及 C++/Lua/JS/C# 畸形输入向量，将 schema/绑定/锁/向量检查接入强制 bundle-load 闸门；再验证在线路由/deadline/cancel/栅栏及独立的复制/持久化闸门。进程内夹具与通过 lock diff 不能授权生产接入。[实体契约验收顺序](./entity-contract.md#发布与验收顺序)中的过时文字同步修正。

## coordinator 迁移事务（第一片） {#coordinator-migration-transaction-first-slice}

2026-10-20 已实现为 `include/mistlane/migration.hpp` 的类型化 C++ 协议，未新增 CLI 或线上动词。这是 todo:48 的最小确定性实现，不含完整 coordinator 运行时或真实负载均衡。`MigrationTransaction` 每对象建模一个参与者；放在一起的源租约、home 事实与 coordinator journal 代表未来不同的服务边界。既有 MLR1 注册/就绪/负载/角色路由继续承担目录职责，router 不改名顶替 coordinator。

### 身份、作用域与 generation

`MigrationRequest` 即 PREPARE：`(归属区服, character)`、不可变请求 `id`、完整 `expected` 路由 `(owner, generation)` 与目标 owner。owner 含 `scope.kind`、`scope.realm`、`scope.cross_group`、`role`、`instance`、`scene`。区服作用域仅填写 `realm` 且角色为 `gameServer`；跨服组作用域仅填写 `cross_group` 且角色为 `crossServer`；实例/场景均为必需标识。参与者的归属区服独立于承载作用域，迁移保持 home data 身份（也不搬 gateway）。这里记录场景归属标识，不创建场景运行时对象。

只有 home data 签发 generation：`HomeGenerationAuthority::reserve` 按 `(realm, character)` 推进高水位，从可信当前授权初始化（新授权按 login 契约从 1 开始）。coordinator 请求分配，不自造版本；中止也消耗已分配 generation。后续每条事件携带 `MigrationToken {key, id, generation}`：错键 `Invalid`，未知 ID 或版本不符 `Stale`，同 ID 换预期 owner/目标 `Conflict`。同参与者仅一条活跃事务；相同 PREPARE（含终态重放）返回 `Duplicate`，不再分配。`write_allowed(owner, generation)` 必须精确匹配 home 当前授权，查到路由不产生写权。复用 `WriterLease` 的准入/计时器/排空闸门并对齐旧 generation 栅栏；没有为现有 MLD1 端点添加 owner 授权校验（缺口 4）。

### 提交、中止与恢复协议

| 阶段 / 类型消息 | 前置条件与效果 | 重试 / 结果 |
|---|---|---|
| PREPARE / `prepare(request)` | 完整预期 owner 和路由 generation 同时比对 route 与 home owner；分配下一代，暂停源准入/计时器，暂存目标；`idle` 或终态 → `preparing` | 同 ID/内容 `Duplicate`；并行事务或换内容拒绝 |
| `SourceDrained` | 所有已准入处理结束，快照/最终有序变更就绪 | 不授权目标或发布路由；重复确认 `Duplicate` |
| `HomeFence` | 源已排空；未来持久实现提交最终序号及源墓碑；模型交出租约并记录栅栏 | 不可逆，超时/中止均不能恢复旧 generation |
| `DestinationReady` | home 栅栏已完成，目标加载栅栏状态并确认新代起租 | 模型启用目标租约，路由仍指向旧 owner |
| COMMIT / `Commit` | 栅栏 + 目标 ACK + 预期路由比对齐备；一起发布目标路由与提交终态 | `committed`；旧 COMMIT 重复不能盖过后续路由 |
| ABORT / `Abort`，再 `Cleanup` | 仅 home 栅栏前允许：进入 `aborting`，排空剩余处理，丢弃半建目标，恢复源旧代准入/计时器 | `aborted`；处理未排空时清理 `Pending`；保留已消耗的 generation |
| RECOVER / `Recover` | 读取可信事务/home 检查点；栅栏前完成中止清理；栅栏后进入 `recovering`，等待真实目标 ACK 后提交 | 不猜 ACK、不复活旧授权；重复终态恢复 `Duplicate` |

必须满足的发布顺序是：**源停止/排空 → home data 栅栏旧 generation 并提交最终序号 → 目标加载/起租并 ACK → coordinator CAS 发布路由及提交终态**。允许栅栏到目标起租之间暂不可用。栅栏前中止恢复为原路由值（此前始终未改）；栅栏后恢复必须向前完成。目标无法继续时保持 recovering，退役/换目标须通过 home 显式接管签发更大 generation，不能猜测失败或回滚。退役/放弃的半片与栅栏后接管模型均已在下面实现；线上半片仍未实现。

运行时持久契约要求：home 的 generation 预留/最终游标/墓碑与对应 home 事务事实原子记录后才暴露确认，coordinator 意图以及路由/终态持久完成后才确认 COMMIT。`MigrationJournal`（见下）现已覆盖 coordinator 半片、MLD1 记录/ACK 存储与持久 home 最终游标权威——终态 ID、已消费 generation、持久游标与每个已确认 MLD1 载荷跨重启保留。跨服务按同一 token 查询/重放 home 权威事实，coordinator 超时或查不到记录不能证明中止。缺失持久证据时仍须保持不可用直到对账，不能产生第二写者。

五例 `coordinator.migration.commit`、`.abort`、`.recover`、`.stale`、`.order` 分别覆盖正常提交、半途清理与版本不复用、七个崩溃切点重建（含栅栏后等待恢复）、旧版本/旧 COMMIT、提前栅栏/起租/发布及路由 CAS 冲突；均复用现有 `test_cases` 执行器。完整 coordinator/data/Avatar 接线、进程/场景生命周期集成、真实负载均衡和持久 data 侧崩溃/退役验证继续保留。

### SQLite journal 存储（持久化第一片，2026-10-27）

`include/mistlane/journal.hpp` 与 `src/journal.cpp`：coordinator 迁移 journal 现在落 SQLite 文件（`--journal FILE`，仅 coordinator 角色），经静态 `sqlite3` vcpkg 端口接入，单二进制/文件部署不引入外部服务。`MigrationJournal::save/restore` 按 `(namespace, realm, character)` 存一个版本化快照——快照即内存模型的 `MigrationTransaction::Snapshot`（路由/home owner、`WriterLease`、journal、当前请求），coordinator 重启后重建同一未决事务，并从最大已消费 generation 恢复 `HomeGenerationAuthority`。`Registry` 在 `MLR1 MIG` 的 `OWNER`/`PREPARE`/`EVENT`/`COMMIT`/`ABORT`/`RETIRE` 每次接受后先落盘再答，SQLite 无法提交时回 `MLR1 ERR STORAGE` 并关连接；启动监听前先恢复全部事务。终态 ID 与中止 generation 均不删除。`coordinator.migration.journal` 对同一文件做两次真实 coordinator 重启：栅栏后未决事务恢复后按原 generation 完成 `READY`/`COMMIT`，下一 PREPARE 分配严格更大 generation；中止/清理终态行跨重启恢复且不重用已消费 generation。

启动现在是确定性回放（持久化第二片，2026-10-28）：`MigrationTransaction::recover()` 强排崩溃源的债在途租约，栅栏前工作立即回滚并恢复源旧代授权，栅栏事务停在 `RECOVERING` 直至目标重发 `READY`（或补完成已 `ACK` 的提交），终态快照是幂等 no-op。`Registry` 在接受流量前执行回放并写回 recovered 快照，恢复过程再崩溃也不会重扫崩溃前状态；版本/键非法仍 fail-closed 拒绝启动。`coordinator.migration.journal.recovery` 覆盖 prepare/drained/fenced/ready/committed 五段 cuts、恢复后二次回放幂等与已消费 generation 纪律；ctest 现为 60/60（MariaDB 仍按仓库惯例排除）。

home 最终游标/墓碑权威随第三片（2026-10-28）持久化：`HomeGenerationAuthority::View` 暴露每 key 一个高水位游标、`restore()` 单调归并、`allocated(key,g)` 声明 g ≤ 游标的 generation 均已消费。`MigrationJournal` 新增 `home_authority` 行，主键 `(namespace, realm, character)`，并与其迁移快照在同一 SQLite 事务提交；游标落后快照即拒绝而非重导。启动先读权威表再逐作用域键独立恢复，旧 journal 无权威行时按高水位一次性回填。`coordinator.migration.authority` 覆盖中止不复用、重启恢复、下一严格更大 generation、旧回填与损坏 fail-closed；ctest 现为 61/61（MariaDB 仍按仓库惯例排除）。

`--journal FILE` 同时也是 data 角色的记录存储（data 持久化切片 2026-10-27）：同一 SQLite 文件携带 `record_store` 表，主键 `(realm, character)`，版本化 BLOB 快照（`generation`、`sequence`、payload）。`RecordStore` 仅在 `MigrationJournal::save_record` 提交后接受 `MLD1 SUBMIT`，对已提交行（含丢 ACK 重放）答 `MLD1 ACK durable`，提交失败回 `MLD1 ERR STORAGE` 且不改内存。data 重启先恢复全部记录再开监听；无 `--journal` 时保持仅易失 `RECEIVED` 路径。`cross.record.journal` 覆盖真实重启，`core.contracts` 覆盖存储/ACK 语义（cross 套件 5/5，ctest 计数升至 58）。

### 成员退役、目标失效与接管（第二片）

2026-10-20 在同一模型内实现，仍未新增 CLI 或线上动词。`retire_member(token, member)` 表示某成员退出注册表而事务仍依赖它，是上文所述接管交换中 home 栅栏前的一半。

声明先校验再采信：键必须与事务一致、generation 必须与 journal 条目一致（否则 `Stale`），事务必须是当前且非终态（否则 `Conflict`），`member` 必须是本事务真正依赖的合法 owner——预期源 owner 或目标。与事务无关的成员、不可识别的 owner 声明、跨区键一律 `Invalid`。

| 退役的成员 | 效果 |
|---|---|
| 源，home 栅栏前 | 源进程已不存在，其在途处理永远不会自行结束。`WriterLease::drain_requests` 强制完成它们（租约离开 `Requested` 后拒绝），随后事务一步中止：丢弃半建目标，恢复源的旧 generation 授权、准入与计时器。路由保持原值不变 |
| 目标，home 栅栏前 | 源仍然存活，其处理仍持有准入权。先答 `Pending` 等它结束，之后完全按普通中止收尾。事务绝不推进，且在 handoff 处于 `Requested` 的整个期间源的写入保持冻结 |
| 栅栏后（任一成员） | `Pending`，零改动。归属保持 `Fenced`、旧授权保持已交出，`Recover` 仍等真实目标 ACK——继续前进的唯一方式是显式接管（见下） |

「已消耗 generation」规则才是让接管安全而非取巧的关键：中止会让已分配的 generation 作废，之后同一路由的 PREPARE 分配下一个 generation（测试中 9 → 10）并提交到一个**存活**的目标。不猜测、不回滚、不跨越栅栏复活；`drain_requests` 在墓碑之后也不可能执行。

**栅栏后接管（模型半片，2026-10-28）。** Fenced 缺口没有 owner，也不能从死目标推断成员，因此 `takeover()` 是解栅栏的显式步骤：先从持久权威（`HomeGenerationAuthority` 永不复用已消耗 generation）保留严格更大 generation，再继续交接到一个存活目标。源已排空且授权已交出，新事务从普通事务 `HomeFence` 之后的位置开始——`source_drained`/`home_fenced` 继承，被取代的 Fenced 事务烧成 `Aborted`，租约保持已交出并仅把 `accept` 重定向（`WriterLease::takeover`），因此在目标 ACK 前不存在任何写入者。拒绝矩阵与 `prepare` 一致：重放幂等、未栅栏或已提交事务为 `Conflict`、非该 Fenced 路由为 `Stale`、新目标为源或已死目标为 `Invalid`。

`coordinator.migration.retire` 覆盖上表三行、租约级强制排空断言、拒绝矩阵（无关成员、畸形 owner、旧 generation、跨区键、终态事务）、栅栏后以严格更大 generation 接管到存活目标，以及跨退役恢复的检查点不会重发 generation。ctest 61/61。

### 注册表线动词（`MLR1 MIG`，第三片 2026-10-20） {#registry-wire-verbs-mlr1-mig-third-slice-2026-10-20}

2026-10-20 实现于 coordinator 注册表链路（`src/node.cpp` 的 `migration_line` 与 `Registry` 迁移表）：上述模型现已可经分帧 TCP 到达，与 `--service-token` coordinator 上的其他注册表帧一样受 MLS1 门禁。home 事实随 `OWNER` 到达，每个 `(namespace, realm, character)` 只采纳一次——重申必须逐字重复，任何不一致回答 `ERR CONFLICT`；coordinator 记录事实，但从不充当 generation 权威。各动词按请求 ID 幂等：重放帧重复其判决（`PREPARED <id> <generation>`、`STEP`、`COMMITTED`、终态 `ABORTED`）而不预留新的 generation。

| 动词 | 行 | 判决 |
|---|---|---|
| `OWNER` | `MLR1 MIG OWNER [NS <ns>] <realm> <character> <gen> <owner>` | `OWNER OK`；与既有事实不一致 `ERR CONFLICT`；模型会拒绝的形状 `ERR INVALID` |
| `PREPARE` | `... PREPARE [NS <ns>] <realm> <character> <id> <gen> <expected> <destination>` | `PREPARED <id> <next-gen>`；`ERR EMPTY`（无事实）、`STALE`（从未发布的预期路由）、`CONFLICT`（改动请求体或并发请求）、`ORDER`（源仍在执行） |
| `EVENT` | `... EVENT [NS <ns>] <realm> <character> <id> <gen> <DRAINED\|FENCE\|READY\|CLEANUP>` | `STEP <id> <STATE>`；栅栏后工作等待中 `PENDING <id>`；跳步 `ERR ORDER` |
| `COMMIT` | `... COMMIT [NS <ns>] <realm> <character> <id> <gen>` | `COMMITTED <id> <published-owner> <generation>`（含重放） |
| `ABORT` | `... ABORT [NS <ns>] <realm> <character> <id> <gen>` | `ABORTING <id>`（回滚已排程）；终态后 `ABORTED <id>` |
| `RETIRE` | `... RETIRE [NS <ns>] <realm> <character> <id> <gen> <owner>` | `ABORTED <id>` / `PENDING <id>` —— 与 `retire_member` 相同的裁定 |
| `TAKEOVER` | `... TAKEOVER [NS <ns>] <realm> <character> <id> <gen> <expected> <destination>` | `TAKEOVER <id> <next-gen>` —— 续行栅栏间隙，按请求 ID 幂等；非栅栏路由 `STALE`、以源或死目标为新目标 `INVALID`、非栅栏间隙 `CONFLICT` |
| `STATUS` | `... STATUS [NS <ns>] <realm> <character>` | `STATE idle <owner> <gen>`、`STATE <id> <STATE> <drained\|open> <fenced\|unfenced> <ready\|notready> <gen>` 或 `MLR1 EMPTY` |

`NS <ns>` 组在每个动词上都是可选的，紧跟在动词之后——省略或写 `-` 即默认组；这里拒绝裸位置参数命名空间，使按两作用域语法写出的帧仍指默认组。`<owner>` 形如 `<realm|cross> NS <ns> <scope> <role> <instance> <scene>`，与注册表动词同一组写法，因此带命名空间的 owner 不会与默认组的同名。标识符沿用注册表的 `[A-Za-z0-9_-]` 上限（64）。行长上限 1024 字节（小型注册表帧为 256），形状拒绝沿用注册表词汇（`ERR INVALID`/`ERR UNEXPECTED`）；成员离开注册表会退役认领了其 `(namespace, scope, role, instance)` 的在途事务——coordinator 从成员资格观察到这一点，无需任何消息。

**命名空间作用域（2026-10-27）。** 每个注册表动词都加入了命名空间：`REGISTER` 与 `QUERY` 取可选组（位置参数 `<namespace>` 或 `NS <namespace>`，`-` 为默认组），`PLACE` 取 `<role> [<ns>|NS <ns>] <scope>`，`ROUTE SET`/`GET` 取**必填**的 `NS <ns>` 组——旧的两参数 `ROUTE GET` 会得 `ERR INVALID`，而不是被当成空命名空间——`MIG` 各动词在动词后取可选组，如上。`QUERY` 与 `PLACE` 上省略组同样意味着默认组，绝不是跨组列表，因此不同组的两个 coordinator 没有任何共享的查询面。路由记录与迁移事实以 `(namespace, realm, character)` 为键；成员资格、路由与退役声明以 `(namespace, scope, role, instance)` 为键，因此成员离开只退役认领了该命名空间身份的在途事务。成员用 `--namespace NAME` 选择组（仅在 gameServer/crossServer/login 有效，与 `--scope` 一并要求 `--coordinator`），`mistlane.place(role, namespace?, scope?, callback)` 可查询具名组——在空组查询得 `EMPTY`，绝不报解析错误。

线协议继承模型的边界，不变：迁移线在 MLS1 之外不做对端认证，journal 与 generation 均易失，coordinator 从不自行发送事件（DRAINED 是源的断言，EVENT 不区分发送方）。栅栏后间隙只能由显式 `TAKEOVER` 动词在线上续行，调用方须点名栅栏路由与存活目标——coordinator 自身绝不从死成员推断 owner。`coordinator.migration.wire`（其原始切片 ctest 56/56，现属 61/61 套件）覆盖完整 OWNER→COMMIT 正向路径、重放/过期/冲突/顺序拒绝、经 CLEANUP 完成的中止（generation 已消耗）、被 SIGTERM 成员的注册表驱动退役、两种 RETIRE 拒绝、TAKEOVER 拒绝矩阵与续行（un-fence→READY→COMMIT 落到存活目标、取严格更新 generation）、tokened coordinator 上 MLS1 门禁与其自身事实，以及形状/负载拒绝矩阵。

## 外部 API 面（第一片） {#external-api-surface-first-slice}

2026-10-20 已实现为 `include/mistlane/api.hpp` + `src/api.cpp` 的类型化 C++ 协议，并于同日接通 HTTP 传输（见下）。与上文迁移切片一样，本片为 apiServer 契约定稿线语义与最小确定性实现（`ApiVerifier`、`ApiReplayGuard`、`ApiAcceptanceStore`、`ApiSurface`）；HTTP 面是 Lua 路由旁的原生 `POST /mla1` 拦截。2026-10-29 起 `--journal FILE` 可选择持久受理后端；归属事实来源仍未接线。凭据（`ApiCredential`，32 字节秘钥）由宿主应用从环境变量/本机私有配置注入——仓库内没有真实秘钥，聊天与代码中出现的均非凭据。

### 请求字段与签名

`ApiRequest` 对恰好十个字段签名；规范字节串以 `MLA1` 幻数开头，随后按顺序对每个字段追加 `u32 BE 字节长 + 精确字节`——长度框定保证字段值不会串扰相邻字段：

| # | 字段 | 形式 | 规则 |
|---|---|---|---|
| 1 | `operation` | `ACCEPT` \| `OWNER` | 选择受理还是只读查询 |
| 2 | `audience` | 标识符 | 必须等于验证器 audience |
| 3 | `principal` | 标识符 | 凭据查找键 |
| 4 | `key_id` | 标识符 | 凭据查找键（轮换标签） |
| 5 | `timestamp` | u64 秒 | 绝对请求时间，见时间窗 |
| 6 | `nonce` | 恰 32 个小写十六进制字符 | 调用方 CSPRNG；防重放声明键 |
| 7–8 | `(realm, character)` | 标识符 | 归属键；`realm` 必须在凭据 realm 集合内 |
| 9 | `idempotency_key` | 标识符（仅 `ACCEPT`） | 受理去重作用域，见下表 |
| 10 | `payload` | 不透明 ≤ 1 MiB（仅 `ACCEPT`） | 随回执透传 |

签名为 `HMAC-SHA256(规范字节, 秘钥)`，常数时间比较。选 HMAC-SHA256 对齐本仓 OpenSSL 栈，MLA1 与 login 凭据都经同一共享 `hmac_sha256` 原语对精确字节签名；按 `(principal, key_id)` 的凭据行标记轮换，轮换策略已决定：行上可选 `valid_until` 让旧密钥到该秒（含）为止仍可验证，而新密钥已在生效，无需协调切换；到期行与不存在的行同样答 `UnknownCredential`，撤销即删除该行——不留墓碑，陈旧调用方得不到“不是活密钥”之外的任何信息。时间窗+nonce 优于调用方序号栅栏的理由：平台回调来自互不共享序号状态的独立运营方，绝对 ±300 秒窗加按 `(audience, principal, nonce)` 的声明集合不需要跨调用方协调。声明只在 HMAC 通过后查询（伪造请求挤不掉诚实声明），保留至 `timestamp + window`（含端点），时钟回拨时拒绝执行（`Clock`，fail closed）。该守卫是进程内内存；多进程部署需要共享/持久声明。

### 幂等受理与归属查询

| 操作 | 语义 |
|---|---|
| `ACCEPT` | 键 `(audience, principal, (realm, character), idempotency_key)` 上的原子 create-or-read：同键同内容 → `Existing` 并返回原回执；同键换内容 → `Conflict`；新键 → `Inserted`。回执**就是**不可变的已受理命令行（b46be86 式单次变更插入，没有先查后插，也没有可重复执行的业务回调）；一次变更让回执与命令同时可见，恰好一次受理以 `Inserted`/`Existing` 可区分。`durability` 在持久后端提交前为 `Volatile`；`Durable` 只能在持久提交之后报告，含丢回复/重启后的 `Existing` |
| `OWNER` | 只读、确定性的 home 事实查询 `(realm, character) → {owner?, generation, Active\|Draining\|Fenced}`，经 `MigrationTransaction::lookup_ownership`——绝不走 coordinator 路由，不预留 generation、不改租约。栅栏缺口在目标确认前无 owner；源在排空期间仍持有归属 |

校验顺序为：格式校验 → audience → 凭据 → realm 授权 → HMAC → 重放声明；任一步失败确定性地回答 `Invalid`/`UnknownCredential`/`Forbidden`/`BadSignature`/`Window`/`Replay`/`Clock`/`Full`，不消耗任何东西。`ApiSurface::handle` 随后把 `ACCEPT` 分派给受理存储、`OWNER` 分派给归属查询，一次类型化应答同时携带 auth + 受理 + 归属。

调用方：生成 CSPRNG nonce → 填绝对时间戳 → 规范化 → HMAC → 投递。验证方：按上述顺序，再按操作表分派。重放请求在其原声明未过期期间确定性地被拒（`Replay`）；超出时间窗后无论是否重放一律 `Window`。

### HTTP 传输（第一片，2026-10-20）

任何节点 HTTP 监听器上的 `POST /mla1` 会被原生拦截，先于 Lua `http` 回调；其余路径（以及 `GET /mla1`）仍走普通 Lua 路由。请求体是规范字节后接 32 字节原始签名；解析逐字节严格——`MLA1` 幻数、恰好十个框定字段、十进制时间戳（`from_chars`，拒绝溢出）、恰好 32 字节签名且其后不得有任何字节——任何偏差（包括形状合法但请求校验不过的字段）一律回答 `400` 加 `INVALID` 结论体，不会误入脚本。应答体用同样的 `u32 BE` 长度方案框定五个词：auth 结论（`OK`/`INVALID`/`UNKNOWN_CREDENTIAL`/`FORBIDDEN`/`BAD_SIGNATURE`/`WINDOW`/`REPLAY`/`CLOCK`/`FULL`）、受理词（`INSERTED`/`EXISTING`/`CONFLICT`/`FULL`/`INVALID` 或空）、持久性（`VOLATILE`/`DURABLE` 或空）、原样回执载荷（或空）、归属词（`FOUND`/`NOT_FOUND` 或空）。HTTP 状态码跟随结论：校验成功 200；`Invalid` 400；`UnknownCredential`/`BadSignature`/`Window`/`Replay` 401；`Forbidden` 403；`OWNER` 查询答 `NOT_FOUND` 404；`Conflict` 409；`Full`/`Clock` 503（时钟回拨在服务端 fail closed，时间恢复后重试可成功）。

启用只靠环境变量且要么全给要么不给：`MISTLANE_MLA1_AUDIENCE` 加 `MISTLANE_MLA1_CREDENTIAL`，后者为 `;` 分隔的行 `principal:key_id:<64 位小写十六进制秘钥>:realm[,realm...][:until]`（`until` 为十进制纪元秒的轮换上限，缺省或 0 即无界）。只配一半、格式非法或 `until` 非十进制都会拒绝节点启动；两个变量都未设置时 `/mla1` 只是普通 Lua 路由。归属查询在 home 事实有来源之前不接任何东西，`OWNER` 确定性地答 `NOT_FOUND`——绝不编造 owner。原生拦截不产生 Lua 回调延迟观测；API 面指标仍延后。

apiServer 上的 `--journal FILE` 选择持久受理后端（`ApiSqliteAcceptanceStore`，`include/mistlane/api_store.hpp`）：每条已受理行先提交到 `api_acceptance` SQLite 表再应答，因此 `DURABLE` 只在行落盘后报告，重启即恢复全部行——丢回复重放答 `EXISTING DURABLE`，不会插入第二条待办命令。存储镜像 `ApiMemoryAcceptanceStore` 的 create-or-read 语义（同键同内容 `Existing`、换内容 `Conflict`、新键 `Inserted`），有界（满存储拒绝新键 `Full`，但仍应答已存储的键），并传播任何非唯一性 SQLite 失败（I/O、busy、磁盘满）而非误报成 `Conflict`。

用例 `api.wire` 与 `api.http` 覆盖：两个操作的逐字节往返；完整畸形体拒绝清单（空体、仅幻数、错误幻数、九/十一字段、说谎长度、多余字节、短签名、未知操作、非数字/空/溢出时间戳、非十六进制 nonce）；每个结论的精确应答字节；状态码映射表；以及在已启动 apiServer 上的端到端——签名 `ACCEPT` 先 `INSERTED` 后幂等 `EXISTING`，同键换体 `CONFLICT`，篡改签名 401，`OWNER` 404，垃圾体 400，其余路由与未配置节点一律 Lua 回落，半配置/非法凭据拒绝启动。`api.acceptance.sqlite`（单元）与 `api.http` 的 `--journal` 块（端到端）覆盖持久后端：插入/重试/冲突恰好一次、重启去重、Full 边界，以及真实 apiServer 重启对丢回复重放答 `EXISTING DURABLE`。

未做及解锁条件：多进程重放声明（解锁：共享/持久声明存储加崩溃测试）、接入 home 事实的归属来源与 owner 迁移重试测试（解锁：home-data 读路径）。轮换与撤销已决定并测试（见下）。

### 进入凭据模型（MLT1，第一片，2026-10-20） {#entry-ticket-model-mlt1-first-slice-2026-10-20}

已拍板的 login 凭据以类型化 C++ 模型落地于 `include/mistlane/entry_ticket.hpp`：`EntryTicket` 覆盖（realm、character、目标 instance、承载 generation、有效期、nonce），规范字节采用 `MLT1` 魔数（与 MLA1 相同的 `u32 BE` 长度方案，数字为十进制文本），以共享 `hmac_sha256` 原语按区服秘钥签名——凭据的 realm 决定秘钥表的键，两个区服绝不共享同一验证答案。`TicketVerifier::verify` 按固定顺序确定作答——畸形 `Malformed`、`UnknownRealm`（秘钥查找先于任何签名运算）、`BadSignature`（常数时间比较）、`Expired`（严格过期后拒绝，含有效期末刻内通过）、对照到达身份的 `WrongCharacter`/`WrongInstance`/`WrongGeneration`，最后才是重放声明——任何失败检查都不消耗 nonce。`TicketClaims` 是有界已见声明表：时钟回拨即 fail closed，仅逐出已过有效期的声明。线形已在第二片绑定（见下）：`entry_ticket_hex`/`parse_entry_ticket` 把规范字节加原始签名编码为十六进制，`MLC1` 握手以参与者尾缀的可选最后一组 `tk <hex>` 携带（`include/mistlane/cross.hpp`）；解码失败、超过 1024 个十六进制字符、凭据声明的 character 或承载版本与尾缀不符、或匿名握手携带凭据，一律整次握手按 INVALID，绝不从声称持有凭据的参与者身上剥离凭据。宿主现已运行验证器——第四片把它接到 crossServer 握手路径上，区服秘钥表经环境变量注入（见下）。仍未做的：声明表按进程且易失——重启或多进程部署需要共享持久声明，`Replay` 才算诚实。

`login.ticket` 覆盖：签发/验证往返、对照手拼字节的逐字节规范框定、每个字段都移动签名、签发侧坏形状拒绝（空 instance、零 generation、过短或大写 nonce）与验证器构造拒绝（无秘钥、非标识符 instance）、完整结论阶梯（翻转签名位、失败后诚实重试、重放、错误 character/instance/generation、跨区服秘钥、未知区服）、含期末刻的有效期、确定性双次过期、时钟回拨与声明表容量。

### 进入凭据传输（`MLC1` `tk`，第二片，2026-10-20）

凭据线形为 `MLT1` 规范字节加原始签名后整体十六进制编码，由握手以参与者尾缀的可选最后一组携带（`include/mistlane/cross.hpp`，`entry_ticket_hex`/`parse_entry_ticket` 在 `include/mistlane/entry_ticket.hpp`）。尾缀是全有或全无：凭据解码失败、超过 1024 个十六进制字符、声明的 character 或承载版本与尾缀不符、或匿名握手携带凭据，一律整次握手按 INVALID——上下文绝不会从声称持有凭据的参与者身上剥离凭据。握手长度上限由 256 提高到 1200 字节以容纳凭据形握手；上限仍界定了单次握手的代价。

传输绑定并不声称验证：验证属第四片——配置了秘钥的宿主在握手路径上验证凭据（见下）。即便如此，声明表仍按进程隔离、也没有存储链，验证通过只标记脚本可读的准入资格，不等于已发生的持久写，归属运行时仍归缺口 4。`core.contracts` 覆盖往返、伪造尾缀拒绝清单（奇数长度、非十六进制、字段长度谎报、截断与超长 blob、超上限尾缀）、character/generation 绑定、匿名握手携凭据、错拼尾缀关键字与握手长度上限。

### 宿主侧验证（`MLT1` verify，第四片 2026-10-21）

2026-10-21 实现于 crossServer 握手路径（`src/node.cpp`）：以 `MISTLANE_TICKET_SECRETS` 启动的宿主（逗号分隔的 `realm:<64 位小写十六进制>` 行，经环境变量注入，与 MLA1 凭据同一方式——框架内不落任何加载器、CLI 秘钥参数或真实秘钥）在准入前验证握手的凭据。结论把凭据绑定到参与者家区服秘钥（`ret` 的区服决定秘钥表键）、宿主自身 `--service-name` 与声明的承载 generation；声明表挂在节点上，同一进程内跨连接的重放照样被拒。每个失败结论以一个词拒绝整次握手——`MLC1 ERR TICKET`——上下文绝不从声称持有凭据的参与者身上剥离凭据，跨服 gateway 原样转发该词；具体原因（`replay`、`expired`、`bad-signature`……）只进运维日志。匿名握手照常准入且保持只读；未配置宿主上的凭据同样准入但无任何资格——携带令牌本身从来不是授权。脚本经 `context.verified` 读取结论，仅在本宿主验证通过时为真。

本片不声称：声明表仍按进程且易失，重启后或同区服第二宿主上的凭据重放不被捕获；`verified` 标记的是脚本可读的准入资格，不是已发生的持久写（没有存储链）；也没有任何东西把凭据与 coordinator 的路由或 generation 事实对照——绑定只在握手自己的措辞与凭据之间。坏配置拒绝启动并点名原因：坏十六进制行、空值、非标识符区服、重复区服、缺 `--service-name`、或该变量出现在任何非 crossServer 角色上。`cross.ticket` 覆盖未配置宿主、诚实准入（`verified=true`）、重放、错 instance/过期/错秘钥拒绝、尾缀绑定的 INVALID，以及启动拒绝矩阵。

## 命名与语义已对齐（有明确边界）

- 调用地址为 `scope/type_id/entity_id/incarnation`；`EntityRef` 使用相同元组。`hosting_generation` 是归属栅栏，不是实体 incarnation（`entity-contract.md:243–254`）。
- `realm` / “来源区服” / “归属区服或组”指数据归属域；`scope` 是 coordinator 分配作用域，不能混用（`cross-return-protocol.md:11`、`architecture.md:70`）。
- `RECEIVED` 表示端点已收入进程内存；`ACK durable` 表示归属 data 已提交持久存储，二者明确定义不同（`cross-return-protocol.md:58`、`service-contracts.md:36–41`）。
- 传输关联（`MLC2`）与实体 `correlation_id` 有意独立；协议错误按各自表面区分，不共用 EntityError 码（`entity-contract.md:237–275`）。

## 按 todo 归口的剩余实现闸门

| Todo 范围 | 保留工作 | 审计编号 |
|---|---|---|
| crossServer 回传与唯一写入权（`todo.md:54`） | 归属持久事务/ACK/重放、可恢复发送源、有界队列/只读/最终回服接线、共享租约权威与 Avatar 集成 | 矛盾 4 的恢复部分；缺口 3/4 |
| login/data/apiServer（`todo.md:56`） | 宿主凭据验证与区服秘钥配置、持久凭据声明表与 home-data 授权消息；MLA1 回调协议（签名/防重放/受理/归属已建模、HTTP 传输与持久受理已接通、MLT1 凭据原语已实现并绑定到 `MLC1` 的 `tk` 尾缀、轮换/撤销已决定并测试）的多进程重放声明与归属事实来源 | 缺口 2（运行时归 4）、4/6 |
| coordinator 归属/迁移（`todo.md:48`） | MLS1 之外的对端认证、真实路由/Avatar 运行时接线，以及跨服务崩溃/重复测试 | 缺口 5（类型协议、模型、持久 home 游标/墓碑、注册表链路 `MLR1 MIG` 动词（含注册表驱动退役）、栅栏后接管模型及其线上动词、持久场景/放置归属已实现；运行时证据仍缺） |
| 实体实现链（`todo.md:76+`） | 完整 envelope/error/EntityRef 向量与 bundle-load 闸门、生产 codec/网络运行时、嵌套复制与持久化 | 缺口 7 |

以上均为保留闸门，不是新增实现声明。每个部分/未解项必须连同上面的原因与解锁条件阅读；历史 todo 叙述不能覆盖当前代码证据。

`apiServer` 外部请求数据由应用定义；未来任何数据外发或云备份默认关闭。
