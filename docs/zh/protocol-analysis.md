# 协议面分析

状态：按 2026-10-19 已有设计稿与实现记录进行跨文档核查。本文接续实体、服务、跨服回传与验证分析，不重述各稿设计，也不把文档契约等同于运行时接线。清单区分线行为、已定设计和缺失的运行时工作。

## 协议面全景

| 分层 | 协议面与边界 | 出处 |
|---|---|---|
| 传输与监听 | TCP 使用四字节大端长度帧，载荷上限 1 MiB；WS 消息映射为单个载荷。HTTP/HTTPS 承载请求/响应；WS/WSS 监听器与升级分别进入游戏会话或 Lua HTTP 生命周期。每进程可配置多监听器。 | [architecture.md 执行与连接模型](../architecture.md#execution-and-connection-model)、[http.md 服务端、多监听器](../http.md#server) |
| 传输控制 | 可选 `MLC2` 在载荷内增加 u32 BE 请求 ID（响应回显、推送用 0）；可选 `MLS1` 认证内部服务链路；可选 `MLM1` 在认证后的上游复用逻辑会话。三者属于不同层；`MLC1` 跨服路由不与 MLC2/MLM1 模式混用。 | [architecture.md:66–70](../architecture.md#execution-and-connection-model)、[validation.md 服务身份/多路复用行](../validation.md) |
| 路由与跨服 | `MLC1` 是 gateway 跨服路由/准入握手，可选携带全有或全无的参与者后缀（`ch`、`gen`、`ret`）。它选择静态房间/组亲和与准入，不等于动态角色分配或迁移。`PROBE1` 提供就绪/过载检查。 | [cross-server.md](../cross-server.md)、[cross-return-protocol.md 参与者与返回上下文](../cross-return-protocol.md#参与者与返回上下文)、[architecture-comparison.md:53–60](../architecture-comparison.md) |
| coordinator 控制面 | `MLR1` 在具名作用域内提供成员注册、心跳、QUERY/PLACE 和 ROUTE SET/GET。注册携带就绪/负载；现有路由记录指向当前已注册成员。归属租约与迁移状态机尚未实现。 | [process-roles.md 当前实现](../process-roles.md#当前实现)、todo.md:48–51 |
| 回传与数据 | `MLD1 LOAD/SUBMIT` 是 `data` 上的帧式 TCP 端点；它执行 generation 栅栏、按 `(realm, character)` 连续序号及重复回执。`RECEIVED` 只是进程内回执；没有持久 ACK 或存储链。每参与者有序发送队列边界已实现但尚未接入 crossServer 运行时。 | [cross-return-protocol.md 回传与持久化确认](../cross-return-protocol.md#回传与持久化确认-mld1)、[service-contracts.md data](../service-contracts.md#data)、[validation.md MLD1 行](../validation.md) |
| 实体契约与同步 | XML `.def` 原型固定一部分类型、显式 ID、项目 XSD 和发布锁。已设计的调用 envelope 是帧载荷，含 `EntityRef` 寻址、独立关联 ID、generation/deadline 和封闭 `EntityError`；属性复制与持久化仍属设计/运行时工作。 | [entity-contract.md 定义格式与校验](../entity-contract.md#one-contract)、[调用 envelope 与错误协议](../entity-contract.md#invocation-envelope-and-error-protocol-decided-unimplemented)、[属性复制](../entity-contract.md#automatic-property-replication) |
| 服务契约 | `login` 是面向客户端的脚本角色与 MLR1 分配客户端；`data` 是归属侧记录端点；可选 `apiServer` 是外部 HTTP 入口。MLS1 是服务链路身份。login 凭据已决定为 HMAC-SHA256，但签发/验证尚未实现。 | [service-contracts.md](../service-contracts.md)、[process-roles.md 跨区数据归属](../process-roles.md#cross-region-data-ownership) |
| HTTP 与错误边界 | HTTP gateway 将请求/上游结果映射为 HTTP 状态；apiServer hooks 与 WebSocket callbacks 定义请求/会话级错误处理。EntityError 是另一套封闭 RPC 错误词表。MLR1/MLD1/MLS1/MLC1 错误各自属于协议，不能视为共享的 EntityError 枚举。 | [http.md 服务端与 Lua HTTP handlers](../http.md)、[entity-contract.md 调用 envelope 与错误协议](../entity-contract.md#invocation-envelope-and-error-protocol-decided-unimplemented) |

## 一致性核查

以下是尚未解决的审计项，不是统计每处重复文字。还剩 3 条矛盾与 6 条缺口。若后写的定案与前稿断言相反，计作矛盾；缺少线格式、归属或时序的计作缺口。

### 矛盾（3）

1. **凭据状态：**`cross-return-protocol.md:15,17,77` 说尚无签发方、入场凭据/签名仍待决定；`service-contracts.md:10,23,26,58` 与 `todo.md:56,58` 已决定由 login 签发 HMAC-SHA256 凭据，并明确仅实现未落地。设计决策已定，回传页文字过时。
2. **回传协议是否已定：**`architecture.md:21` 称回传协议/确认语义尚未实现。`cross-return-protocol.md:3,46,74` 与 `service-contracts.md:7` 表明协议及 data 端点归属已定，只是实现不完整。这里把“尚未决定”和“决定但未实现”混为一谈。
3. **崩溃/重启后的去重持久性：**`cross-return-protocol.md:65` 承诺发送方重启后由归属侧去重；`:58`、`service-contracts.md:41` 和 `todo.md:55` 又说明当前记录与去重状态只在进程内，data 重启即 EMPTY。只有持久存储与持久序号状态落地后，重启去重承诺才成立。

### 缺口（6）

1. **进入上下文不完整：**回传参与者结构有 `generation`；`service-contracts.md:22` 的 login 进入响应只列来源区服、角色标识、返回路由。没有写初始 generation 在哪里签发、返回或递增。
2. **持久确认应答格式：**设计提到 `ACK durable`，但 MLD1 语法只列 `EMPTY`、`STATE`、`RECEIVED` 和错误（`cross-return-protocol.md:48–55`）。没有带 realm/character/generation/sequence 的版本化应答格式，也未规定丢 ACK 后如何重放。
3. **归属权执行与端点检查之间有距离：**唯一写入权要求一个可写 Avatar 与显式接管/墓碑；当前 MLD1 只比较 generation 和序号。租约发放/收回/移交消息、过期语义和执行点均未定义。单一 generation 栅栏不等于完整交接协议。
4. **coordinator 路由与归属状态关系不明：**MLR1 ROUTE SET/GET 记录当前在线宿主（`architecture.md:70`、`todo.md:51`）；职责表也把归属/迁移状态列给 coordinator（`process-roles.md:11`）。临时路由、归属侧持久 owner 与迁移状态之间没有编码关系。
5. **apiServer 回调受理协议未定：**`service-contracts.md:52–54` 说明当前信任网络，回调认证/幂等仍开放；缺少签名校验、防重放窗口、持久受理响应或归属查询消息。HTTP 响应生命周期没有定义业务持久性。
6. **实体契约阶段闸门不完整：**XSD、ID/保留锁及 envelope/错误语义已写，但 `todo.md:76–90` 中兼容 diff、EntityRef 运行时语义、编译器/SDK 生成、嵌套同步与 data 持久化仍开放。XML 原型能生成 XSD/lock，却没有发布兼容流程；错误 envelope 没有线编码/黄金字节或运行时绑定。格式审定到运行时接入之间缺互通验收物。

### todo:54 第一切片已解决

- **序号顺序（原矛盾 1）：**发送队列按参与者只发送连续序号，与端点 `stored + 1` 校验一致。`RECEIVED` 释放发送槽，但变更保留到持久确认。
- **realm 键（原缺口 1）：**端点栅栏/去重状态和发送队列均使用 `(realm, character)`。端点状态目前仅在内存中，无在线迁移；将来持久化的旧 character-only 键须回填 realm 或明确解决冲突。

### 命名与语义已对齐（有明确边界）

- 调用地址为 `scope/type_id/entity_id/incarnation`；`EntityRef` 使用相同元组。`hosting_generation` 是归属栅栏，不是实体 incarnation（`entity-contract.md:243–254`）。
- `realm` / “来源区服” / “归属区服或组”指数据归属域；`scope` 是 coordinator 分配作用域，不能混用（`cross-return-protocol.md:11`、`architecture.md:70`）。
- `RECEIVED` 表示端点已收入进程内存；`ACK durable` 表示归属 data 已提交持久存储，二者明确定义不同（`cross-return-protocol.md:58`、`service-contracts.md:36–41`）。
- 传输关联（`MLC2`）与实体 `correlation_id` 有意独立；协议错误按各自表面区分，不共用 EntityError 码（`entity-contract.md:237–275`）。

## 按 todo 归口的未决问题

### todo.md:54 — crossServer 回传与唯一写入权

| 卡点 | 可选方案 | 推荐 |
|---|---|---|
| 序号缺口与乱序描述冲突；端点现要求连续的 `stored+1`。 | A. 保持严格有序，由发送方缓存/重试缺口。B. data 保存有界乱序窗口。 | **A（本片已实现）。** 队列按角色串行提交并等待/重试缺号；端点仍是最终校验方。 |
| 角色键描述未纳入 realm。 | A. 角色 ID 全局唯一、只按 character 建键。B. 全链路按 `(realm, character)` 建键。 | **B（端点和队列已实现）。** 契约只保证区服内唯一，MLD1 已有 realm。 |
| 发送方崩溃恢复承诺假设 data 去重可跨重启，但当前 MLD1 在内存。 | A. 持久化前收窄承诺。B. 同事务持久 inbox/序号与状态。 | **B（推荐，作为实现门槛）。** 状态和序号原子落盘后才回 ACK durable；此前 RECEIVED 不能作为恢复凭据。 |
| 租约/交接没有具体请求应答时序。 | A. coordinator 发 generation，data 侧 CAS 接管。B. 超时/心跳自行推断新 owner。 | **A（本文代决）。** home data 发新 generation 并封存旧值；coordinator 只在来源栅栏、目标加载及确认后发布路由。 |
| 发送队列、只读阈值与最终回服状态未接线。 | A. 每角色有界队列、明确只读与最终序号屏障。B. 丢最旧项或把 RECEIVED 当完成。 | **A（推荐）。** 不丢弃，也不把回执当持久确认；确认前强制改路由仍算回传失败。 |

### todo.md:56 — login/data/apiServer 契约与接线

| 卡点 | 可选方案 | 推荐 |
|---|---|---|
| login 凭据决策没有同步到回传页，密码原语未接线。 | A. login 用区服密钥签短时 HMAC-SHA256 凭据。B. mTLS/PKI 或外部 JWT 服务。 | **A（已经决定，实现待办）。** 同步回传页；票据未验证前参与者上下文不能作为有效授权。 |
| 服务契约进入上下文遗漏 generation，未说明签发时序。 | A. login 分配前向 data 申请/推进 generation。B. 宿主进入时自行生成。 | **A（推荐）。** data 是唯一 generation 权威；票据绑定 realm、角色、目标实例、generation、到期时间和 nonce。 |
| MLD1 没有持久 ACK 格式和存储事务边界。 | A. 状态与序号一并持久提交后 ACK。B. 收到即 ACK。 | **A（已定原则）。** 定义稳定的 `ACK durable gen <n> seq <m>` 应答，在原子提交后发送；`RECEIVED` 保持非终态。 |
| apiServer 回调认证/幂等和持久响应仍未定。 | A. 全交应用自行处理且无框架契约。B. 定义签名校验、幂等键和持久受理边界。 | **B（推荐）。** 支付规则仍由应用负责；成功响应前要求签名通过且幂等受理已持久化。 |

### todo.md:48 与 todo.md:76+ — coordinator 与实体阶段

| 卡点 | 可选方案 | 推荐 |
|---|---|---|
| MLR1 路由没有归属/场景状态迁移协议。 | A. 把 MLR1 注册表扩成游戏状态权威。B. 保持 MLR1 为分配/路由目录，在 home data generation 栅栏外定义迁移状态机。 | **B（推荐）。** coordinator 留在控制面；迁移实现前明确 reserve → prepare → commit/abort 与路由发布顺序。 |
| 实体契约阶段没有从 XML 到运行时的发布闸门。 | A. 直接从现有原型做运行时。B. 先冻结 schema、错误字节、兼容 diff 与黄金向量。 | **B（推荐）。** 按格式审定 → 最小 codec/XSD/lock 兼容原型 → 运行时推进；EntityRef、嵌套复制和持久化分别设闸。 |
| Entity envelope/error 只有语义字段，没有序列化黄金表示；EntityRef 仍只是声明类型。 | A. 各运行时自行约定编码。B. C++/Lua/JS/C# 绑定前定义规范编码与错误向量。 | **B（推荐）。** 保持已决定的字段顺序与封闭错误集，发布测试向量；运行时接入前拒绝未知/畸形形式。 |

`apiServer` 外部请求数据由应用定义；未来任何数据外发或云备份默认关闭。
