# 进程职责与命名

本页是 Mistlane 的目标职责约定。`gameServer`、`crossServer`、`coordinator` 使用本次讨论确定的名称。**职责设计已整理，不代表角色、CLI 或业务能力已实现。** 当前一个 `mistlane` 二进制的实际参数仍见[使用指南](./guide.md)。Apollo 的进程命名不随本页改变。

## 职责与边界

| 目标名称 | 负责什么 | 边界 |
|---|---|---|
| `gateway` | TCP、WS、WSS 游戏接入；连接与会话；按角色当前承载位置转发；下行推送、广播发送、限流和背压 | 持有连接与路由缓存；不持有权威角色玩法数据，不选择副本规则，不修改业务数据库 |
| `login` | 账号验证、角色列表与选择；请求 coordinator 分配承载；进入游戏前的接入流程 | 不执行场景、战斗和持久角色对象逻辑；认证通过不等于角色已在游戏进程就绪 |
| `coordinator` | 维护游戏进程注册、就绪与负载；分配目标进程；维护角色/场景归属及迁移状态；协调切线、切服、跨服与返回 | 不运行场景 tick、怪物或战斗；不转发全部游戏消息；不保存完整角色业务状态；不因心跳超时直接创造第二个权威实例 |
| `gameServer` | 本区服角色对象、常驻地图、动态副本、怪物、战斗、活动、奖励规则；通过 data 加载与保存 | 一个进程可持有多个场景和角色；角色长期数据与场景行为沿老游戏的组织方式由这里承载，不采用 Base/Cell 职责拆分 |
| `crossServer` | 多区服共同参与的场景、副本、战斗与活动；跨服参与者的运行状态、来源信息和返回上下文 | 是跨服玩法执行服；不承担旧 CrossWorld 的全局协调职责；不等于第二套独立账号或玩家数据库 |
| `data` | 按数据归属加载、缓存和持久化；定义事务、并发写入条件、保存结果和错误；对接数据库 | 缓存与数据库职责在此明确；不决定战斗胜负或奖励规则；不能把收到保存消息当成持久化完成 |
| `apiServer`（可选） | HTTP/HTTPS 外部接口与回调、WebSocket upgrade 及连接消息；入口鉴权、原始请求校验、有界递交；调用内部业务服务 | 支付验签由应用实现，受理确认须符合持久化契约；奖励规则交给当前承载业务服务；升级后的帧走 WebSocket 回调 |

`gateway` 是游戏接入职责；`apiServer` 是可选外部系统接入职责，TCP/HTTP/HTTPS/WS/WSS 是监听协议。游戏入口和 HTTP 服务均可按需启用 WS/WSS；目标是独立监听配置及明文/TLS 共存，不是把 WS 单独拆成一个进程。该能力仍待实现。

## 外部接口能力与独立进程

名称采用 `apiServer`（外部 API 服务），与 gameServer/crossServer 的命名方式一致。这里限定为平台回调、支付通知与运营等外部业务接口；游戏客户端的实时消息仍由 gateway 接入。API server 是已有用语，例如 [Nakama 官方服务间通信文档](https://heroiclabs.com/docs/nakama/server-framework/runtime-examples/server-to-server/)描述了在 API server 上注册 HTTP handler；这只作为术语依据，不表示两个项目的部署职责完全相同。

HTTP/HTTPS、WS/WSS 是框架的监听与回调能力，不能仅因启用这些协议就要求多起一个进程。老代码没有统一的外部 API 进程：`d_pay`、`d_payserver` 承担支付职责，`d_world/HttpServer.hpp` 则在 World 内嵌 HTTP。

`apiServer` 表达可选的外部系统接入职责，不是旧进程直接改名。应用需要独立的外部安全边界、容量或发布周期时，可以部署它；简单部署可以在既有服务中启用接入模块。支付业务仍须按应用定义，不把 HTTP handler 等同于支付服务。gateway 接收游戏客户端消息，外部接入模块处理平台回调/管理接口及相应 WebSocket；二者可使用同一种协议，但入口身份与业务权限分别定义。

## coordinator 的部署范围

区服协调与跨服组协调使用同一种职责，可以分别部署为不同作用域的 coordinator。区服实例管理本区服 gameServer；跨服组实例管理该组 crossServer。双方交换分配、迁移与返回信息，不需要一台全局 coordinator 处理所有区服热路径。

coordinator 负责承载决策和注册状态；进程启动、重启与资源限制默认由部署脚本或 systemd 等进程管理工具负责。

目标是每个作用域内的归属变更有明确决策者。高可用时的选主、状态恢复和防止双重承载尚需设计；不能把多个普通 coordinator 实例直接视为高可用方案。

## 主要流程

1. **登录**：客户端经 gateway 到 login；login 请求 coordinator 分配 gameServer；目标 gameServer 经 data 加载角色，创建角色并进入场景；就绪后完成路由发布并通知客户端。分配与加载过程中保留显式状态，不能提前把角色标为游戏就绪。
2. **普通游戏消息**：客户端 ↔ gateway ↔ 当前 gameServer/crossServer。角色和场景逻辑由游戏服务执行；coordinator 不作为每条消息的固定中转站。
3. **同进程切场景**：角色离开旧 Scene、更新位置、进入目标 Scene，并更新必要摘要；没有跨进程角色复制。
4. **跨进程切换**：coordinator 协调目标承载；旧承载停止相关写入并形成可恢复状态；目标接收并确认、路由切换、旧承载释放。超时/断连必须有明确恢复规则，不能同时运行两个可写角色。具体握手与故障恢复尚未实现。
5. **外部回调**：外部平台 → 外部接入模块（可选独立 apiServer） → 持久受理；查询当前承载后调用业务服务，执行业务校验、幂等和持久化，再通知在线客户端。离线时的执行策略需与角色加载/写入权一起设计。

## 跨服的数据归属

进入 crossServer 改变运行位置，不自动改变账号、角色 ID 或数据所属区服。跨服参与上下文需携带可验证的来源区服、角色标识、承载版本及返回信息；路由通过 coordinator 查询/更新，不在订单或长期数据里固化 gameServer 地址。

目标已明确为：角色可转移到 crossServer 执行，长期数据归属仍在原区服，跨服变更通过归属侧服务回传原 data 保存；不要求 crossServer 直接掌握各区服数据库地址。旧实现的 CrossWorld → 归属 World → Data/Cache 路径及其局限见[玩家、场景与跨服模型](./player-scene-model.md)；已定的参与者上下文、唯一写入权与 MLD1 确认设计见[跨服回传与唯一写入权协议](./cross-return-protocol.md)，均未实现。图中的数据访问边表示该服务路径，不代表跨服直接写库或两个角色同时写入。

## 旧代码对应关系

| game-fogs | 目标职责 |
|---|---|
| Gate | gateway |
| Login | login |
| World、CrossWorld 的协调部分 | 不同作用域的 coordinator；旧全局玩法需单独归属，不笼统塞入协调层 |
| Hegel | gameServer |
| CrossHegel | crossServer |
| Data/Cache | data |
| 旧支付服务与 World 内嵌 HTTP | 保留应用业务归属；apiServer 是可选整合设计，不是旧统一接口进程的改名 |

旧切换实现的 `requesthegel2hegel` 实际经过 Gate 更新目标并转发；这属于待分析的既有实现，不承诺目标迁移协议照搬旧时序。

## 当前代码与迁移边界

| 当前 CLI 角色 | 已实现的运行能力 | 尚缺能力 |
|---|---|---|
| `gateway` | TCP/WS/WSS 转发，按连接选择上游，主动推送照常转发；`--routing cross` 开启静态跨服路由/准入 | 玩家身份、动态承载路由、迁移接线 |
| `gameServer` | Lua message 执行，生命周期与重载，直接 TCP/WS/WSS；也保留已有 HTTP 适配 | 完整角色、场景、副本与持久化链路 |
| `crossServer` | TCP 跨服上下文与 Lua 执行、最终连接准入 | 跨服角色迁移、场景与回传保存 |
| `apiServer` | Lua HTTP/HTTPS 请求钩子；不带 transport 时默认 HTTP；可选 WS/WSS message | 平台回调业务、可靠内部调用及多监听 |
| `coordinator` | 仅 TCP 的注册表：成员注册、心跳上报就绪与负载，按命名作用域回答 QUERY/PLACE，断连即退役成员 | 承载决策与归属状态、区服/跨服组作用域、迁移握手、高可用与恢复 |
| `login`、`data` | 名称和职责已登记；CLI 明确拒绝启动并说明未实现；线级契约见[服务契约](./service-contracts.md) | 账号验证、归属数据服务和接线 |

单二进制仍为 mistlane。现有运行角色已迁移，独立通用转发角色已删除，跨服静态入口归入 gateway 的 routing 策略；这不等于完成旧 World 的协调能力。旧角色名称不保留 CLI 别名。MLC1/PROBE1、room/group 键和 max-shard-sessions 配额参数本轮保持原协议含义，它们不是新增进程名称。

## 实施验收

任务见仓库根目录 `todo.md`。按服务身份与通信 → 协调与归属 → 角色/场景 → 跨服移交与数据写入 → 入口和工具的依赖推进。任何运行角色改名都同时更新 CLI 校验、Lua ServiceContext、脚手架、帮助、测试、指标标签及中英文文档；当前已有审核整改任务保持有效。


地图静态数据、场景实例、Session/Avatar 及 C++/Lua 分工详见[玩家、场景与跨服模型](./player-scene-model.md)；gameServer 逐项旧实现评估见[能力评估](./gameserver-capabilities.md)。
