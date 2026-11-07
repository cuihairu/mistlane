# 当前实施顺序



- [x] 审核整改
  证据：2026-10-18

- [x] 多协议监听：游戏服务器与 HTTP 服务均可开启 WS/WSS；支持同进程多个独立监听地址及明文/TLS 共存
  证据：2026-10-19
  - [x] 定义独立 listener 配置、证书与路由绑定；同端口 HTTP→WS upgrade 与不同端口监听均有明确行为，冲突配置启动失败
  证据：commit bd5533e
  - [x] 游戏 WS/WSS 进入游戏会话与 Lua message；HTTP 服务增加 WebSocket 生命周期与消息回调，普通请求保留 Lua http，升级后不走 HTTP handler
  证据：commit bd5533e
  - [x] 增加会话定向推送、发送队列字节上限、慢消费者背压、断连清理及退出策略
  证据：commit 8c36486
  - [x] 验证 TCP + WS + WSS、HTTP + WS、HTTPS + WSS、多个监听同时开启、证书错误、路由隔离与关闭监听；更新脚手架配置和帮助
  证据：commit bd5533e

- [x] 整理目标进程职责及实际实现边界：docs/process-roles.md 与 docs/zh/process-roles.md；首页和架构图移除 router 必经层假设
  证据：commit 8c138c8
- [ ] 按目标职责迁移运行角色（禁止只改名字冒充实现）：gateway、login、coordinator、gameServer、crossServer、data、可选 apiServer；保持单二进制方案可用，目录/target/CLI 一次性迁移并清理别名（部分完成）（BLOCKED）— 跳过：MLD1 唯一写入权/持久化确认/离线回服故障恢复协议已设计并实现（docs/cross-return-protocol.md，含四步移交测试 coordinator_migration_fourstep），但 Avatar 运行时移交（含场景/回服）与 login/data/apiServer 契约运行时验收仍未实现（docs/avatar-runtime.md 无 Avatar 类型），父项仍依赖下述三个未完成子项（跨服移交/结算/回服故障恢复、login/data/apiServer 契约与运行时接线、角色校验/脚手架/文档同步验收）。复核注记（2026-11-06）：所缺条件仍缺——include/src 非测试代码无 Avatar/Scene/Map 类型；docs/avatar-runtime.md 仍明示 Avatar 不存在；子项（行 30/31/34）仍未勾，父项保持 BLOCKED
  - [x] 定义服务身份、会话身份、请求关联和有界双向消息；替换强制一问一答路径，补推送/广播及服务连接复用
  证据：commit 0f720ff
  - [x] 实现 coordinator 的进程注册/就绪/负载、角色与场景归属、迁移状态；区服与跨服组明确作用域，router 不直接改名顶替该能力
    证据：2026-10-20；ctest 69/69
    - [x] 缺口 5 第一片
      证据：2026-10-20；ctest 56/56
  - [x] 明确 gameServer 的角色、场景模板/实例、计时、同步和释放；从旧代码逐项评估通用能力，排除原游戏玩法和配置
  证据：commit 073bd52
- [ ] 实现 crossServer 的参与者来源、场景与返回上下文；长期变更回传归属侧 data；先确定回传与持久化确认协议及唯一写入权，再实现移交/结算/重试/离线/回服故障恢复（BLOCKED）— 跳过：MLD1 唯一写入权/持久化确认/离线回服故障恢复协议已设计并实现（docs/cross-return-protocol.md，含四步移交测试 coordinator_migration_fourstep、sender 重试队列），但 Avatar 运行时移交/结算/回服故障恢复（含场景）仍未实现（docs/avatar-runtime.md 无 Avatar 类型），仍依赖实体运行时链。复核注记（2026-11-06）：所缺条件仍缺——仍无 Avatar 运行时（含场景/结算/回服），无跨服移交端到端实现，保持 BLOCKED
- [ ] 定义 login、data 和可选 apiServer 的服务契约及接线；保留数据归属，禁止订单固化承载进程地址（部分完成）（BLOCKED）— 跳过：login/data/apiServer 契约与接线已定义并部分实现（docs/service-contracts.md；MLD1 记录端点、MLS1 身份、MLA1 持久化接受、entry-ticket 已实现），但登录完整进入流程（data-issued generation 授权与 host 持久化写入授权）与 Avatar 运行时接线仍未实现，且依赖跨服移交完成。复核注记（2026-11-06）：所缺条件仍缺——docs/service-contracts.md 明示 data-issued generation 授权、host 持久化写入授权仍待实现（仅契约），Avatar 运行时接线未实现，保持 BLOCKED
  - [x] 将现有可复用转发/Lua 执行/跨服准入迁入目标职责，CLI 与文档以当前实现为准，不保留旧别名
    证据：2026-10-19
- [ ] 同步角色校验、Lua ServiceContext、脚手架、帮助、指标标签、测试与中英文文档；验证正常登录、双向消息、同进程切场景、跨进程/跨服移交、回调与多监听，运行时验收后再勾选（部分完成）（BLOCKED）— 跳过：跨进程/跨服移交与回调尚未实现，无法做运行时验收。复核注记（2026-11-06）：所缺条件仍缺——跨进程/跨服移交与回调仍未实现，无法做运行时验收，保持 BLOCKED

- [x] 补齐 Session/Avatar、旧跨服回传保存、C++/Lua 实际分工、地图三层数据与大地图边界分析；game-fogs 原始
- [x] 设计地图定义、只读空间数据缓存与可变 Scene 的生命周期；定义准备完成/失败/超时、常驻/临时场景清理、静态数据引用与释放；进入场景或迁移 Avatar 不重复传输/加载整张地图
- [x] 大地图按资源量、活跃实体量、跨进程无缝分别评估；客户端与服务端加载分开，不把实例化、分块、AOI 或 Avatar 跨服移交互相等同。流式加载/空间分区先出需求与设计，不直接实现
  证据：commit 4f11983

- [x] 外部 API 接入职责统一命名 apiServer；中英文职责、首页、架构、协议说明与 game-fogs 对照分析同步；保留可嵌入模块/可选独立部署，不新增默认必需进程
  证据：commit 7655b1a

## 暂缓：实体契约与属性同步草稿


- [x] 编写通用 Entity 契约设计提案：docs/entity-contract.md 与 docs/zh/entity-contract.md；XML .def 为唯一手写契约，生成 sproto、服务器描述及 JS/C# SDK；客户端随契约构建发布，业务契约更新不重编 C++ 服务器。提案不是已实现能力
  证据：commit 46607a0
- [ ] 按可复用库边界实现契约、类型存储、嵌套同步、绑定生成和快照编解码；Mistlane 提供网络/路由/观察者/归属 data 适配，先在仓库内模块化，不另建项目或新增命名（BLOCKED）— 跳过：需先审定实体契约格式并完成最小编解码原型（见下一大项），当前为提案阶段。复核注记（2026-11-06）：所缺条件仍缺——docs/entity-contract.md 仍为设计/提案阶段（定义期实现、进程内 harness、live transport 绑定已落地，但实体契约格式未经最终审定、无最小编解码原型），保持 BLOCKED
- [ ] 按实体契约提案分阶段实现（先审定格式与最小编解码原型，再进入完整运行时）（BLOCKED）— 跳过：未审定格式与最小编解码原型，依赖提案与网络实体运行时；当前仅提案及静态运行时。复核注记（2026-11-06）：所缺条件仍缺——实体契约格式未经最终审定、无最小编解码原型，网络实体运行时未实现，保持 BLOCKED
  - [x] 固定 XML XSD 与语义检查（属性名作为标签、types.xml 复合类型引用、动态标签共享规则及项目 schema 生成）、EntityRef、稳定编号与保留规则、生成锁文件、方法权限、envelope 和错误协议；禁止按遍历顺序重编号
    证据：commit 46607a0
  - [x] 验证 sproto 在 C++/Lua、JavaScript、C# 的同源黄金样本；覆盖 INT64 bigint/long、嵌套结构、缺省值、未知字段和畸形输入，不预设现成 JS/C# 绑定可用
    证据：2026-10-19
  - [x] 实现 contract check/build 工具、可复现生成、版本摘要、兼容报告与服务端 bundle 原子加载；生成物不覆盖业务代码，客户端产物不泄漏内部方法和私有属性
    证据：2026-10-19
  - [x] 实现通用 Entity 身份/生命周期、C++ 描述解释与 Lua 绑定、受控属性及只读保留代理；Avatar、Monster 共用运行时，不限定玩家实体，不引入 Base/Cell 进程
    证据：2026-11-03 核实；entity.properties/entity.tree/entity.endpoint/entity.transport 全绿（Avatar/Monster 同运行时、只读保留代理、Lua 绑定、SIGHUP 替换保留旧服务）；`--contract` 加载于游戏/跨服节点进程内，无 Base/Cell 进程
  - [ ] 实现 JS/C# 直接操作 Avatar 的方法/属性/通知绑定（客户端不暴露 .server/.client 中间层，主要请求通过自己的 Avatar）、内部实体 RPC、身份权限校验、请求期限/取消、明确错误及有界队列；非幂等调用不能超时后静默重试（BLOCKED）— 跳过：JS/C# 绑定生成与完整运行时（网络实体 RPC、权限、有界队列）未实现，非幂等语义未定。复核注记（2026-11-06）：所缺条件仍缺——docs/entity-contract.md 仍明示网络实体 RPC、权限、有界队列及生产 JS/C# codecs 未实现，保持 BLOCKED
  - [ ] 实现全量基线/增量/重连/观察者变更与接收者版本过滤；属性同步为核心机制，同步范围不等于空间 AOI（BLOCKED）— 跳过：观察者/接收者版本过滤及全量基线/重连协议未实现，需先有网络实体运行时。复核注记（2026-11-06）：所缺条件仍缺——观察者/接收者版本过滤及全量基线/重连协议未实现（仅 PropertySync 客户端接收器在进程内），无网络实体运行时，保持 BLOCKED
  - [x] 参考 BigWorld 的嵌套路径与数组切片同步，明确 KBEngine 当前普通容器原地修改的通知边界；实现受控结构/数组所有权链、嵌套增量、插删有序操作、生成的客户端属性回调和整批应用
    证据：2026-10-20
  - [x] 约束属性为有界值树：拒绝循环与初期按值递归、跨属性可变共享，复制赋值，旧容器句柄失效；处理 Lua/协议下标转换、结构变更前后路径合并、积压与重建基线
    证据：2026-10-20
  - [x] 验证嵌套修改自动同步/标记持久化，不靠顶层重新赋值或业务 setDirty；覆盖数组结构变化、旧句柄、混合观察者及在途保存，以大背包单项修改衡量字节/分配/CPU
    证据：2026-10-20
  - [ ] 实现归属 data 加载/版本转换/默认值及持久化快照，generation/revision 条件保存与提交确认；仅一个强制保存入口，禁止发出消息就清空 dirty，索引与数据迁移显式管理（BLOCKED）— 跳过：需先实现网络实体运行时与归属 data 适配，当前归属侧持久化/迁移契约未接线。复核注记（2026-11-06）：所缺条件仍缺——归属侧持久化/迁移契约未接线（PersistentStore 为进程内模型，无存储后端），无网络实体运行时，保持 BLOCKED
  - [ ] 实现契约不变的 Lua 安全替换，以及同二进制新进程加载新契约后的实体迁移、唯一写入权交接、在途请求与计时器处理；现有 SIGHUP 不等于该能力（BLOCKED）— 跳过：实体迁移/唯一写入权交接机制未实现，SIGHUP 仅替换脚本不迁移实体。复核注记（2026-11-06）：所缺条件仍缺——docs/entity-contract.md 明示现有 SIGHUP 不实现该提案，实体迁移/唯一写入权交接未实现，保持 BLOCKED
  - [ ] 验证新旧 JS/C# 客户端、混合观察者、兼容协商、迁移/保存失败与回滚；以相同服务器二进制摘要承载两版契约、新属性与新方法，证明服务器业务协议更新无需重编。（当前门控：条目 118 的实体迁移/唯一写入权交接机制待网络实体运行时；已有可用件（BLOCKED））— 跳过：门控的实体迁移/唯一写入权交接机制未实现，无法端到端验证。复核注记（2026-11-06）：所缺条件仍缺——实体迁移/唯一写入权交接机制未实现，无网络实体运行时，无法端到端验证，保持 BLOCKED

- [ ] Avatar/属性抽象前先补齐客户端协议契约（未完成前不实现新 Avatar API）。已核实旧链路为 sproto，编号表和 PacketHandler 分发见 player-scene-model 文档；不以第三方 Protobuf 依赖替代（BLOCKED）— 跳过：需恢复原始业务协议定义与客户端绑定，缺历史资产。复核注记（2026-11-06）：所缺条件仍缺——仓库内无原始业务 .sproto 源码/客户端绑定资产（仅 build 下测试夹具 avatar.sproto，非业务协议），docs/player-scene-model.md 明示需恢复原始定义，保持 BLOCKED
  - [ ] 定位或恢复原始业务协议定义（含 sproto RPC 协议名/编号、request/response 及引用类型）、生成脚本及正式客户端绑定；确认前后端生成物来源，缺失项明确记录，不将现有二进制 schema 等同于完整生成流程（BLOCKED）— 跳过：原始业务协议定义/生成脚本/正式客户端绑定未能定位或恢复，缺历史资产。复核注记（2026-11-06）：所缺条件仍缺——仓库内无原始业务协议定义/生成脚本/正式客户端绑定（仅 build 测试夹具），缺历史资产，保持 BLOCKED
  - [ ] 列出客户端请求、服务端响应/推送和内部消息的编号、字段编号/类型、方向、处理器、身份/权限、错误码、版本兼容规则及验证方式（BLOCKED）— 跳过：缺原始协议定义与编号表（见上一项），无法列出完整清单。复核注记（2026-11-06）：所缺条件仍缺——原始协议定义与编号表缺失（上一项仍 BLOCKED），无法列清单，保持 BLOCKED
  - [ ] 定义属性声明、增量同步、持久化字段、嵌套变更追踪与周期保存；强制保存只有一个入口，明确其持久化确认、失败与重试语义；撤回 self:sync()，方法名和防覆盖机制参见实体契约提案，尚未实现（BLOCKED）— 跳过：依赖上一项协议清单与属性同步机制，均未完成。复核注记（2026-11-06）：所缺条件仍缺——协议清单（上一项）与属性同步机制均未完成，保持 BLOCKED

- [x] 设计 C++ 通用 Avatar/Session/迁移/加载保存契约及 Lua 生命周期钩子；覆盖错误、超时、重入、迁移期间操作，不能只提供 message 即视为完成
- [x] 文档时效性清理
  证据：2026-10-18


## 合并自 todo.md（2026-10-28，原文件未勾项摘录，随后删除

## H. 发布后整理

- [ ] 为首个公开源码候选确定版本号与发布说明（BLOCKED）— 跳过：需用户拍板版本号与发布时间，且依赖发布页/归档流程就绪。复核注记（2026-11-06）：所缺条件仍缺——RELEASE_NOTES.md 仍为 draft only，无版本号/发布时间，需维护者拍板，保持 BLOCKED

- [ ] 为源码归档附上 SHA256 校验值（BLOCKED）— 跳过：依赖版本号与发布说明确定（上一项），缺用户拍板。复核注记（2026-11-06）：所缺条件仍缺——版本号/发布说明未确定（上一项仍 BLOCKED），无 SHA256 归档，需用户拍板，保持 BLOCKED

- [ ] 检查 GitHub 发布页包含 Apache-2.0 许可证链接和 NOTICE（BLOCKED）— 跳过：需发布页实际存在并核对链接/NOTICE，涉及用户凭证与真实发布。复核注记（2026-11-06）：所缺条件仍缺——仓库无 git tag/发布页，需真实 GitHub 发布与用户凭证，保持 BLOCKED
## J. 崩溃报告、Prometheus、内部探测与性能剖析

- [ ] 为 profiler 工作流提供可重复的跨服压测场景和符号化步骤，记录采样对吞吐、P99 和 RSS 的影响（BLOCKED）— 跳过：需要真实压测集群/profiler 工具链与符号化环境，缺硬件与部署。复核注记（2026-11-06）：所缺条件仍缺——无真实压测集群/profiler/符号化环境，保持 BLOCKED

- [ ] 在目标 Linux/容器环境执行崩溃演练、监控抓取、探针过载演练和压力剖析；未测试前不宣称已解决历史跨服卡死或容量问题（BLOCKED）— 跳过：需真实 Linux/容器环境与监控抓取，缺真实机器。复核注记（2026-11-06）：所缺条件仍缺——无真实 Linux/容器环境与监控抓取，保持 BLOCKED
## K. 项目初始化 CLI 与进程运维模板

- [ ] 按架构比较文档继续补内部 liveness/readiness 探测、Crashpad 转储生成/保留与部署工作流；再依据真实压力测试决定动态 registry、lease/fencing、房间 drain/migration 或容器编排需求（BLOCKED）— 跳过：依赖真实压力测试（上一项）与容器编排决策，均未完成。复核注记（2026-11-06）：所缺条件仍缺——真实压力测试（上一项）与容器编排决策均未完成，保持 BLOCKED

## 审核发现

- [x] 审核发现：4 字节整数解码未做符号扩展，INT32 负值（如 -1）被解码为 4294967295，与上游 sproto expand64 及 JS/C# golden 的有符号扩展不符，导致负 INT32 往返失败并被 value_valid 判 INVALID（include/mistlane/entity_codec.hpp:281）
  证据：2026-10-30；ctest 68/68
- [x] 审核发现：黄金/往返向量仅覆盖 8 字节负整数（-9007199254740993），未覆盖以 4 字节编码的负 INT32（-2147483648..-1），使上述符号扩展缺陷漏检（src/test_cases.cpp:860）。已补：negative32（-1）与 neg_array（-1、-2147483648）向量加入 golden 与 roundtrip，mistlane_entity_codec 全绿
  证据：2026-10-30；ctest 68/68
- [x] 审核发现：valid_utf8 未校验 Unicode 码点上限，接受以 0xF5-0xF7 开头、码点大于 U+10FFFF 的非法 UTF-8（实测 F5 80 80 80 被接受），与 JS fatal TextDecoder/C# StrictUtf8 的拒绝行为不符（include/mistlane/entity_codec.hpp:90）
  证据：2026-10-30；ctest 68/68
- [x] 审核发现：畸形输入测试未覆盖码点超过 U+10FFFF 的 UTF-8 序列（如 F5 80 80 80），使上述 UTF-8 缺陷未暴露（src/test_cases.cpp:1002）。已补：decode_why 断言 F5 80 80 80 被拒，mistlane_entity_codec 全绿
  证据：2026-10-30；ctest 68/68
- [x] 审核发现：文件顶部注释声称与 JS/C# SDK golden 逐字节一致，但 4 字节整数解码未做有符号扩展，与 goldens 实际行为不符（include/mistlane/entity_codec.hpp:16）
  证据：2026-10-30；ctest 68/68
- [x] 审核发现：头部注释新增说法「4 字节符号扩展是刻意偏离上游（upstream sproto does not）」不实
  证据：2026-10-30；ctest 68/68
- [x] 审核发现：commit message 称「Add golden negative INT32 vectors to the C++ probe」，但 src/sproto_probe.cpp 未新增任何负 INT32 golden 向量（负值向量 negative32/neg_array 仅加到 src/test_cases.cpp）；probe 内新增的 4 字节符号扩展解码分支（src/sproto_probe.cpp:227、:260）无任何向量覆盖
  证据：2026-10-30；ctest 68/68
- [x] 审核发现：SprotoCodec 属性快照把声明属性 id 直接当 sproto 字段 tag 编入 16 位 tag 序列，而契约解析对属性 id 仅限 9 位十进制（无 16 位上限），相邻属性 id 间距超过 sproto 单步 gap 可表示范围（约 32767）的合法契约（实测 id=1 与 id=65535 共存）经 encode_properties 后 gap word 被截断为 0xfff9，decode_properties 静默丢弃该大 id 字段且仍返回 true
  证据：2026-10-30；ctest 68/68
- [x] 审核发现：entity.snapshot 用例的声明属性 id 仅 1..12，未覆盖属性 id 间距超过 sproto 16 位 tag 可表示范围的边界，使上述 gap word 截断缺陷漏检（src/test_cases.cpp:1055）。已补：entity.snapshot 新增 id=1 与 id=32766（16 位上限）共存的 max-gap 往返断言，及 id=65535 在契约解析和编解码两层的拒绝断言（src/test_cases.cpp:1123-1153）
  证据：2026-10-30；ctest 68/68；tests=1 passed
- [x] 审核发现：MLD1 FENCE 未校验请求代与存储记录当前代一致
  证据：commit 6d01031；ctest 68/68；tests=1 passed
- [x] 审核发现：plain gateway 的实体关联重写在 MLC2 关联会话上被旁路
  证据：commit 6d01031；ctest 68/68；tests=1 passed
- [x] 审核发现：Lua `entity.spawn`/`entity.destroy` 对 scope/type/incarnation/generation 仅用 `lua_isinteger` 判型、再直接 `static_cast<uint32_t>`，未校验非负与 32 位范围
  证据：commit 6d01031；ctest 68/68；tests=1 passed
- [x] 审核发现：entity_frame_offset 重构把尺寸下限检查从 is_entity_frame 移入 entity_frame_offset（后者仅要求帧以 "MLE1" 开头、或在 4 字节 MLC2 前缀后出现 "MLE1"，不再要求达到 correlation 切片下限），但 forward_to_client / forward_upstream 在 `entity_correlation(frame, at)` 调用之后才判 `frame.size() >= at + 8`，导致短帧越界读取
  证据：ctest 67/68；tests=1 passed
- [x] 审核发现：MLR1 MIG COMMIT 发布角色路由时忽略 `registry_.set_route(...)` 的返回值
  证据：ctest 67/68；tests=1 passed
- [x] 审核发现：MLR1 MIG COMMIT 新增的 route_publishable 预检查（src/node.cpp:2216-2219）在 `tx->apply(token, Commit)` 之前执行，而 apply 首先校验 token 一致性（`token.key != data_.key` 判 Invalid、`journal.find(token.id)` 缺失或 `token.generation != row.token.generation` 判 Stale、`token.id != data_.current` 判 Conflict，include/mistlane/migration.hpp:243-247、258），预检查只判 `existing->state != Committed && !route_publishable(...)`，不校验 generation 与 current。故在目标成员退出注册表的窗口内，对同一条 PREPARING 事务以错误代 COMMIT（如 move-8 记录代 9 却发 gen 8）或以非 current 的旧 id COMMIT，会被误判为 `MLR1 ERR PENDING`，而 apply 本应判 `MLR1 ERR STALE`/`MLR1 ERR CONFLICT`
  证据：ctest 68/68；tests=1 passed

## 审核发现 2026-10-31

- [x] 审核发现：MLR1 MIG COMMIT 的 PENDING 预检（src/node.cpp:2214-2225）把 apply() 的 token 一致性校验（generation 匹配、非终态、id==current）与 commit() 的门控条件（home_fenced、destination_ready、route==expected）逐条内联复制，而非从单一事实源推导。上一版预检正是因只复制了 `state != Committed && !route_publishable(...)` 而漏掉 generation/current 校验，把本应 STALE/CONFLICT 的错误代/过期 id COMMIT 误判为 PENDING（即本提交修复的缺陷，见 todo.md 上一条），故内联复制是本次缺陷的根因模式。风险仍在：今后 apply()/commit() 若新增或调整任一校验/门控条件，此内联预检会静默漂移
  证据：commit da6b331；ctest 70/70；tests=1 passed

- [x] 审核发现：`--journal` coordinator 收到无 generation 的旧式 `MLR1 ROUTE SET`（src/node.cpp:2034-2035 默认 generation=0）时，`save_placement` 把 generation=0 持久化（src/journal.cpp:624-646 校验块未要求 generation 非零），而 `restore_placements` 却以 `|| !generation` 判该行损坏并抛 `placement row corrupted`（src/journal.cpp:694-701），导致该 coordinator 重启即拒绝启动
  证据：commit 74d7c01；ctest 72/72；tests=1 passed
- [x] 审核发现：`LuaService::ticket_issue`（src/lua_service.cpp:357-389）在参数校验中直接 `throw std::runtime_error`（:365-373、:380-383），未像 `websocket_send`/`session_send` 等 Lua 回调那样包裹 try/catch 并转 `lua_error`/`luaL_error`；该 C++ 异常会穿过 `lua_pcall` 的 C 帧向上传播，Lua 的 CallInfo 栈不会正常回卷，可能使后续 `lua_pcall` 处于不一致状态（`placement_call`:592 为既有同模式，本函数新增一处实例，且坏参数路径未被 `login_ticket_wire` 覆盖）
  证据：commit 74d7c01；ctest 72/72；tests=1 passed
- [x] 审核发现：`ticket_issue` 以 `static_cast<std::uint64_t>(lua_tointeger(...))` 接收 generation/expires（src/lua_service.cpp:380-383），`lua_isinteger` 接受负数，负值被回绕为巨大无符号数（-1 → UINT64_MAX），而 `!generation || !expires` 仅拒 0 故放行
  证据：commit 74d7c01；ctest 72/72；tests=1 passed

- [x] **DOC-GIF · P2 · 文档 · 配置开关效果动态图(每个开关 off→on 对比+GIF)**
  证据：8 组 off→on GIF；`pnpm docs:build` + `pnpm docs:check` 通过；`python3 tools/export.py` 通过；gtest 72/72（排除本机 MariaDB 初始化失败的环境项）



## 代码深审 2026-11-01

- [x] P2 生命周期：src/node.cpp:461 session_send 把栈局部 reason 以引用捕获进异步写回调，异步失败（message_size/operation_aborted）完成后向已销毁栈帧写入（use-after-scope）。修复：回调改为按值捕获或把 reason 移入回调可写状态，同步拒绝语义改注释。
  证据：commit e143a77；ctest 72/72（排除 MySQL 环境项）
- [x] P2 逻辑/安全：include/mistlane/entry_ticket.hpp:180 TicketVerifier::verify 用 std::array::operator!= 非恒定时间比较 HMAC，与 src/api.cpp:95 CRYPTO_memcmp 不一致，MLT1 门票验证留下时序侧信道。修复：改 CRYPTO_memcmp。
  证据：commit e143a77；ctest 72/72（排除 MySQL 环境项）
- [x] P2 资源泄漏：src/lua_service.cpp:807 stream_publish 在 callback_ref 之后才做字段校验，校验抛异常时 Lua registry 引用未 unref。修复：校验前置或异常路径 unref。
  证据：commit e143a77；ctest 72/72（排除 MySQL 环境项）
- [x] P2 资源泄漏：include/mistlane/entity_runtime.hpp:48,58 delivered_ 集合只 emplace 从不 erase（destroy 只擦 entities_），长驻节点去重条目无界增长。修复：destroy/重基线时清理 delivered_。
  证据：commit e143a77；ctest 72/72（排除 MySQL 环境项）
- [x] P3 越界/逻辑：include/mistlane/entity_envelope.hpp:123 encode_entity_reply 以 16 位宽度写 reason.size()，>64KB 的 reason 静默截断。修复：拒绝超长 reason 或改宽编码。
  证据：commit e143a77；ctest 72/72（排除 MySQL 环境项）
- [x] P3 逻辑/竞态：src/web.cpp:57 deliver 置 credit_=true 后可能覆盖暂存 incoming_，单槽桥接下罕见消息丢失。修复：reader_ 重新 arm 前保留旧消息或先清 credit_。
  证据：commit e143a77；ctest 72/72（排除 MySQL 环境项）
- [x] P3 逻辑/竞态：include/mistlane/cross.hpp:92 cross_shard 做 hash % count，零 upstream 会除零；当前 main.cpp:334 守卫不可达，属防御缺失。修复：count==0 提前返回/断言。
  证据：commit e143a77；ctest 72/72（排除 MySQL 环境项）
- [x] P3 TLS/认证：src/node.cpp:1039 MLS1 服务令牌用 std::string::operator!= 非恒定时间比较。修复：改 CRYPTO_memcmp。
  证据：commit e143a77；ctest 72/72（排除 MySQL 环境项）
- [x] P3 TLS/认证：src/node.cpp:2096 registry 身份校验同样非恒定时间令牌比较。修复：改 CRYPTO_memcmp。
  证据：commit e143a77；ctest 72/72（排除 MySQL 环境项）


- [x] 审核发现：`--data` 的 ReturnSender 只在 `!web_mode_` 分支启动（src/node.cpp:3045），crossServer 以 WS/HTTP/HTTPS 监听（web_mode=true）运行时发送器永不 start，`mistlane.returning.seed/enqueue` 入队后无人发送（src/node.cpp:3038-3047）
  证据：commit e143a77；ctest 72/72（排除 MySQL 环境项）
- [x] 审核发现：`LuaService::return_enqueue` 用 `lua_isinteger` 判型后直接 `static_cast<uint64_t>(lua_tointeger(...))` 接收 generation/sequence（src/lua_service.cpp:383-385），负数回绕为巨大值并通过 queue 的 `!change.sequence||!change.generation` 检查，data 侧 `record_decimal`（≤10 位）拒收后重试耗尽，变更静默卡死——与同批已修的 ticket_issue 负值缺陷同根因
  证据：commit e143a77；ctest 72/72（排除 MySQL 环境项）
- [x] 审核发现：ReturnSender 连 data 的腿用裸 TcpChannel 直发 `MLD1 LOAD/SUBMIT`，无 MLS1 身份握手（src/node.cpp:534-548）；带 `--service-token` 的 data（契约要求每条腿 MLS1，docs/service-contracts.md:36）会回 `MLS1 ERR TOKEN` 并关闭，`--data` 无法对接受保护的 data
  证据：commit e143a77；ctest 72/72（排除 MySQL 环境项）
- [x] 审核发现：「bounded queue」名不符实：`ReturnSendQueue` 只限每项重试次数（retry_limit_=3），容量无界（src/node.cpp:700 的 `pending_before_load_`、include/mistlane/record.hpp:424 的 `pending_` 均无限增长），与 src/main.cpp:266 帮助文案「retry in a bounded queue」及协议「队列满转只读」声明不符（文档已注明队列满未实现，但实现也没有任何容量上限）
  证据：commit e143a77；ctest 72/72（排除 MySQL 环境项）
- [x] 审核发现：`return_enqueue` 允许 payload 到 `max_frame`（src/lua_service.cpp:388），但 `send_next` 在其前拼上 `MLD1 SUBMIT <realm> <char> gen N seq N ` 头（src/node.cpp:642-647），`TcpChannel::write` 拒收 >max_frame，近满帧（约 1MiB 减去头长）永远发送失败并烧尽重试
  证据：commit e143a77；ctest 72/72（排除 MySQL 环境项）
- [x] 审核发现：新 `--data` 选项与 `DATA` 环境键未加入 docs/guide.md:203（及 docs/zh/guide.md:203）的配置/环境键清单，config 文件用户无法发现该键，文档与新增 CLI 不同步
  证据：commit e143a77；docs:build + docs:check 通过
- [x] 审核发现：todo.md 末尾新增了多余空行，`git diff --check` 报 `todo.md:158: new blank line at EOF`（todo.md:158）
  证据：commit e143a77；`git diff --check` 通过
- [x] 审核发现：ReturnSender 的 MLS1 身份写将 writing_ 置 true（src/node.cpp:572）而成功回调未复位（src/node.cpp:573-576），data 回 "MLS1 OK" 后 phase 转 ready（src/node.cpp:593）但 send_next 被 writing_ 守卫拦住（src/node.cpp:669），跨服回传对带 --upstream-token 的 data 永不发送 LOAD/SUBMIT，回归自 commit 09aacd6
  证据：commit 122629d；ctest 72/73（排除 MySQL 环境项）
- [x] 审核发现：drain_pending 向 queue_ 逐条搬入待载变更（src/node.cpp:703-706），若 queue_ 已接近容量或同键重复序列则 queue_.enqueue 抛异常，receive 捕获后 retry_later 重连但 loaded_cursor_ 已含该键不再重载，剩余变更滞留 pending_before_load_ 且 pending_before_load_count_ 失真（上限被放宽），调用方已得成功却静默丢弃
  证据：commit 122629d；ctest 72/73（排除 MySQL 环境项）
- [x] 审核发现：测试缺口——cross_return_sender（src/test_cases.cpp:4365）未给 crossServer 设 --upstream-token，ReturnSender 的 MLS1 identify 路径（src/node.cpp:570-596）无端到端覆盖，上述 writing_ 卡死不会被测试捕获；ReturnSendQueue 容量上限（record.hpp:364）与 return_enqueue 正数拒绝（lua_service.cpp:386-389）亦无直接用例
  证据：commit 122629d；ctest 72/73（排除 MySQL 环境项）

- [x] 审核发现：本轮 diff 未见问题（9505e4fb..HEAD）：仅 docs 改动——README.md/README.zh-CN.md 徽章行布局调整（合并为单行、去除嵌套重复的 <p align="center">），且 README.zh-CN.md 将 docs 徽章链接从 https://cuihairu.github.io/mistlane/ 修正为 https://cuihairu.github.io/mistlane/zh/（与 .vitepress/config.ts 中 zh locale root='/zh/'、base='/mistlane/' 一致，修正了指向英文首页的错误链接）。无代码改动，无注入/反序列化/越界/泄漏/竞态/错误处理面，无测试适用。覆盖范围：9505e4fbc785..HEAD 全量（1 commit aa41fc4，README.md + README.zh-CN.md，+2/-10）
  证据：commit 122629d；`git diff --stat 9505e4fb..HEAD` 仅 README.md/README.zh-CN.md（+2/-10），无代码/测试改动，已逐行核实为徽章行布局与中文文档链接修正，无问题

## 审核发现

- [x] 审核发现：main.cpp 允许 crossServer 携带 `--upstream-token` 时无条件要求 `--data`（src/main.cpp:360-363），但该 token 同时是 crossServer 注册 coordinator 时的 registry MLS1 身份（src/node.cpp:2824-2827），注释「The crossServer presents this identity to its MLD1 data leg, not to a registry」与 registry 腿实际使用同一 token 相矛盾；`crossServer --coordinator --upstream-token`（无 `--data`）此前为合法配置（旧校验仅 `!coordinator_enabled` 时拒绝），现被误拒
  证据：commit 2f5240d；src/main.cpp:366；ctest 73/74（仅 MySQL 环境项失败）

- [x] 审核发现：ReturnSender::drain_pending 预校验（src/node.cpp:850-860）只校验容量与同键重复序列，未校验 `generation>0 && sequence>0`，而 `queue_.enqueue`（include/mistlane/record.hpp:362）强制该不变量；零/负代或零序变更可经 pre-load 池入队（src/node.cpp:657-675 同样未校验正数），在搬入循环中途触发 enqueue 抛异常，造成部分搬入、剩余变更滞留——正是注释声称「atomic」要消除的失败模式（当前仅 Lua 边界 return_enqueue 拦截，属防御缺口）
  证据：commit 2f5240d；src/node.cpp:647、src/node.cpp:862；ctest 73/74（仅 MySQL 环境项失败）

- [x] 审核发现：pre-load 池 enqueue（src/node.cpp:657-675）接受尚未 seed 的参与者（未校验 send_cursor_ 含该键），变更永久滞留 pending_before_load_——该键不进 seeded_，send_next 永不 LOAD、drain_pending 永不搬入；而 queue 路径（include/mistlane/record.hpp:362）对未 seed 键抛「unseeded participant」，池边界与队列边界不变量不一致
  证据：commit 2f5240d；src/node.cpp:659；ctest 73/74（仅 MySQL 环境项失败）

- [x] 审核发现：测试缺口——cross_return_queue_bounded（src/test_cases.cpp:5533）仅覆盖独立 ReturnSendQueue 的容量/重复/contains，未覆盖 ReturnSender pre-load 池与 drain_pending 的联合容量记账及「满/重复在 drain 时拒收」路径；结合上述池边界不变量缺口，该路径目前可达且无端到端/单元用例（cross_return_sender 已覆盖带 token 的 MLS1 identify 端到端，主修复本身有测试）
  证据：commit 2f5240d；src/test_cases.cpp:4492-4497；ctest 73/74（仅 MySQL 环境项失败）

- [x] 审核发现：service_hello（src/node.cpp:182-186）把 `--service-name` 原样拼入空格分隔的 MLS1 四词帧，service_fields（src/node.cpp:174-178）要求恰好四词，而 `--service-name` 在 main.cpp 无空白/单词语校验（仅 MISTLANE_TICKET_SECRETS 时经 record_identifier，src/node.cpp:3220）；`--service-name "a b"` 会破坏帧使对端解析失败拒识——现被新用到 crossServer data 腿身份（src/node.cpp:3228-3230），属既有缺口被新路径暴露
  证据：commit 2f5240d；src/main.cpp:368、src/test_cases.cpp:3550-3553；ctest 73/74（仅 MySQL 环境项失败）

- [x] 审核发现：cross_return_sender 新增 unseed/negpre/zeropre 探针未真正验证新 sink 侧守卫——unseed 用 pcall+ok 判定，但 ReturnSender::enqueue 拒绝时是内部捕获返回 "invalid"（src/node.cpp:680），Lua 包装层以普通返回值下发 false,"invalid"（src/lua_service.cpp:400-401）而非抛 Lua 错，故 pcall 的 ok 恒为 true、回复恒为 "UNSEED ACCEPTED"，断言 find("UNSEED ")==0 无论 src/node.cpp:659 是否拒收都通过；negpre/zeropre 的负代/零序在 Lua 边界即被拒（src/lua_service.cpp:392-393），从未到达新增守卫 src/node.cpp:647 的 !generation||!sequence，仅重复既有 neg/zero 行为（src/test_cases.cpp:4412-4429、4492-4497）
  证据：ctest -R "cross_return_sender|cross_return_queue_bounded" 2/2 Passed

- [x] 审核发现：main.cpp:361-366 放宽 crossServer `--upstream-token --coordinator`（无 --data）为新合法配置，但无正例测试——新增 orphan 探针（src/test_cases.cpp:3546-3549）仅断言仍被拒的「无 coordinator 无 data」分支，未覆盖新放行的 coordinator-without-data 启动/注册路径，回退收紧不会被抓到（src/test_cases.cpp:3546-3549）
  证据：ctest -R coordinator_registry 1/1 Passed

- [x] 审核发现：--service-name 校验（src/main.cpp:367-368）用 record_identifier（禁 '.'/':'，src/record.hpp 或 include/mistlane/record.hpp:36-42），比线格式更严且与自身默认不一致——service_hello 默认身份为 listen 地址+':'+端口（src/node.cpp:183-184，如 "127.0.0.1:9000"），该串不满足 record_identifier；接收端 service_fields（src/node.cpp:174-178）只解析任意单词、不对 instance 做标识符校验，故校验拒绝的值在线上合法且是同一代码默认生成的串（src/main.cpp:367-368、src/node.cpp:183-184）
  证据：ctest -R cross_return_sender 1/1 Passed
- [x] 审核发现：携带 `--coordinator` 的 gameServer 或 crossServer 现在接受 `--service-name xs:1` 等非 record 标识符并正常启动，但 coordinator 会拒绝其注册并令节点不断重试，导致节点无法被发现或参与路由（src/main.cpp:375）
  证据：ctest 74/74 Passed（src/main.cpp 对注册成员拒绝非 record 标识符，src/test_cases.cpp:3555-3561 覆盖该拒绝）

- [x] 审核发现：本提交只对注册成员的 `--service-name` 做启动期 record_identifier 拒收（src/main.cpp:382-386），但同一「启动正常、注册被拒、无限重试」缺陷仍经 `--scope` 与 `--namespace` 打开——coordinator 的 register_member 要求 `record_identifier(scope)`（src/node.cpp:2350）与 record_identifier 命名空间标记（src/node.cpp:2339），而 main.cpp 对 scope/namespace 仅校验「需 --coordinator」（src/main.cpp:356-357），无任何格式校验；注册成员携带 `--scope "a:b"` 或 `--namespace "n:m"`（无空白、可通过单词语帧）仍正常启动，协调者回 `MLR1 ERR INVALID`，成员 reschedule 无限重试（src/node.cpp:2859-2861、2901-2903），与本提交刚关闭的 service_name 故障同根因（src/main.cpp:382-386、src/node.cpp:2339/2350）——已修复：注册成员（gameServer/crossServer）的 `--scope` 与 `--namespace` 现于启动期按 record_identifier 拒收（src/main.cpp:387-394，namespace "-" 默认组合法、scope 非空且须为标识符），与 coordinator register_member 校验一致；src/test_cases.cpp:3564-3575 覆盖 `--scope "a:b"`、`--namespace "n:m"` 反例
  证据：ctest -R coordinator_registry 1/1 Passed；TMPDIR=/tmp ctest 74/74 Passed（build/debug，含 MySQL/Redis 集成）

- [x] 审核发现：todo.md 本提交新增末行「本轮 diff 未见问题（88c5fcee..HEAD）」（todo.md:222）的区间标注与本轮实际审核范围（2d67cf3891..7d3d792d3f）不符，且与同提交新增的已修复发现（todo.md:220）区间重叠——该行作为「未见问题」结论自相矛盾/陈旧，需修正区间或移除（todo.md:222）。——已修复：移除该陈旧「未见问题」行（该轮 2d67cf3..7d3d792 实际有已修复与待办发现各一条，「未见问题」不成立）；纯 todo.md 记账更正，无代码改动

> 已删除审核行「本轮 diff 未见问题（7d3d792..f295513）」——不属实：f295513 的实际 diff 含一条未修复的启动期校验缺口（--scope/--namespace，见上一条）与一条已修复的 todo 记账更正，并非「未见问题」，该结论自相矛盾/陈旧

- [x] 审核发现：新校验对 `--scope` 未像 `--namespace` 那样显式豁免 "-"（src/main.cpp:392，record_identifier("-") 为真），但 coordinator 的 place/query 会把 scope "-" 清空或按空拒查（src/node.cpp:2427、2440），而 register_member 却按字面存储 "-"（src/node.cpp:2355）——注册成员带 `--scope "-"` 可启动且注册成功，却永久无法被任何放置查询命中，与本修复目标同类「可启动但不可用」缺陷未闭合（src/main.cpp:392、src/node.cpp:2355/2427/2440）——已修复：注册成员（gameServer/crossServer）的 `--scope "-"` 现于启动期按 record_identifier 拒收（src/main.cpp:395，`config.scope == "-"` 显式豁免移除），与 coordinator register_member 的默认组语义一致；src/test_cases.cpp:3570-3575 覆盖 `--scope "-"` 反例
  证据：ctest -R coordinator_registry 1/1 Passed
- [x] 审核发现：todo.md 本提交「已修复」证据的行号微偏——"src/main.cpp:388-394" 实际校验块为 387-394，"src/test_cases.cpp:3562-3576" 实际新增用例为 3564-3575（todo.md:223）——已修复：该发现所述行号微偏属实，且本提交继续修正了 `--scope "-"` 反例（见上一条），故更新 todo.md:223 的「已修复」证据引用为实际行号（src/main.cpp:387-395、src/test_cases.cpp:3564-3575）；纯 todo.md 记账更正，无代码改动
  证据：src/main.cpp:387-395 校验块、src/test_cases.cpp:3564-3575 新增用例；ctest -R coordinator_registry 1/1 Passed

> 已删除审核行「本轮 diff 未见问题（bc4ba76d..HEAD）」——不属实：bc4ba76d..HEAD 是移动区间；在 b6ba329 的创建时点仅含 00bafc7，但后续 docs 提交使区间继续增长（现值随新提交增长，不固定）。其中固定子集 bc4ba76d..0337b3d 已含提交 00bafc7、b6ba329、1709c90、0337b3d，其 diff 含一条已修复缺陷（--scope "-" 启动期未拒收，src/main.cpp:395、src/test_cases.cpp:3570-3575）与多条已修复的 todo 记账更正；其余为审核记账本身，并非「未见问题」，该结论自相矛盾/陈旧
- [x] 审核发现：新增说明称 `bc4ba76d..HEAD` 仅含提交 `00bafc7`，但该区间还包含添加此说明的提交 `b6ba329` 与修复此说明的 `1709c90`，导致审核范围记录错误（todo.md:235）。——已修复：更正为「含提交 00bafc7、添加此说明的 b6ba329 与修复此说明的 1709c90」，并将区间终点由移动的 `..HEAD` 钉定为 `..0337b3d`（移动终点使每次修复提交自身进入区间、计数再度失效，钉定后不再漂移）；纯 todo.md 记账更正，无代码改动
  证据：git rev-list --count bc4ba76d..0337b3d = 4（00bafc7、b6ba329、1709c90、0337b3d）；TMPDIR=/tmp ctest 74/74 Passed（build/debug）
- [x] 审核发现：`bc4ba76d..HEAD` 的区间说明及数量证据遗漏当前提交 `1709c90`，实际包含 3 个提交（todo.md:235）。——已修复：更正区间说明为「含提交 00bafc7、添加此说明的 b6ba329 与修复此说明的 1709c90」，并将区间终点钉定为 `0337b3d`、计数更新为 4；纯 todo.md 记账更正，无代码改动
  证据：git rev-list --count bc4ba76d..0337b3d = 4（00bafc7、b6ba329、1709c90、0337b3d）；TMPDIR=/tmp ctest 74/74 Passed（build/debug）
- [x] 审核发现：`bc4ba76d..HEAD` 区间说明及计数证据遗漏当前提交 `0337b3d` 自身，实际含 4 个提交却记作 3 个（todo.md:235-237），复现了本提交自称已修复的「遗漏当前提交」缺陷。——已修复：将区间终点由移动的 `..HEAD` 钉定为 `..0337b3d`，记录计数为 4（00bafc7、b6ba329、1709c90、0337b3d）；移动终点使每次修复提交自身进入区间、计数再次失效，是此缺陷反复复现的根因，钉定终点后不再漂移；纯 todo.md 记账更正，无代码改动
  证据：git rev-list --count bc4ba76d..0337b3d = 4（00bafc7、b6ba329、1709c90、0337b3d）；TMPDIR=/tmp ctest 74/74 Passed（build/debug）

- [x] 审核发现：钉定区间时同时改写了「已删除审核行」的引文，将原来的 bc4ba76d..HEAD 误记为 bc4ba76d..0337b3d，导致历史记录失真（todo.md:235）。——已修复：恢复「已删除审核行」引文为 bc4ba76d..HEAD（该引文只恢复 2d0f62c 已存在的历史文本，未恢复 1709c90 提交清单，两者仅区间终点字符串一致），历史区间不再失真；纯 todo.md 记账更正，无代码改动
  证据：git show 2d0f62c^:todo.md 引文为 bc4ba76d..HEAD；TMPDIR=/tmp ctest 74/74 Passed（build/debug）

- [x] 审核发现：「已删除审核行」引文恢复为移动终点 bc4ba76d..HEAD，却保留钉定区间 bc4ba76d..0337b3d 的 4 提交清单，而当前 HEAD 下 bc4ba76d..HEAD 实际含 6 个提交（另有 2d0f62c、458cfca），复现了「移动终点随新提交漂移」缺陷（todo.md:235）。——已修复：不再给出随 HEAD 漂移的当前计数或完整提交清单；改为固定事实（bc4ba76d..0337b3d 含 4 个提交）加「区间为移动区间、现值随新提交增长」的定性说明，避免再次复现同一缺陷
  证据：git rev-list --count bc4ba76d..0337b3d = 4；git rev-list --count bc4ba76d..HEAD = 6（截至本修复前）；TMPDIR=/tmp ctest 74/74 Passed（build/debug）
- [x] 审核发现：新「已修复」行称恢复引文「与 1709c90 的原始引文一致」，但 1709c90 原始引文提交清单仅「00bafc7 与 b6ba329」2 项，恢复后为 4 项（00bafc7、b6ba329、1709c90、0337b3d），不一致——仅区间终点字符串一致，提交清单仍为 2d0f62c 版本，「与原始引文一致」表述不成立（todo.md:243）。——已修复：删除「与 1709c90 的原始引文一致」的虚假等价结论，改为明确说明该引文只恢复 2d0f62c 已存在的历史文本，未恢复 1709c90 提交清单，两者仅区间终点字符串一致
  证据：git show 1709c90:todo.md 行 235 清单为 00bafc7、b6ba329；git show 2d0f62c:todo.md 行 235 清单为 00bafc7、b6ba329、1709c90、0337b3d；TMPDIR=/tmp ctest 74/74 Passed（build/debug）
- [x] 审核发现：父项「仍依赖子项 30/31/34」以数字指代子项，但 todo.md 无子项编号体系，30/31/34 无法解析为确定子项；若按行号（30/31/34）理解则行号随编辑漂移（本文件已有「行号微偏」审核先例），引用脆弱不可靠（todo.md:21）。——已修复：将数字引用替换为三个直接子项的文字描述（跨服移交/结算/回服故障恢复、login/data/apiServer 契约与运行时接线、角色校验/脚手架/文档同步验收），不依赖行号或不存在的编号体系
  证据：todo.md:21 文字引用；TMPDIR=/tmp ctest 74/74 Passed（build/debug）
- [x] 审核发现：审核时点 b985ba8 下 `bc4ba76d..HEAD` 包含 7 个提交而非所述的 6 个，移动区间的「现值」随后续提交失准（todo.md:235）。——已修复：将行 235 的漂移数值「现值 6」改为定性说明「现值随新提交增长，不固定」，不再给出随 HEAD 漂移的当前计数；纯 todo.md 记账更正，无代码改动
  证据：git rev-list --count bc4ba76d..b985ba8 = 7（审核时点）；git rev-list --count bc4ba76d..b151bb4 = 8（含修复提交自身，故不写死现值）；TMPDIR=/tmp ctest 74/74 Passed（build/debug）
- [x] 审核发现：`00bafc7..0337b3d` 排除起点 `00bafc7`，实际只有 3 个提交且 diff 仅涉及 todo.md，不能包含所述 scope 校验修复（todo.md:235）。——已修复：行 235 固定子集改引用 bc4ba76d..0337b3d（含 00bafc7、b6ba329、1709c90、0337b3d 共 4 个提交，diff 含 scope 校验修复）；00bafc7..0337b3d 排除起点后仅 3 个 docs 提交且只动 todo.md，不含该修复；纯 todo.md 记账更正，无代码改动
  证据：git rev-list --count 00bafc7..0337b3d = 3（b6ba329、1709c90、0337b3d）；git diff --name-only 00bafc7..0337b3d = todo.md；git rev-list --count bc4ba76d..0337b3d = 4（00bafc7、b6ba329、1709c90、0337b3d）；TMPDIR=/tmp ctest 74/74 Passed（build/debug）
- [x] 审核发现：记录声称已删除「与 1709c90 的原始引文一致」，但该表述仍保留在第 243 行，故此项标记已修复与文件现状不符（todo.md:248）。——已修复：将行 243 的「与 1709c90 的原始引文一致」改为准确说明「该引文只恢复 2d0f62c 已存在的历史文本，未恢复 1709c90 提交清单，两者仅区间终点字符串一致」，与行 248 修复结论一致；纯 todo.md 记账更正，无代码改动
  证据：git show 1709c90:todo.md 行 235 清单为 00bafc7、b6ba329；git show 2d0f62c:todo.md 行 235 清单为 00bafc7、b6ba329、1709c90、0337b3d；todo.md:243 现为准确说明；TMPDIR=/tmp ctest 74/74 Passed（build/debug）
> 已删除审核行「本轮 diff 未见问题（b985ba83..HEAD）」——不属实：该区间含 b151bb4，其 diff 在记录移动区间提交数时只写审核时点的 7，未计入 b151bb4 自身，提交后实测 bc4ba76d..b151bb4 为 8，故「未见问题」不成立
- [x] 审核发现：本轮 diff 未见问题（b151bb4e..HEAD）。——已核实属实：区间仅含 1 个提交 13634a3（纯 todo.md 记账更正，+3/-2），facts 经实测（bc4ba76d..b985ba8=7、bc4ba76d..b151bb4=8）准确，git diff --check 干净，无代码改动
  证据：commit 13634a3；TMPDIR=/tmp ctest 74/74 Passed（build/debug）

## 源码分层分析 2026-11-07

- [ ] 抽象层清单：L1 进程与角色层——解析/校验单进程配置并选择命令目标（代表 `src/main.cpp:195`、`src/main.cpp:336-448`）。
- [ ] 抽象层清单：L2 传输与会话层——终结多协议连接、组帧、认证/复用/限流并转发载荷（代表 `src/node.cpp:226`、`src/node.cpp:1146-1860`）。
- [ ] 抽象层清单：L3 协调与注册层——维护成员/路由、回答放置并驱动迁移事务（代表 `src/node.cpp:2214-2793`、`src/node.cpp:2795-3010`）。
- [ ] 抽象层清单：L4 服务执行层——托管沙箱 Lua VM、业务 API/回调并连接 Redis/HTTP（代表 `src/lua_service.cpp:136-1063`）。
- [ ] 抽象层清单：L5 实体运行时层——解析契约、编解码/派发实体调用并跟踪属性/同步原型（代表 `include/mistlane/entity_contract.hpp:94`）。
- [ ] 抽象层清单：L6 持久化、准入与可观测层——保存记录/迁移/API 受理，验证凭据并暴露探测/指标（代表 `include/mistlane/record.hpp:125-497`、`include/mistlane/diagnostics.hpp:22`）。

超大文件候选：

- [ ] LAY-001 [P1] 拆分 `src/test_cases.cpp`（6744 行）——单文件串联协议、迁移、持久化、Lua、HTTP 和实体用例，案例表仍集中在 `src/selftest.cpp:9-84`、`src/selftest.cpp:129-206`。
- [ ] LAY-002 [P1] 拆分 `src/node.cpp`（3624 行）——同时包含 TCP/session、MLR1 registry、probe/dashboard 和顶层 node 生命周期（`src/node.cpp:1226-1894`、`src/node.cpp:2214-3010`、`src/node.cpp:296-560`）。
- [ ] LAY-003 [P1] 拆分 `include/mistlane/entity_contract.hpp`（2664 行）——XML 解析、兼容锁/digest/XSD 生成与 C++/JS/C# 绑定生成共享一个巨型头文件入口（`include/mistlane/entity_contract.hpp:94`）。
- [ ] LAY-004 [P1] 拆分 `src/lua_service.cpp`（1063 行）——VM 生命周期、Redis/ticket/return/metrics/entity/HTTP API 和异步适配器混在同一实现文件（`src/lua_service.cpp:110-159`、`src/lua_service.cpp:136-1063`）。

目录/分类不合理处：

- [ ] `docs/websocket-bad-weak-ptr.md:1`、`docs/entity-db-mapping-and-philosophy.md:1`、`docs/entity-sync-persistence.md:1`、`docs/mmo-engine-design-highlights.md:1`、`docs/mmo-engine-survey.md:1` 是英文占位，而完整内容只存在于 `docs/zh/` 对应页。
- [ ] 中文审计文档 `docs/审计-文档一致性.md:1` 放在英文 locale 根目录，未进入 `docs/zh/`。
- [ ] `docs/review/2026-10-30-decisions.md:1` 与 `docs/review/2026-10-30-exec-round.md:1` 是放在英文 locale 的大篇幅中文评审，`docs/zh/review/` 下没有对应文件名页面。
- [ ] `examples/sproto_sdk_golden.cs:1` 与 `examples/sproto_sdk_golden.mjs:1` 是被 ctest 使用的协议 golden fixture（`CMakeLists.txt:94-109`），放在 `examples/` 下会误导读者以为它们是可复用 SDK 示例。
