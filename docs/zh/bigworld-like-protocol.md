# 类 BigWorld 协议体系：演化记录

状态：按 2026-10-19 仓库文档核对的研究笔记。本文在[协议面分析](./protocol-analysis.md)的全景和缺口审计上继续，不重复该审计。“类 BigWorld”指契约、编解码、传输、路由、实体复制和客户端绑定协同组成的体系，不表示与 BigWorld 或 KBEngine 线兼容。

以下外部资料是检索参考，不是 Mistlane 既有结论。链接于 2026-10-19 核对。BigWorld 材料属于历史文档；KBEngine 用作开放实现参考，不代表所有引擎行为完全相同。

## 参考体系核对清单

七个可以分别核对的部分共同构成对照。它们描述的是集成协议体系，而非单独的序列化库。

| # | 组成部分 | BigWorld / KBEngine 参考证据 | 覆盖内容 |
|---|---|---|---|
| 1 | 实体契约定义 | [KBEngine 实体定义](https://kbengine.github.io/docs/programming/entitydef.html)说明 `entities.xml`、各实体 `.def`、属性、方法执行侧与属性作用域。历史[BigWorld 服务端编程指南：实体定义](https://howarduong.github.io/github.io/doc/html/server_programming_guide/ch02.html)将 `.def` 作为脚本通信接口。 | 实体类型注册；属性、持久化/同步作用域、可调用方法、执行侧和稳定标识。 |
| 2 | 引擎消息目录与编解码 | [KBEngine 通信协议格式](https://kbengine.github.io/docs/programming/kbe_message_format.html)和[客户端 SDK 编程](https://kbengine.github.io/docs/programming/clientsdkprogramming.html)介绍引擎/客户端消息定义及导入消息与实体定义。[KBEngine `messages.xml` 示例](https://github.com/kbengine/kbengine/blob/master/kbe/res/server/messages.xml)是源码目录。 | 引擎控制消息与游戏实体消息、方向、编号、载荷语法、错误和序列化规则。实体契约与引擎消息目录相关但不相同。 |
| 3 | 网络层与数据包帧格式 | [KBEngine 通信协议格式](https://kbengine.github.io/docs/programming/kbe_message_format.html)描述协议帧/消息表示；[服务端配置](https://kbengine.github.io/docs/configuration/kbengine_defaults.html)公开数据包长度行为。 | 建连、包边界、消息分发、大小限制、重连和传输适配。TCP/WebSocket/KCP 支持取决于具体实现或插件；所引协议文档不表示所有传输均为通用内建能力。 |
| 4 | 应用组件与转发路径 | [KBEngine 服务端布局](https://kbengine.github.io/docs/concepts/layout.html)、[启动/关闭](https://kbengine.github.io/docs/startup_shutdown.html)和[客户端 SDK 编程](https://kbengine.github.io/docs/programming/clientsdkprogramming.html)展示 Loginapp/Baseapp/Cellapp 角色与客户端消息导入流程。 | LoginApp 准入与交接、Gate/BaseApp 客户端链路、BaseApp/CellApp 实体调用、组件路由与内部转发。应用链路承载协议消息。 |
| 5 | 实体可见性与观察者同步 | [KBEngine 实体定义](https://kbengine.github.io/docs/programming/entitydef.html)规定属性作用域；[服务端配置](https://kbengine.github.io/docs/configuration/kbengine_defaults.html)记录 View 半径与 witness 行为。 | 观察者选择/AOI、可见性生命周期、属性/方法复制和带宽策略。编解码本身不决定接收者或发送时机。 |
| 6 | 跨服与世界/空间管理 | [KBEngine 概念与服务端布局](https://kbengine.github.io/docs/concepts/layout.html)描述分布式服务组件；[实体定义](https://kbengine.github.io/docs/programming/entitydef.html)涉及实体组成和迁移相关数据。 | Space/Cell、分配、实体迁移/ghosting、归属、跨进程或机器的转移顺序与恢复。此类语义依赖运行时协调，不能由消息 schema 单独提供。 |
| 7 | 客户端 SDK 与契约分发 | [KBEngine 客户端 SDK 编程](https://kbengine.github.io/docs/programming/clientsdkprogramming.html)记录客户端插件接入及导入引擎/实体协议；[UE4 示例](https://github.com/kbengine/kbengine_ue4_demo)指向从 demo assets 生成插件。 | 从服务端定义生成/导入消息编号、实体类型、类型化代理、分发与客户端编解码绑定。 |

## 老 game-fogs 到 Mistlane 的演化

老客户端链路见[玩家与场景模型](./player-scene-model.md#protocol-and-method-contracts-before-abstraction)：`game/Data/Data.lua` 加载二进制 `game/proto/sproto.spb`；`game/lib/sp.lua` 编解码；`game/proto/Protocol.lua` 将名称映射到编号；`PacketHandler` 注册暴露给客户端的处理器。原始 `.sproto` 文本源及可复现生成链路尚未找到。此处从老链路已有事实映射到 Mistlane，不把生成二进制当作源契约。

| 类 BigWorld 组成/老项目证据 | Mistlane 证据与现状 | 分类 |
|---|---|---|
| 实体定义；KBEngine `.def`/XML 与 game-fogs 未恢复的 sproto schema | XML 实体/类型契约校验 ID，生成项目 XSD、发布锁和确定性的 `.sproto` schema；尚无运行时接线。[实体契约](./entity-contract.md#one-contract) | **已演化** — 已有明确的实体源契约；这不是恢复出的老 schema。 |
| 消息目录与二进制 codec；game-fogs `.spb` + `sp.lua` | 已有 sproto schema 生成和 fixture 黄金向量；尚无标准 sproto 运行时/codec 接入。[生成产物](./entity-contract.md#generated-artifacts)、[老链路](./player-scene-model.md#protocol-and-method-contracts-before-abstraction) | **演化中** — schema 生成已存在；运行时执行尚无。 |
| 数据包帧/网络适配 | Mistlane 有 TCP 长度帧、WS 载荷映射、HTTP 监听器，以及分别定义的 MLC2/MLS1/MLM1 控制层。这些是传输/运行时契约，不是 sproto RPC 接入。[协议分析](./protocol-analysis.md#protocol-surface-inventory)、[架构](./architecture.md#execution-and-connection-model) | **演化中** — 通用传输已有；codec 分发、协商和客户端协议链路未有。 |
| Login/Gate/Base/Cell 消息转发；老项目 Gate 路由与 handler 分发 | Mistlane 有 gateway、login/data 角色、MLR1 注册/分配/路由和 crossServer 转发；没有完整实体调用链或等价 Base/Cell 拆分。[进程职责](./process-roles.md#current-implementation)、[架构对照](./architecture-comparison.md) | **演化中** — 服务路由存在，但未复现老 handler 语义和完整实体调用。 |
| 观察者/AOI 属性同步；老项目 `SC_CharDataUpdate=4005` 与手工 dirty 行为 | 实体属性复制有设计，但没有 Avatar 运行时、接收者集合、变更追踪或 sproto 通知。[实体契约](./entity-contract.md#automatic-property-replication)、[老链路](./player-scene-model.md#protocol-and-method-contracts-before-abstraction) | **演化中** — 目标语义已有文档；运行时缺失。 |
| 跨服/世界归属与迁移 | 存在 MLC1 静态亲和/准入、MLR1 目录和部分 MLD1 回传端点；归属租约和实体转移状态机不存在。[跨服](./cross-server.md)、[跨服回传](./cross-return-protocol.md) | **演化中** — 传输/控制/数据零件部分存在；完整分配、迁移和归属协议尚无。 |
| 客户端 SDK 生成与消息/实体绑定同步 | JavaScript/C# SDK 是提案 bundle 输出，没有 generator 或可运行客户端绑定。老生成的 `Protocol.lua` 不能还原源定义。[生成产物](./entity-contract.md#generated-artifacts)、[账本 88–91](./avatar-runtime.md#deliberately-not-decided-here) | **未演化** — 没有 SDK 生成发布链路。 |
| 服务端/客户端定义的兼容发布流程 | 原型已有显式 ID 与 release lock；协议字节向量、schema 兼容 diff 和客户端/运行时一致性仍未完成。[协议分析，缺口 7](./protocol-analysis.md#-gaps) | **演化中** — 契约检查已有部分，互操作协议发布闸门没有。 |

计数：**已演化 1，演化中 6，未演化 1**。“已演化”表示 Mistlane 已有承担类似契约职责的产物，不表示复刻老行为或线兼容。前一份跨文档审计中的其他细节、矛盾和缺口仍以[协议分析](./protocol-analysis.md)为准。

## sproto 实现路线

决策：实体/RPC 消息编码使用 sproto。XML `.def` 和类型文件继续作为手写契约，sproto 是生成物。按[实体契约](./entity-contract.md#design-choice)，规划中的 Server bundle 包含 sproto schema、Lua bindings 和 digests。当前构建生成经过校验的确定性 schema、XSD、release lock 和 digest，并有仅供测试的 Lua codec 原型；实体运行时和 SDK 生成仍未实现。

| 阶段 | 具体工作 | 验收证据形态 |
|---|---|---|
| 1. XML 契约生成 sproto schema | `.def` 保持源契约；先运行既有语义校验，再生成。保留来源说明，将 game-fogs 原始 sproto 文本标为未恢复。映射类型、字段、方法及 request/response 或单向通知；内部名固定为 `E{entityId}_{Server|Client}_{methodId}`。cloudwu/sproto parser 的 tag 上限为 0–32766；当前确定性映射为 `entityId * 512 + side * 256 + methodId`，entity id ≤63、method id ≤254。 | `contract.tool` 检查类型映射、稳定命名和重复输出；CLI 在全部输入通过后生成 XSD、lock、digest 和 schema。上游 parser 可解析生成的 `avatar.sproto`。 |
| 2. 编解码边界与黄金向量 | 外层传输帧与载荷编码分开；使用 cloudwu/sproto 上游线格式覆盖标量、数组、嵌套和边界值。此前 fixture 专用规范 TLV 向量保留并明确标为内部格式。 | `mistlane_sproto_wire_golden` 对非空和空 Avatar 固定上游 sproto 字节；`mistlane_sproto_upstream_parser` 解析生成 schema。`sproto.golden` 保留为独立内部 TLV 检查。 |
| 3. Lua 原型与分发 | 使用生成 schema 和上游 Lua 绑定；C++ 契约描述符按 entity/side/method 分派，并传入 caller 与 target 上下文。按契约检查字段；request 返回类型化响应，notify 无响应。 | `contract.tool` 覆盖确定性 C++ bindings、分发和五类负向路径；`mistlane_sproto_lua_roundtrip` 覆盖 Lua 编解码回环。 |
| 4. RPC harness 与生命周期 | 定义关联 ID、超时/取消、重复处理、响应归属和过期实体代次行为。 | 进程内及双进程 harness 覆盖请求/响应、push、超时、过期引用、错误调用方、断连和重复处理。 |
| 5. SDK 兼容 | 从共享 schema 生成 JS/C# 绑定，并核对支持的客户端版本。 | JS/C# 编解码标准黄金向量；兼容检查发现已发布 ID 删除、重编号或改类型。 |
| 6. 发布锁与发布闸门 | 在发布锁中持久记录协议 tag 和 schema digest，并与已发布 bundle 比较兼容性。 | 可复现 bundle 和兼容报告在发布前阻断破坏性变更。 |

这是研究闸门，不是时间表或产品承诺。阶段是否完成以证据为准；仅有生成文件不能作为后续阶段完成的依据。

## 已实现状态与证据

阶段 1 已实现。阶段 2 对当前 fixture 已完成：`mistlane contract build` 校验 XML 并生成确定性 `.sproto`；vendored 上游 parser 可解析；上游 Lua 绑定编码为标准线上字节，并以固定 hex 黄金向量校验后再解码。独立的 `sproto.golden` 仍是项目内部 TLV，未冒充 sproto 字节。`examples/contracts/avatar.def` 是新构造的 fixture，并非恢复出的 game-fogs 源文件。

阶段 3 已有使用生成 schema 和上游绑定的 Lua 编码/解码/重编码原型。C++ `EntityDispatcher` 按契约方法 ID 与方向分派，将 caller 和 target 传给处理器，并用闭合 `EntityError` 集合拒绝无效调用。`contract build` 另生成确定性的 `<stem>.bindings.hpp` caller 调用包装、handler 注册辅助函数及描述符表；业务处理器仍由运行时注册。负向覆盖包括未知方法、字段类型错误、缺必填、编码损坏和 caller 上下文缺失。阶段 3 仍未做：将上游 sproto 二进制解码直接接入调度器、运行时实体路由/生命周期、按字段生成的强类型参数对象。阶段 4–6 未做：RPC 生命周期 harness、JS/C# SDK 兼容、发布锁闸门。证据见 `contract.tool`、三个 `mistlane_sproto_*` CTest 用例及 `examples/sproto_lua.lua`。

## 范围与排除项

本文是老 `game-fogs/sproto` 项目的笔记式演化记录，记录从现有客户端 sproto 二进制和 Lua codec 链路迈向显式、生成式、跨运行时协议体系的可能路径。本文不声称已完整实现，不承诺与 BigWorld/KBEngine 互操作，也不制定商业交付日程。产品化、托管服务设计和云备份均不在范围内。

## 外部资料

本文所有外部资料于 **2026-10-19** 核对，属于参考资料，不是 Mistlane 既有结论：

- [KBEngine 文档索引](https://kbengine.github.io/docs/) — 实体、传输、服务端和客户端 SDK 文档入口。
- [KBEngine 实体定义](https://kbengine.github.io/docs/programming/entitydef.html) — `.def`、实体注册、方法和属性作用域。
- [KBEngine 通信协议格式](https://kbengine.github.io/docs/programming/kbe_message_format.html) — 数据包/消息协议参考。
- [KBEngine 服务端布局](https://kbengine.github.io/docs/concepts/layout.html) — 分布式组件职责。
- [KBEngine 客户端 SDK 编程](https://kbengine.github.io/docs/programming/clientsdkprogramming.html) — 客户端协议导入与实体定义。
- [KBEngine 服务端配置](https://kbengine.github.io/docs/configuration/kbengine_defaults.html) — 包长行为、View 半径与 witness 选项。
- [KBEngine demo assets Avatar.def](https://github.com/kbengine/kbengine_demos_assets/blob/master/scripts/entity_defs/Avatar.def) — 具体实体定义示例。
- [KBEngine UE4 示例](https://github.com/kbengine/kbengine_ue4_demo) — 客户端集成与 SDK 生成参考。
- [历史 BigWorld 服务端编程指南](https://howarduong.github.io/github.io/doc/html/server_programming_guide/ch02.html) — `.def` 与 `entities.xml` 背景；历史文档第三方镜像。
- [sproto schema 与 RPC](https://github.com/cloudwu/sproto#rpc-api) — 上游 schema、RPC request/response、parser 与 codec 参考。
