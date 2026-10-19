# 协议面分析

状态：2026-10-20 对照 `fe8b2a6` 与下述文档修正重新核查。本文接续实体、服务、跨服回传与验证分析，不把文档契约等同于运行时接线。清单区分线行为、已定设计和缺失的运行时工作。

## 协议面全景

| 分层 | 协议面与边界 | 出处 |
|---|---|---|
| 传输与监听 | TCP 使用四字节大端长度帧，载荷上限 1 MiB；WS 消息映射为单个载荷。HTTP/HTTPS 承载请求/响应；WS/WSS 监听器与升级分别进入游戏会话或 Lua HTTP 生命周期。每进程可配置多监听器。 | [architecture.md 执行与连接模型](../architecture.md#execution-and-connection-model)、[http.md 服务端、多监听器](../http.md#server) |
| 传输控制 | 可选 `MLC2` 在载荷内增加 u32 BE 请求 ID（响应回显、推送用 0）；可选 `MLS1` 认证内部服务链路；可选 `MLM1` 在认证后的上游复用逻辑会话。三者属于不同层；`MLC1` 跨服路由不与 MLC2/MLM1 模式混用。 | [architecture.md:66–70](../architecture.md#execution-and-connection-model)、[validation.md 服务身份/多路复用行](../validation.md) |
| 路由与跨服 | `MLC1` 是 gateway 跨服路由/准入握手，可选携带全有或全无的参与者后缀（`ch`、`gen`、`ret`）。它选择静态房间/组亲和与准入，不等于动态角色分配或迁移。`PROBE1` 提供就绪/过载检查。 | [cross-server.md](../cross-server.md)、[cross-return-protocol.md 参与者与返回上下文](../cross-return-protocol.md#参与者与返回上下文)、[architecture-comparison.md:53–60](../architecture-comparison.md) |
| coordinator 控制面 | `MLR1` 在具名作用域内提供成员注册、心跳、QUERY/PLACE 和 ROUTE SET/GET。注册携带就绪/负载；现有路由记录指向当前已注册成员。归属租约与迁移状态机尚未实现。 | [process-roles.md 当前实现](../process-roles.md#当前实现)、todo.md:48–51 |
| 回传与数据 | `MLD1 LOAD/SUBMIT` 是 `data` 上的帧式 TCP 端点；它执行 generation 栅栏、按 `(realm, character)` 连续序号及重复回执。`RECEIVED` 只是进程内回执；没有持久 ACK 或存储链。每参与者有序发送队列边界已实现但尚未接入 crossServer 运行时。 | [cross-return-protocol.md 回传与持久化确认](../cross-return-protocol.md#回传与持久化确认-mld1)、[service-contracts.md data](../service-contracts.md#data)、[validation.md MLD1 行](../validation.md) |
| 实体契约与同步 | XML `.def` 已生成 XSD/lock/digest、sproto 及 C++/JS/C# 原型绑定，`contract compat` 比较发布锁。已有上游线向量、派发器与进程内实体/RPC 夹具。完整网络 envelope/error 编码、生产 codec、运行时加载、属性复制与持久化仍开放（缺口 7）。 | [entity-contract.md](./entity-contract.md)、[实现证据](./bigworld-like-protocol.md#已实现状态与证据)、[validation.md 契约/SDK 行](./validation.md) |
| 服务契约 | `login` 是面向客户端的脚本角色与 MLR1 分配客户端；`data` 是归属侧记录端点；可选 `apiServer` 是外部 HTTP 入口。MLS1 是服务链路身份。login 凭据已决定为 HMAC-SHA256，但签发/验证尚未实现。 | [service-contracts.md](../service-contracts.md)、[process-roles.md 跨区数据归属](../process-roles.md#cross-region-data-ownership) |
| HTTP 与错误边界 | HTTP gateway 将请求/上游结果映射为 HTTP 状态；apiServer hooks 与 WebSocket callbacks 定义请求/会话级错误处理。EntityError 是另一套封闭 RPC 错误词表。MLR1/MLD1/MLS1/MLC1 错误各自属于协议，不能视为共享的 EntityError 枚举。 | [http.md 服务端与 Lua HTTP handlers](../http.md)、[entity-contract.md 调用 envelope 与错误协议](../entity-contract.md#invocation-envelope-and-error-protocol-decided-unimplemented) |

## 一致性核查

固定沿用原始审计 `a429664` 的编号：4 条矛盾、7 条缺口，含已解项，解决后不重排。原文中**归属权执行是缺口 4，coordinator 路由与归属是缺口 5**；本次两者与缺口 2、6、7 一并核查。A = 改文档消解；B = 改代码消解；C = 保留并写清未来工作。既有 B 实现只记对应提交，不重做。

**终态：矛盾 4 条（已解 4 / 未解 0）；缺口 7 条（已解 2 / 部分 3 / 未解 2）。** 消除矛盾承诺不等于实现崩溃恢复；部分/未解运行时工作仍受下述闸门约束。

### 矛盾

1. **序号顺序——已解，B（`b46be86`，此前已完成）。** 发送队列按 `(realm, character)` 串行发送连续序号，与端点 `stored + 1` 校验一致。`RECEIVED` 释放发送槽，变更仍保留到持久确认。证据：`include/mistlane/record.hpp:135–151,163–244`；`cross.return.queue`、`cross.record`。
2. **凭据状态——已解，A。** 原文：“入场凭据是否签名、由谁签发”仍待决定。改后：签发方已定为 login，以区服密钥签发短时效 HMAC-SHA256 凭据；C++ 签发/验证及凭据传输未实现，当前上下文仍未获持久写授权。证据：[参与者上下文](./cross-return-protocol.md#参与者与返回上下文)、[已定决策](./cross-return-protocol.md#已定决策与剩余工作)，与既有决定 `fe8b2a6:docs/service-contracts.md:10,23,26,58` 对齐。
3. **回传协议是否已定——已解，A。** 原文（`fe8b2a6:docs/zh/architecture.md:21`）：“具体协议与确认语义待实现”。改后：回传协议/确认语义已定；LOAD/SUBMIT/RECEIVED、有序队列、持久 ACK codec 与租约模型已实现，存储、持久 ACK 发出和运行时接线未实现。证据：[架构](./architecture.md#目标游戏服务架构)、`b46be86`（队列）、`fe8b2a6`（ACK/租约）、`include/mistlane/record.hpp:52–69,108–159,247–264`。
4. **崩溃/重启后的去重持久性——文字已解，A；持久恢复保留，C。** 原文：“归属侧重启后看到重复并按序号去重”，丢 ACK 重发后“队列排空”。改后：发送方崩溃会丢失进程内队列；仅当应用重建相同提交、同一 data 进程仍存活且 generation/最新序号匹配时，才去重为 `RECEIVED`。data 重启丢失载荷、游标与栅栏并答 `EMPTY`；更旧序号答 `ERR SEQ`。回执和崩溃均不意味着持久恢复。证据：[故障处理](./cross-return-protocol.md#故障处理)、`include/mistlane/record.hpp:121–159,163–244`；`src/test_cases.cpp:1867–1872` 只验证新 data 进程，未验证持久重启。**恢复未解·原因：**无归属侧持久事务及可恢复发送端重放来源。**解锁条件：**原子持久化状态/游标/generation/墓碑，以丢持久 ACK、data/发送端重启测试证明恢复后才扩大承诺（缺口 3）。

### 缺口

1. **身份/去重键中的 realm——已解，B（`b46be86`，此前已完成）。** 端点栅栏/去重与发送队列均按 `(realm, character)` 建键。现有状态易失，无在线迁移；未来持久 character-only 键须回填 realm 或解决冲突。证据：`include/mistlane/record.hpp:105–113,244`；`cross.record`、`cross.return.queue` 的跨区隔离断言。
2. **进入上下文完整性——契约已解，A。** 原文（`fe8b2a6:docs/zh/service-contracts.md:22`）：“来源区服、角色标识与返回路由”。改后：响应补齐归属 data 签发的承载 generation 与 login 凭据。[login 生命周期](./service-contracts.md#login)明确分配 → data 授权 → 凭据/响应 → 接收宿主验证：新键从 1 开始；重新分配/接管须分配严格更大 generation 并按交接顺序栅栏旧授权；中止分配不复用。凭据同时绑定 generation、realm/character/target/expiry/nonce。证据：`include/mistlane/cross.hpp:19,63`、`src/lua_service.cpp:229–234` 已传递 generation；`fe8b2a6` 已定 data 为 generation 权威。授权线消息、密码原语和运行时有效授权校验仍归缺口 4，不冒充已实现。
3. **持久确认应答格式——部分解，既有 B（`fe8b2a6`）+ C。** 格式/编解码/发送端校验已解：`MLD1 ACK durable <realm> <character> gen <n> seq <m>`，丢 ACK 按原幂等键重放。证据：`include/mistlane/record.hpp:52–69,228–236`；`cross.return.queue`。**未解·原因：**端点仅有易失记录、不发持久 ACK，发送端也不可恢复。**解锁条件：**状态/游标/栅栏原子持久事务，丢响应及 data 重启后重放已提交 ACK，并以崩溃测试覆盖可恢复的发送端重放来源。存储部分按 `fe8b2a6` 原记录继续开放。
4. **归属权执行与端点检查——部分解，A/C（既有模型 B：`fe8b2a6`）。** [交接设计](./cross-return-protocol.md#写入租约交接)已固定顺序、旧源晚到写、在途工作与计时器；`WriterLease` 仅测试本地转换（`include/mistlane/record.hpp:247–264`、`src/test_cases.cpp:1903–1914`）。**未解·原因：**孤立模型不构成共享权威；MLD1 SUBMIT 仍可凭更大 generation 和下一序号被接受（`record.hpp:121–151`），不查 owner 授权；MLS1 只认服务链路。持久租约/墓碑未接入 coordinator/data/Avatar，授权/撤销/到期消息未编码。**解锁条件：**认证且幂等的授权/撤销/接管线消息与持久 home 权威，Avatar 处理器/计时器闸门接线，以及双宿主竞争、到期、旧/伪造 generation 和各阶段崩溃恢复测试。不得凭模型标全解。
5. **coordinator 路由与归属——未解，C。原因：**MLR1 SET 仅查在线成员，GET/退役跟随成员状态（`src/node.cpp:1225–1247,1277–1284,1446–1485`）；路由只存 scope/role/instance，没有 generation 或迁移事务。`fe8b2a6` 已定目标确认后发布路由，但未编码或实现 coordinator 提交/中止/恢复交换。**解锁条件：**绑定 generation 的迁移请求 ID 与预期 owner/route 比对、持久 prepare/commit/abort、data 栅栏与目标确认后的路由发布，以及崩溃/重复/退役测试，证明路由不能授权第二写者。MLR1 保持目录职责，home data 保持 generation 权威。
6. **apiServer 回调受理——未解，C。原因：**[apiServer 契约](./service-contracts.md#apiserver)仍为可信网络与请求级处理；平台签名/规范字节、密钥轮换、防重放窗口、幂等键作用域、持久受理响应及归属查询/重试消息均未定。login HMAC 决策不能替代 HTTP 回调认证。**解锁条件：**选定平台签名/规范化和重放策略，定义持久去重/受理、响应/错误/重试及 home 查询协议，落实事务并测试坏签名、过期/重放、重复/丢回复、重启与 owner 迁移。HTTP 2xx 不能证明业务持久。
7. **实体发布闸门——部分解，A/C（既有实现 `3326fd4`、`1048bcd`、`c0d9d82`、`49c59c2`）。** 原文：“没有发布兼容流程”，没有运行时/黄金验收物。改后：`contract build` 已确定性生成 XSD/lock/digest/sproto/C++/JS/C#；`contract compat OLD.lock NEW.lock` 检查新增/删除/改分配（`src/contract_tool.cpp:57–77,123–143`）；已有上游 sproto 向量及派发器/实体/RPC 夹具（`include/mistlane/entity_runtime.hpp:11–72`、[验证记录](./validation.md)）。**未解·原因：**参数/标量向量不覆盖完整 invocation/error envelope；EntityRef 参数仍映射为整数（`entity_dispatch.hpp:74–79`）；网络加载/路由、hosting-generation 校验、完整 JS/C# codec、嵌套复制与持久化仍缺失。**解锁条件：**冻结规范 envelope/error/EntityRef 字节及 C++/Lua/JS/C# 畸形输入向量，将 schema/绑定/锁/向量检查接入强制 bundle-load 闸门；再验证在线路由/deadline/cancel/栅栏及独立的复制/持久化闸门。进程内夹具与通过 lock diff 不能授权生产接入。[实体契约验收顺序](./entity-contract.md#发布与验收顺序)中的过时文字同步修正。

### 命名与语义已对齐（有明确边界）

- 调用地址为 `scope/type_id/entity_id/incarnation`；`EntityRef` 使用相同元组。`hosting_generation` 是归属栅栏，不是实体 incarnation（`entity-contract.md:243–254`）。
- `realm` / “来源区服” / “归属区服或组”指数据归属域；`scope` 是 coordinator 分配作用域，不能混用（`cross-return-protocol.md:11`、`architecture.md:70`）。
- `RECEIVED` 表示端点已收入进程内存；`ACK durable` 表示归属 data 已提交持久存储，二者明确定义不同（`cross-return-protocol.md:58`、`service-contracts.md:36–41`）。
- 传输关联（`MLC2`）与实体 `correlation_id` 有意独立；协议错误按各自表面区分，不共用 EntityError 码（`entity-contract.md:237–275`）。

## 按 todo 归口的剩余实现闸门

| Todo 范围 | 保留工作 | 审计编号 |
|---|---|---|
| crossServer 回传与唯一写入权（`todo.md:54`） | 归属持久事务/ACK/重放、可恢复发送源、有界队列/只读/最终回服接线、共享租约权威与 Avatar 集成 | 矛盾 4 的恢复部分；缺口 3/4 |
| login/data/apiServer（`todo.md:56`） | C++ 凭据签发/验证、规范凭据传输与 home-data 授权消息；签名/防重放/持久受理回调及归属查询 | 缺口 2（运行时归 4）、4/6 |
| coordinator 归属/迁移（`todo.md:48`） | generation 绑定的提交/中止/恢复消息及路由发布执行 | 缺口 5 |
| 实体实现链（`todo.md:76+`） | 完整 envelope/error/EntityRef 向量与 bundle-load 闸门、生产 codec/网络运行时、嵌套复制与持久化 | 缺口 7 |

以上均为保留闸门，不是新增实现声明。每个部分/未解项必须连同上面的原因与解锁条件阅读；历史 todo 叙述不能覆盖当前代码证据。

`apiServer` 外部请求数据由应用定义；未来任何数据外发或云备份默认关闭。
