# 当前实施顺序

先完成已确定的旧游戏进程关系，不新增进程名称。契约库、XML 生成、自动属性同步均为暂缓草稿；本轮不实现。按以下顺序推进并以真实链路验收：

1. 现有运行角色/CLI/配置/脚手架/文档一致迁移；独立转发层从默认拓扑删除，跨服静态转发属于 gateway，不冒充 coordinator。
2. 服务与会话身份、有界双向消息、原 gateway 连接和当前玩家承载路由。
3. login → coordinator 分配 → gameServer → 归属 data 加载；就绪后发布玩家路由。
4. coordinator 协调跨 gameServer/crossServer 移交；保留原 gateway 与归属 data，校验唯一写入权、回服与失败恢复。
5. apiServer 平台回调的身份校验、持久受理、幂等和当前玩家寻址；地图/场景与多监听分别按现有任务验收。

- [x] 审核整改（2026-10-18 独立审核，见 docs/review/luna-review-2026-10-18.md）：
      按审核报告的三张表逐项处理，**先核对再动手**，每项给 file:line 证据。
      1) 表一「缺陷」：按风险排序修，高风险优先
      2) 表二「文档与实现不一致」：以代码为准改文档，或明确标注为规划中
         **禁止为对齐文档去改业务代码**
      3) 表三「值得补的能力」：只做评估与分期建议，**不实现**（需用户拍板）
      每完成一项勾掉并写证据；全部测试通过后 commit+push；不 bump 版本号。
      整改记录见 docs/review/luna-review-response-2026-10-18.md（表一 7 项：6 修 1 复核；
      表二 4 项文档更新+压测手册实测 16000/16000；表三 7 项评估分期，未实现）。
      证据：src/node.cpp（探针写缓冲 shared_ptr 保活、Peer::finish/retire、
      buffer_{256}+max_peers 64、tick 同步 set_ready、reject 计数、parse_address 五处）；
      src/test_cases.cpp probe.control 扩展；src/scaffold.cpp 文案更正；ctest 14/14 绿；
      MariaDB 集成测试本环境复跑通过；docs:build 通过。

- [x] 多协议监听：游戏服务器与 HTTP 服务均可开启 WS/WSS；支持同进程多个独立监听地址及明文/TLS 共存（2026-10-19 完成）。
  - [x] 定义独立 listener 配置、证书与路由绑定；同端口 HTTP→WS upgrade 与不同端口监听均有明确行为，冲突配置启动失败。
      证据：src/main.cpp --listener/LISTENERS 解析、重复端口启动拒绝；src/web.cpp 按 `req->getLocalAddr().toPort()`
      分发 game/lifecycle；wss/https 监听支持 `?cert=FILE&key=FILE` 覆盖全局证书（src/node.cpp 逐监听证书校验）。
  - [x] 游戏 WS/WSS 进入游戏会话与 Lua message；HTTP 服务增加 WebSocket 生命周期与消息回调，普通请求保留 Lua http，升级后不走 HTTP handler。
      证据：src/web.cpp SocketController dispatch；src/node.cpp WebsocketService attach/pump/closed；
      src/lua_service.cpp websocket_open/message/close + mistlane.websocket.send。
  - [x] 增加会话定向推送、发送队列字节上限、慢消费者背压、断连清理及退出策略。
      证据：单次推送上限 1 MiB（src/lua_service.cpp websocket_send）、Trantor 2 MiB 高水位强制关闭
      （src/web.cpp highWaterMarkCallback）、断连清理与 SIGTERM 先关 WebSocket 会话再退出（src/node.cpp stop/close_all）。
  - [x] 验证 TCP + WS + WSS、HTTP + WS、HTTPS + WSS、多个监听同时开启、证书错误、路由隔离与关闭监听；更新脚手架配置和帮助。
      证据：src/test_cases.cpp transport_listeners（`mistlane test --case transport.listeners` 3 次通过）；
      ctest --preset linux-debug 15/15；scaffold.init 通过（新增 config/instances/gameServer-mixed.env、
      run-instance.sh LISTENERS 展开）；docs/http.md、docs/guide.md、docs/zh/http.md、docs/zh/guide.md、
      docs/validation.md、docs/zh/validation.md 同步；check-docs.mjs 28 图通过；pnpm docs:build 通过。

- [x] 整理目标进程职责及实际实现边界：docs/process-roles.md 与 docs/zh/process-roles.md；首页和架构图移除 router 必经层假设。
- [ ] 按目标职责迁移运行角色（禁止只改名字冒充实现）：gateway、login、coordinator、gameServer、crossServer、data、可选 apiServer；保持单二进制方案可用，目录/target/CLI 一次性迁移并清理别名。
  - [ ] 定义服务身份、会话身份、请求关联和有界双向消息；替换强制一问一答路径，补推送/广播及服务连接复用。
  - [ ] 实现 coordinator 的进程注册/就绪/负载、角色与场景归属、迁移状态；区服与跨服组明确作用域，router 不直接改名顶替该能力。
  - [ ] 明确 gameServer 的角色、场景模板/实例、计时、同步和释放；从旧代码逐项评估通用能力，排除原游戏玩法和配置。
  - [ ] 实现 crossServer 的参与者来源、场景与返回上下文；长期变更回传归属侧 data；先确定回传与持久化确认协议及唯一写入权，再实现移交/结算/重试/离线/回服故障恢复。
  - [ ] 定义 login、data 和可选 apiServer 的服务契约及接线；保留数据归属，禁止订单固化承载进程地址。
  - [ ] 将现有可复用转发/Lua 执行/跨服准入迁入目标职责，CLI 与文档以当前实现为准，不保留旧别名。
  - [ ] 同步角色校验、Lua ServiceContext、脚手架、帮助、指标标签、测试与中英文文档；验证正常登录、双向消息、同进程切场景、跨进程/跨服移交、回调与多监听，运行时验收后再勾选。

- [x] 补齐 Session/Avatar、旧跨服回传保存、C++/Lua 实际分工、地图三层数据与大地图边界分析；game-fogs 原始证据与 Mistlane 中英文抽取设计分别归档。
- [ ] 设计地图定义、只读空间数据缓存与可变 Scene 的生命周期；定义准备完成/失败/超时、常驻/临时场景清理、静态数据引用与释放；进入场景或迁移 Avatar 不重复传输/加载整张地图。
- [ ] 大地图按资源量、活跃实体量、跨进程无缝分别评估；客户端与服务端加载分开，不把实例化、分块、AOI 或 Avatar 跨服移交互相等同。流式加载/空间分区先出需求与设计，不直接实现。

- [x] 外部 API 接入职责统一命名 apiServer；中英文职责、首页、架构、协议说明与 game-fogs 对照分析同步；保留可嵌入模块/可选独立部署，不新增默认必需进程。

## 暂缓：实体契约与属性同步草稿

本节仅保留讨论，不属于当前实现清单；先完成已确定的进程职责与调用链，再单独审核与排期。

- [x] 编写通用 Entity 契约设计提案：docs/entity-contract.md 与 docs/zh/entity-contract.md；XML .def 为唯一手写契约，生成 sproto、服务器描述及 JS/C# SDK；客户端随契约构建发布，业务契约更新不重编 C++ 服务器。提案不是已实现能力。
- [ ] 按可复用库边界实现契约、类型存储、嵌套同步、绑定生成和快照编解码；Mistlane 提供网络/路由/观察者/归属 data 适配，先在仓库内模块化，不另建项目或新增命名。
- [ ] 按实体契约提案分阶段实现（先审定格式与最小编解码原型，再进入完整运行时）：
  - [ ] 固定 XML XSD 与语义检查（属性名作为标签、types.xml 复合类型引用、动态标签共享规则及项目 schema 生成）、EntityRef、稳定编号与保留规则、生成锁文件、方法权限、envelope 和错误协议；禁止按遍历顺序重编号。
  - [ ] 验证 sproto 在 C++/Lua、JavaScript、C# 的同源黄金样本；覆盖 INT64 bigint/long、嵌套结构、缺省值、未知字段和畸形输入，不预设现成 JS/C# 绑定可用。
  - [ ] 实现 contract check/build 工具、可复现生成、版本摘要、兼容报告与服务端 bundle 原子加载；生成物不覆盖业务代码，客户端产物不泄漏内部方法和私有属性。
  - [ ] 实现通用 Entity 身份/生命周期、C++ 描述解释与 Lua 绑定、受控属性及只读保留代理；Avatar、Monster 共用运行时，不限定玩家实体，不引入 Base/Cell 进程。
  - [ ] 实现 JS/C# 直接操作 Avatar 的方法/属性/通知绑定（客户端不暴露 .server/.client 中间层，主要请求通过自己的 Avatar）、内部实体 RPC、身份权限校验、请求期限/取消、明确错误及有界队列；非幂等调用不能超时后静默重试。
  - [ ] 实现全量基线/增量/重连/观察者变更与接收者版本过滤；属性同步为核心机制，同步范围不等于空间 AOI。
  - [ ] 参考 BigWorld 的嵌套路径与数组切片同步，明确 KBEngine 当前普通容器原地修改的通知边界；实现受控结构/数组所有权链、嵌套增量、插删有序操作、生成的客户端属性回调和整批应用。
  - [ ] 约束属性为有界值树：拒绝循环与初期按值递归、跨属性可变共享，复制赋值，旧容器句柄失效；处理 Lua/协议下标转换、结构变更前后路径合并、积压与重建基线。
  - [ ] 验证嵌套修改自动同步/标记持久化，不靠顶层重新赋值或业务 setDirty；覆盖数组结构变化、旧句柄、混合观察者及在途保存，以大背包单项修改衡量字节/分配/CPU。
  - [ ] 实现归属 data 加载/版本转换/默认值及持久化快照，generation/revision 条件保存与提交确认；仅一个强制保存入口，禁止发出消息就清空 dirty，索引与数据迁移显式管理。
  - [ ] 实现契约不变的 Lua 安全替换，以及同二进制新进程加载新契约后的实体迁移、唯一写入权交接、在途请求与计时器处理；现有 SIGHUP 不等于该能力。
  - [ ] 验证新旧 JS/C# 客户端、混合观察者、兼容协商、迁移/保存失败与回滚；以相同服务器二进制摘要承载两版契约、新属性与新方法，证明服务器业务协议更新无需重编。

- [ ] Avatar/属性抽象前先补齐客户端协议契约（未完成前不实现新 Avatar API）。已核实旧链路为 sproto，编号表和 PacketHandler 分发见 player-scene-model 文档；不以第三方 Protobuf 依赖替代证据。
  - [ ] 定位或恢复原始业务协议定义（含 sproto RPC 协议名/编号、request/response 及引用类型）、生成脚本及正式客户端绑定；确认前后端生成物来源，缺失项明确记录，不将现有二进制 schema 等同于完整生成流程。
  - [ ] 列出客户端请求、服务端响应/推送和内部消息的编号、字段编号/类型、方向、处理器、身份/权限、错误码、版本兼容规则及验证方式。
  - [ ] 定义属性声明、增量同步、持久化字段、嵌套变更追踪与周期保存；强制保存只有一个入口，明确其持久化确认、失败与重试语义；撤回 self:sync()，方法名和防覆盖机制参见实体契约提案，尚未实现。

- [ ] 设计 C++ 通用 Avatar/Session/迁移/加载保存契约及 Lua 生命周期钩子；覆盖错误、超时、重入、迁移期间操作，不能只提供 message 即视为完成。
- [ ] 文档时效性清理（2026-10-18 用户令：很多文档已过时）：
      在本轮审核整改完成之后执行，避免与整改冲突。
      1) 通读 README / docs，找出与当前代码不符的描述（能力、配置项、命令、路径、版本）
      2) 以代码为准修正文档；**禁止为对齐文档去改业务代码**
      3) 已废弃但仍留在文档里的功能，明确标注「已移除」或删除，不要留成"看起来还能用"
      4) 命令行示例逐条跑一遍，跑不通的改掉或删掉
      5) 构建通过后 commit+push，回报改动文件清单
