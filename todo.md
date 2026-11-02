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
- [ ] 按目标职责迁移运行角色（禁止只改名字冒充实现）：gateway、login、coordinator、gameServer、crossServer、data、可选 apiServer；保持单二进制方案可用，目录/target/CLI 一次性迁移并清理别名（部分完成）（BLOCKED）
  - [x] 定义服务身份、会话身份、请求关联和有界双向消息；替换强制一问一答路径，补推送/广播及服务连接复用
  证据：commit 0f720ff
  - [x] 实现 coordinator 的进程注册/就绪/负载、角色与场景归属、迁移状态；区服与跨服组明确作用域，router 不直接改名顶替该能力
    证据：2026-10-20；ctest 69/69
    - [x] 缺口 5 第一片
      证据：2026-10-20；ctest 56/56
  - [x] 明确 gameServer 的角色、场景模板/实例、计时、同步和释放；从旧代码逐项评估通用能力，排除原游戏玩法和配置
  证据：commit 073bd52
  - [ ] 实现 crossServer 的参与者来源、场景与返回上下文；长期变更回传归属侧 data；先确定回传与持久化确认协议及唯一写入权，再实现移交/结算/重试/离线/回服故障恢复（BLOCKED）
  - [ ] 定义 login、data 和可选 apiServer 的服务契约及接线；保留数据归属，禁止订单固化承载进程地址（部分完成）（BLOCKED）
  - [x] 将现有可复用转发/Lua 执行/跨服准入迁入目标职责，CLI 与文档以当前实现为准，不保留旧别名
    证据：2026-10-19
  - [ ] 同步角色校验、Lua ServiceContext、脚手架、帮助、指标标签、测试与中英文文档；验证正常登录、双向消息、同进程切场景、跨进程/跨服移交、回调与多监听，运行时验收后再勾选（部分完成）（BLOCKED）

- [x] 补齐 Session/Avatar、旧跨服回传保存、C++/Lua 实际分工、地图三层数据与大地图边界分析；game-fogs 原始
- [x] 设计地图定义、只读空间数据缓存与可变 Scene 的生命周期；定义准备完成/失败/超时、常驻/临时场景清理、静态数据引用与释放；进入场景或迁移 Avatar 不重复传输/加载整张地图
- [x] 大地图按资源量、活跃实体量、跨进程无缝分别评估；客户端与服务端加载分开，不把实例化、分块、AOI 或 Avatar 跨服移交互相等同。流式加载/空间分区先出需求与设计，不直接实现
  证据：commit 4f11983

- [x] 外部 API 接入职责统一命名 apiServer；中英文职责、首页、架构、协议说明与 game-fogs 对照分析同步；保留可嵌入模块/可选独立部署，不新增默认必需进程
  证据：commit 7655b1a

## 暂缓：实体契约与属性同步草稿


- [x] 编写通用 Entity 契约设计提案：docs/entity-contract.md 与 docs/zh/entity-contract.md；XML .def 为唯一手写契约，生成 sproto、服务器描述及 JS/C# SDK；客户端随契约构建发布，业务契约更新不重编 C++ 服务器。提案不是已实现能力
  证据：commit 46607a0
- [ ] 按可复用库边界实现契约、类型存储、嵌套同步、绑定生成和快照编解码；Mistlane 提供网络/路由/观察者/归属 data 适配，先在仓库内模块化，不另建项目或新增命名（BLOCKED）
- [ ] 按实体契约提案分阶段实现（先审定格式与最小编解码原型，再进入完整运行时）
  - [x] 固定 XML XSD 与语义检查（属性名作为标签、types.xml 复合类型引用、动态标签共享规则及项目 schema 生成）、EntityRef、稳定编号与保留规则、生成锁文件、方法权限、envelope 和错误协议；禁止按遍历顺序重编号
    证据：commit 46607a0
  - [x] 验证 sproto 在 C++/Lua、JavaScript、C# 的同源黄金样本；覆盖 INT64 bigint/long、嵌套结构、缺省值、未知字段和畸形输入，不预设现成 JS/C# 绑定可用
    证据：2026-10-19
  - [x] 实现 contract check/build 工具、可复现生成、版本摘要、兼容报告与服务端 bundle 原子加载；生成物不覆盖业务代码，客户端产物不泄漏内部方法和私有属性
    证据：2026-10-19
  - [ ] 实现通用 Entity 身份/生命周期、C++ 描述解释与 Lua 绑定、受控属性及只读保留代理；Avatar、Monster 共用运行时，不限定玩家实体，不引入 Base/Cell 进程（BLOCKED）
  - [ ] 实现 JS/C# 直接操作 Avatar 的方法/属性/通知绑定（客户端不暴露 .server/.client 中间层，主要请求通过自己的 Avatar）、内部实体 RPC、身份权限校验、请求期限/取消、明确错误及有界队列；非幂等调用不能超时后静默重试（BLOCKED）
  - [ ] 实现全量基线/增量/重连/观察者变更与接收者版本过滤；属性同步为核心机制，同步范围不等于空间 AOI（BLOCKED）
  - [x] 参考 BigWorld 的嵌套路径与数组切片同步，明确 KBEngine 当前普通容器原地修改的通知边界；实现受控结构/数组所有权链、嵌套增量、插删有序操作、生成的客户端属性回调和整批应用
    证据：2026-10-20
  - [x] 约束属性为有界值树：拒绝循环与初期按值递归、跨属性可变共享，复制赋值，旧容器句柄失效；处理 Lua/协议下标转换、结构变更前后路径合并、积压与重建基线
    证据：2026-10-20
  - [x] 验证嵌套修改自动同步/标记持久化，不靠顶层重新赋值或业务 setDirty；覆盖数组结构变化、旧句柄、混合观察者及在途保存，以大背包单项修改衡量字节/分配/CPU
    证据：2026-10-20
  - [ ] 实现归属 data 加载/版本转换/默认值及持久化快照，generation/revision 条件保存与提交确认；仅一个强制保存入口，禁止发出消息就清空 dirty，索引与数据迁移显式管理（BLOCKED）
  - [ ] 实现契约不变的 Lua 安全替换，以及同二进制新进程加载新契约后的实体迁移、唯一写入权交接、在途请求与计时器处理；现有 SIGHUP 不等于该能力（BLOCKED）
  - [ ] 验证新旧 JS/C# 客户端、混合观察者、兼容协商、迁移/保存失败与回滚；以相同服务器二进制摘要承载两版契约、新属性与新方法，证明服务器业务协议更新无需重编。（当前门控：条目 118 的实体迁移/唯一写入权交接机制待网络实体运行时；已有可用件（BLOCKED））

- [ ] Avatar/属性抽象前先补齐客户端协议契约（未完成前不实现新 Avatar API）。已核实旧链路为 sproto，编号表和 PacketHandler 分发见 player-scene-model 文档；不以第三方 Protobuf 依赖替代（BLOCKED）
  - [ ] 定位或恢复原始业务协议定义（含 sproto RPC 协议名/编号、request/response 及引用类型）、生成脚本及正式客户端绑定；确认前后端生成物来源，缺失项明确记录，不将现有二进制 schema 等同于完整生成流程（BLOCKED）
  - [ ] 列出客户端请求、服务端响应/推送和内部消息的编号、字段编号/类型、方向、处理器、身份/权限、错误码、版本兼容规则及验证方式（BLOCKED）
  - [ ] 定义属性声明、增量同步、持久化字段、嵌套变更追踪与周期保存；强制保存只有一个入口，明确其持久化确认、失败与重试语义；撤回 self:sync()，方法名和防覆盖机制参见实体契约提案，尚未实现（BLOCKED）

- [x] 设计 C++ 通用 Avatar/Session/迁移/加载保存契约及 Lua 生命周期钩子；覆盖错误、超时、重入、迁移期间操作，不能只提供 message 即视为完成
- [x] 文档时效性清理
  证据：2026-10-18


## 合并自 todo.md（2026-10-28，原文件未勾项摘录，随后删除

## H. 发布后整理

- [ ] 为首个公开源码候选确定版本号与发布说明（BLOCKED）

- [ ] 为源码归档附上 SHA256 校验值（BLOCKED）

- [ ] 检查 GitHub 发布页包含 Apache-2.0 许可证链接和 NOTICE（BLOCKED）
## J. 崩溃报告、Prometheus、内部探测与性能剖析

- [ ] 为 profiler 工作流提供可重复的跨服压测场景和符号化步骤，记录采样对吞吐、P99 和 RSS 的影响（BLOCKED）

- [ ] 在目标 Linux/容器环境执行崩溃演练、监控抓取、探针过载演练和压力剖析；未测试前不宣称已解决历史跨服卡死或容量问题（BLOCKED）
## K. 项目初始化 CLI 与进程运维模板

- [ ] 按架构比较文档继续补内部 liveness/readiness 探测、Crashpad 转储生成/保留与部署工作流；再依据真实压力测试决定动态 registry、lease/fencing、房间 drain/migration 或容器编排需求（BLOCKED）

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

- [ ] P2 生命周期：src/node.cpp:461 session_send 把栈局部 reason 以引用捕获进异步写回调，异步失败（message_size/operation_aborted）完成后向已销毁栈帧写入（use-after-scope）。修复：回调改为按值捕获或把 reason 移入回调可写状态，同步拒绝语义改注释。
- [ ] P2 逻辑/安全：include/mistlane/entry_ticket.hpp:180 TicketVerifier::verify 用 std::array::operator!= 非恒定时间比较 HMAC，与 src/api.cpp:95 CRYPTO_memcmp 不一致，MLT1 门票验证留下时序侧信道。修复：改 CRYPTO_memcmp。
- [ ] P2 资源泄漏：src/lua_service.cpp:807 stream_publish 在 callback_ref 之后才做字段校验，校验抛异常时 Lua registry 引用未 unref。修复：校验前置或异常路径 unref。
- [ ] P2 资源泄漏：include/mistlane/entity_runtime.hpp:48,58 delivered_ 集合只 emplace 从不 erase（destroy 只擦 entities_），长驻节点去重条目无界增长。修复：destroy/重基线时清理 delivered_。
- [ ] P3 越界/逻辑：include/mistlane/entity_envelope.hpp:123 encode_entity_reply 以 16 位宽度写 reason.size()，>64KB 的 reason 静默截断。修复：拒绝超长 reason 或改宽编码。
- [ ] P3 逻辑/竞态：src/web.cpp:57 deliver 置 credit_=true 后可能覆盖暂存 incoming_，单槽桥接下罕见消息丢失。修复：reader_ 重新 arm 前保留旧消息或先清 credit_。
- [ ] P3 逻辑/竞态：include/mistlane/cross.hpp:92 cross_shard 做 hash % count，零 upstream 会除零；当前 main.cpp:334 守卫不可达，属防御缺失。修复：count==0 提前返回/断言。
- [ ] P3 TLS/认证：src/node.cpp:1039 MLS1 服务令牌用 std::string::operator!= 非恒定时间比较。修复：改 CRYPTO_memcmp。
- [ ] P3 TLS/认证：src/node.cpp:2096 registry 身份校验同样非恒定时间令牌比较。修复：改 CRYPTO_memcmp。


- [ ] 审核发现：`--data` 的 ReturnSender 只在 `!web_mode_` 分支启动（src/node.cpp:3045），crossServer 以 WS/HTTP/HTTPS 监听（web_mode=true）运行时发送器永不 start，`mistlane.returning.seed/enqueue` 入队后无人发送（src/node.cpp:3038-3047）
- [ ] 审核发现：`LuaService::return_enqueue` 用 `lua_isinteger` 判型后直接 `static_cast<uint64_t>(lua_tointeger(...))` 接收 generation/sequence（src/lua_service.cpp:383-385），负数回绕为巨大值并通过 queue 的 `!change.sequence||!change.generation` 检查，data 侧 `record_decimal`（≤10 位）拒收后重试耗尽，变更静默卡死——与同批已修的 ticket_issue 负值缺陷同根因
- [ ] 审核发现：ReturnSender 连 data 的腿用裸 TcpChannel 直发 `MLD1 LOAD/SUBMIT`，无 MLS1 身份握手（src/node.cpp:534-548）；带 `--service-token` 的 data（契约要求每条腿 MLS1，docs/service-contracts.md:36）会回 `MLS1 ERR TOKEN` 并关闭，`--data` 无法对接受保护的 data
- [ ] 审核发现：「bounded queue」名不符实：`ReturnSendQueue` 只限每项重试次数（retry_limit_=3），容量无界（src/node.cpp:700 的 `pending_before_load_`、include/mistlane/record.hpp:424 的 `pending_` 均无限增长），与 src/main.cpp:266 帮助文案「retry in a bounded queue」及协议「队列满转只读」声明不符（文档已注明队列满未实现，但实现也没有任何容量上限）
- [ ] 审核发现：`return_enqueue` 允许 payload 到 `max_frame`（src/lua_service.cpp:388），但 `send_next` 在其前拼上 `MLD1 SUBMIT <realm> <char> gen N seq N ` 头（src/node.cpp:642-647），`TcpChannel::write` 拒收 >max_frame，近满帧（约 1MiB 减去头长）永远发送失败并烧尽重试
- [ ] 审核发现：新 `--data` 选项与 `DATA` 环境键未加入 docs/guide.md:203（及 docs/zh/guide.md:203）的配置/环境键清单，config 文件用户无法发现该键，文档与新增 CLI 不同步
- [ ] 审核发现：todo.md 末尾新增了多余空行，`git diff --check` 报 `todo.md:158: new blank line at EOF`（todo.md:158）
