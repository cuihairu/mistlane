简体中文 | English

# Mistlane 已代决立即执行动作落档（2026-10-30 实施轮）

> 执行依据：`docs/review/2026-10-30-decisions.md` 各 D 项【立即执行动作】；范围按「已代决（2026-10-14 用户授权，后续可推翻）」执行。每项一节，逐节单独 commit + push（禁攒批），commit 信息英文短词、不携 AI 痕迹。
>
> 跳过规则（硬性）：要密钥/账号/用户私有数据/外部软件安装的，不执行，仅在对应项标「待用户：xxx」。分析结论与仓库事实冲突时以仓库为准并在本节修正。
>
> 本节为设计记录/清单/方案对比（文档类），不含代码改动；实现类（D01 最小链路、D02 补缺口、D03 接线、D08 验收、D12/D13 用例、D24 加载、D30/D31 样例）在维护者确认范围（D07）与运行时环境齐备后执行，本节只记录设计与缺口。

## D06 实体宿主路由：实例级放置记录设计

来源：todo.md:91（宿主路由显式 BLOCKED），:123 重申。D06 建议先在仓库内保持模块化、由既定 coordinator 提供实例级放置记录，网络寻址须同时带明确的归属版本。

### 记录结构

按 D06 推荐，实例放置记录以五元组区分实例，值为当前归属与写入资格：

| 键 | 含义 | 来源/约束 |
|---|---|---|
| `namespace` | 部署命名空间 | 与 MIG 线动词 `[<namespace>]` 一致（`src/node.cpp:2026`）；空或固定名用于单部署 |
| `scope` | 归属范围（如 `character`/`scene`/`service`） | 限定同类型实体集合，避免跨类型键冲突 |
| `type_id` | 实体类型 ID | 与 Lua 契约 `entities` 表绑定一致（D28） |
| `entity_id` | 实体实例 ID | 宿主内唯一；跨宿主由 coordinator 保证唯一 |
| `incarnation` | 实例化代号 | 每次新建实例递增；用于拒绝「销毁后不复活」的迟到消息与旧实例残留 |

| 值 | 含义 | 约束 |
|---|---|---|
| `host` | 当前宿主进程标识 | 由 coordinator 分配/确认，不依赖自报身份 |
| `generation` | 写入资格代号 | 每次交接递增；旧资格被 `HomeSequenceAuthority`/`record_fence` 拒绝（`include/mistlane/record.hpp:125/153/203`、`src/journal.cpp:369/544/559`） |
| `status` | 放置状态 | `publishing`/`active`/`transferring`/`retiring`/`destroyed` |

### 与现有 envelope 的对应关系

- MIG 线动词的 `<realm> <character>`（`src/node.cpp:2026-2041`）对应本记录的 `scope`+`entity_id`；`<id> <generation>` 对应 `incarnation`+`generation`。
- MIG `TAKEOVER`（`:2193`）在目标宿主加载确认后推进 `generation`，与本记录 `transferring → active` 状态迁移一致；旧宿主的旧 `generation` 写入由栅栏拒绝，不依赖 coordinator 再查一次地址簿。
- 现无独立 `envelope.hpp`；envelope 由 MIG/MLD1 线动词参数承载（`MLD1 SET/GET` 于 `src/node.cpp:1931/1949`）。「从 envelope 自报身份授予 owner 权限」不作数——owner 权限仅由 coordinator 确认的放置记录给出（D09）。

### 修改权限与比较旧值

| 事件 | 谁能改 | 更新 | 旧值比较 |
|---|---|---|---|
| 创建 | coordinator | 写 `status=active`，递增 `incarnation`，分配 `host`，`generation=0` | 不存在 → 允许 |
| 迁移（prepare） | coordinator | `status=transferring`，目标 `host` 记录 | 源 `generation` 与 coordinator 记忆一致才允许 |
| 迁移（commit） | coordinator | 目标 `generation` 递增，`status=active` | 目标已确认加载才发布 |
| 迁移（abort/retire） | coordinator | 回退源 `status=active` 或置 `retiring` | 无第二写者（旧资格已拒） |
| 销毁 | coordinator | `status=destroyed`，保留墓碑 | 无新请求，迟到消息按 `incarnation` 拒绝 |

「比较旧值更新」：发布新路由前，coordinator 用当前记忆中的 `generation` 与源宿主最后确认值比较；不一致（说明期间已有交接或回滚）则拒绝发布并令接收方重取。

### 验收例（设计阶段，实施轮运行时执行）

1. 同类型多实例分布在两个宿主：各实例路由独立，不互相覆盖。
2. 旧版本拒绝：旧宿主以旧 `generation` 写入，被栅栏拒绝；新宿主以新 `generation` 写入成功。
3. 销毁后不复活：已 `destroyed` 的 `incarnation` 收到的任何消息被拒，不创建新实例。
4. 迁移期间请求：`transferring` 期间的新请求要么等到新 `generation`，要么被明确拒绝，不出现两个宿主同时应答同一 `incarnation`。

### 缺口核对

- 现有 MIG 动词已含 OWNER/PREPARE/EVENT/COMMIT/ABORT/RETIRE/TAKEOVER/STATUS（`:2026-2041`），无独立「放置记录表」持久化；`record_fence` 持久化已落地（`src/journal.cpp:559`），放置记录本身若需跨重启查询，须另立记录持久化（当前为内存协调面）。
- 「从 envelope 自报身份授予 owner 权限」在代码中未实现，保持不做（D09 定义真实绑定来源）。
- 实例级放置记录当前未实现；本节为设计，不机械接线（D06「不做/延后」）。

## D09 JS/C# 直接调用：可信绑定、准入、超时与取消

来源：todo.md:128；生成绑定已落地，真实身份与传输闭环仍受阻。D09 推荐保留现有 SDK，先定义客户端到自身角色的可信绑定，再补期限/取消的传输语义。

### 绑定来源（Session → 已认证角色 → 授权实体）

不采用「客户端自报角色 ID 授权」——gateway 服务身份不能自动代表任何玩家的 owner 身份（D09 风险 3）。绑定链为：

| 环节 | 来源 | 约束 |
|---|---|---|
| Session 身份 | 传输握手：`MLS1` 识别内部对端、`MLC2` 打开关联会话（`src/node.cpp:803-833`）；跨 gateway 复用上游 hub 身份 | 身份由握手协商，不来自 payload 自报 |
| 已认证角色 | login 签发的短期 HMAC 凭据（D04），凭据绑定 data 已确认的授权 | 验签通过 ≠ 取得写权；写权由 D06 放置记录给出 |
| 授权实体 | coordinator 确认的放置记录（D06）中 `entity_id` 属于该角色 | 客户端只能操作自己有权限的实体（D09 收益） |

「gateway 的服务身份不能自动代表任何玩家的 owner 身份」：gateway 是传输中继（`src/node.cpp:830` 网关拥有共享腿、不代答 MLC2），其身份只用于中继层，不授予实体写权。

### client/owner/server 三类准入案例

| 案例 | 调用方身份 | 目标 | 准入 |
|---|---|---|---|
| client | 已认证玩家 | 自己的角色实体 | 允许（owner 权限） |
| owner | 已认证玩家 | 他人实体 | 默认拒绝；仅当角色显式授权（D16 权限表）才允许 |
| server | 内部服务腿（`MLS1`） | 任意受控实体 | 允许（服务身份，非玩家） |

### 期限、断线、取消与已提交结果的可查询方式

EntityRuntime RPC 状态（`include/mistlane/entity_runtime.hpp:64`）：`PENDING`/`COMPLETED`/`TIMED_OUT`/`DISCONNECTED`。

| 事件 | 语义 | 可查询结果 |
|---|---|---|
| deadline 到 | `PENDING → TIMED_OUT`（`:72`）；非幂等请求显示「结果未知」，不静默再执行 | 客户端可查 `TIMED_OUT`，凭结果状态决定是否重发（幂等键，D04） |
| 断线 | `PENDING → DISCONNECTED`（`:85`）；取消只承诺停止尚未开始的工作 | 已提交的修改不因取消自动撤销（D09 取舍） |
| 迟到应答 | 未知/迟到响应被丢弃（`:77`），返回 `STALE` | 不重复结算，不误报成功 |
| 已完成重复响应 | `COMPLETED` 后再次收到 → `CONFLICT`（`:82`） | 明确冲突，不覆盖已提交结果 |

### 支持矩阵

| 传输组合 | 支持 | 说明 |
|---|---|---|
| 普通 gateway | 是 | `entity_endpoint.hpp:51` 识别 `MLE1` 前缀（偏移 0） |
| 复用连接 | 是 | `node.cpp:204-211` 检测 MLC2 前缀（偏移 4）再识别 MLE1 |
| cross gateway | 是 | `node.cpp:808` 网关把 MLC2 向上游中继 |
| MLC2 + MLE1 组合 | 是 | `node.cpp:204` MLC2 帧带 4 字节 transport id，MLE1 marker 在偏移 4 |

「普通 gateway 测试覆盖全部传输组合」不作数：普通 gateway 验证的是 MLE1 直连路径，cross gateway/MLC2 组合需各自场景验收（D05 多监听场景）。

### 缺口核对

- deadline/断线状态机已实现（`entity_runtime.hpp:72-85`）；「deadline 到后非幂等请求显示结果未知」依赖 D04 幂等键与 D06 归属查询来源，运行时接线待实施。
- 绑定来源当前未实现为独立授权表；本节为设计，不凭「客户端自报角色 ID」授权（D09「不做/延后」）。

## D11 data 加载、快照与提交确认

来源：todo.md:138/139；数据库提交与保存接线开放。D11 推荐第一阶段沿用现有 SQLite 后端、经适配层把实体持久快照接到 data；保存只在提交确认后清除相应版本的脏标记。

### payload 是版本化快照还是变更集

**首阶段采用受帧大小约束的完整持久快照（版本化快照）**，不用变更集。理由（D11 取舍/风险）：端点仅保留最新 payload，若它是零散差量且无合并逻辑，重启后可能还原不全。现有 `MLD1` opaque payload（`src/node.cpp:798`）不自动等于完整实体状态——需适配层明确「快照」语义。

### 契约摘要、持久 revision 与回传序号的对应关系

`include/mistlane/entity_persist.hpp` 已实现核心原语：

| 原语 | 含义 |
|---|---|
| `PersistentStore::save(generation, expected_revision)`（:88） | 单一强制保存入口；`expected_revision != revision_` → `CONFLICT`（另一写者先落，:93） |
| `SaveOutcome{snapshot, receipt, error, reason}`（:13-22） | `reason` ∈ `generation`/`revision`/`in-flight`/`""`；`receipt` 含 `generation_`/`revision_`/`save_counter_`（:95） |
| `confirm()`（:102-110） | 提交确认后 `committed_revision_` 前移到快照的 revision；落在保存后的写保持 dirty |
| `restore(generation, committed_revision, snapshot)`（:120-142） | 加载路径带版本转换；持久键缺失补默认值；`revision_ = committed_revision` |
| 脏标记 | 发送不清 dirty；仅 `confirm()` 清，且只清到快照实际捕获的 revision（:37-39） |

对应关系：契约摘要 = 生成器/`entities` 表绑定产物摘要（D28）；持久 revision = `committed_revision_`；回传序号 = `save_counter_`（在途保存的序号，确认时对齐）。三者共同定位「哪份快照、哪个版本、哪次保存」——崩溃后查询按此三元组重取。

### 验证（设计阶段列验收，实施轮运行时执行）

1. **data 重启还原**：`restore()` 从 SQLite 快照重建 `revision_`/`committed_revision_`，重启后写入资格与提交状态一致。
2. **提交失败不清脏**：`confirm()` 未到 → 快照 revision 之前的新写仍 dirty，不误清。
3. **在途保存期间再写**：保存后、`confirm()` 前的新写保持 dirty，`confirm()` 只清到快照 revision（:110）。
4. **旧代提交拒绝**：旧 `generation` 的 `save()` → `STALE`（:39 外来代）；不回退 `committed_revision_`。

### 缺口核对

- 适配层（SQLite ↔ `PersistentStore`）与「帧大小约束下的完整快照」序列化尚未接线；`MLD1` 现为 plain script traffic（`node.cpp:3063`）。按 D11「不重造已有 record_fence；不让业务多开强制保存入口」——接线须走 `PersistentStore::save` 单一入口。
- 查询索引与批量数据迁移待实际需求（D11「不做/延后」）；不默认 SQLite 文件存储具备多机高可用。

## D17 属性声明与唯一强制保存入口

来源：todo.md:146；尚未实现，依赖协议与实体契约链。D17 推荐统一成一条引擎保存路径，Lua 业务只提供状态或发起保存；属性同步与持久保存分开。

### 引擎命名空间中的显式保存入口

**保留只读引擎命名空间中的显式保存入口**，`avatar_save` 如保留则作为该路径调用的生命周期钩子。现有唯一入口：

- `PersistentStore::save(generation, expected_revision)`（`include/mistlane/entity_persist.hpp:88`）——单一强制保存入口；`expected_revision != revision_` → `CONFLICT`，外来 `generation` → `STALE`。`SaveOutcome` 携带 `snapshot`/`receipt`（`generation_`/`revision_`/`save_counter_`）/`error`/`reason`。
- `PersistentStore::confirm()`（:102）——提交确认，`committed_revision_` 前移到快照 revision；只在确认后清脏。
- `PersistentStore::restore()`（:120）——加载路径，带版本转换。

### avatar_save 与 writeToDB 的职责区分

D17 风险 3：文档同时出现 `avatar_save` 与拟议 `writeToDB`，若各自直接落库就破坏唯一入口。职责划分：

| 入口 | 职责 | 是否直连落库 |
|---|---|---|
| `avatar_save`（如保留） | 生命周期钩子，通知引擎「该保存了」 | 否——仅调用 `PersistentStore::save` |
| 拟议 `writeToDB` | 不新增 | 否——统一走 `PersistentStore::save` |
| `PersistentStore::save` | 引擎唯一强制保存路径 | 是（经适配层到 SQLite） |

「不恢复含混的 `self:sync()`」；「不把通知发出作为清脏依据」——清脏只由 `confirm()` 完成（D11）。

### 周期、迁移、退出保存都汇入同一队列和提交确认

- **周期保存**：引擎定时触发 → `PersistentStore::save`。
- **迁移保存**：MIG TAKEOVER 前源宿主强制 `save` + `confirm` 成功后才移交写权（D02/D06 栅栏顺序）。
- **退出保存**：进程退出前 `save` + 等待 `confirm`（durable ACK）才允许退出。

三者共用 `save_counter_` 序号与 `confirm()` 队列——崩溃后按 D11 三元组（契约摘要/持久 revision/回传序号）查询。

### 失败、重试与确认版本（对照 D11）

| 失败 | 行为 |
|---|---|
| `STALE`（外来 generation） | 拒绝；不回退已提交 revision；旧写者不复活（D13 栅栏后只向前） |
| `CONFLICT`（revision 前移） | 拒绝；另一写者先落；调用方重取基线（D10）后重试 |
| `in-flight` | 拒绝重复强制保存；等待在途 `confirm` |
| `confirm` 失败/超时 | 保持 dirty；不误清；重试或告警（D26 重试策略） |

### 缺口核对

- 属性同步与持久保存分开：属性推送走 MLE1 通知（`entity_endpoint.hpp`），持久保存走 `PersistentStore::save`——二者当前未统一接线，实施轮按 D11 适配层完成。
- 未获实施范围授权（D07）前不增加公开 API（D17「不做/延后」）；名称可在实现评审时一次定清，不仅为改名引入 API（D27）。

## D27 Avatar 钩子确切名字与签名

来源：todo.md:149；实现时定，关联 :146 的保存入口冲突。D27 推荐沿用文档九类生命周期职责，名称与签名在最小运行时落地时一次固定，保留引擎专用命名空间。

### 九类钩子：输入、返回、失败、允许状态、是否产生保存

依据 `docs/zh/avatar-runtime.md:68-76` 表格与状态机（:36-46），补齐允许状态与保存能力两列。**名称与签名在最小运行时落地时一次固定**——本表为推荐，实施轮按真实用例验证后冻结脚本接口。

| 钩子 | 输入 | 返回 | 失败处理 | 允许状态 | 产生保存？ |
|---|---|---|---|---|---|
| `avatar_load` | `(context, state)` | avatar 表 | 不发布 Avatar，带原因拒绝进入 | `loading`（→`active`/`closed`） | 否（加载不写） |
| `avatar_enter` | `(context)` | 无 | 带显式原因拒绝进入 | `active` | 否 |
| `avatar_leave` | `(context)` | 无 | 记日志；绝不阻塞解除绑定 | `active`/`suspended` | 否 |
| `avatar_tick` | `(context)` | 无 | 记日志；Avatar 保留 | `active` 唯一 | 否（tick 不触发保存） |
| `avatar_save` | `(context)` | state 表 | 上报运维；重试属发送队列 | `active`（含 `suspended` 期间未完成保存义务） | **是——唯一强制保存入口** |
| `avatar_transfer_out` | `(context)` | 状态快照 | 移交中止；Avatar 回 `active` | `active`（→`transferring`） | 是（移交前强制保存） |
| `avatar_transfer_in` | `(context, state)` | 无 | 到达 Avatar 在本宿主关闭；来源保留其 Avatar | `loading`（→`active`） | 否（接管不另写） |
| `avatar_detach` | `(context)` | 无 | 记日志；无论如何进 `suspended` | `active`（→`suspended`） | 否 |
| `avatar_fail` | `(context, reason)` | 无 | 尽力而为：VM 替换期间可能不运行 | 任一失败态 | 否（失败钩子不触发保存） |

### 统一强制保存路径（与 D17）

`avatar_save` 是唯一强制保存入口（`avatar-runtime.md:80`）：钩子做的任何事——发消息、推送、进场景——都不清脏、不通过副作用触发保存；清脏仅由 `PersistentStore::confirm()`（D11/D17）。`avatar_save` 如保留则作为 `PersistentStore::save` 路径调用的生命周期钩子，不直连落库。

### 重入证明（加载失败与迁移期间请求）

- **加载失败**：`avatar_load` 失败 → 不发布 Avatar、拒绝进入（:58）；`loading` 状态「任何处理器不得瞄准该角色，请求以显式失败拒绝，绝不隐形排队」（:51）。
- **迁移期间请求**：`transferring`/`loading`/`suspended` 期间到达的处理器以显式失败拒绝；已在执行的处理器在移交开始前结束（:92）。VM 串行（:54）保证同一时刻只有一个处理器在跑，不重入。
- **断线**：`avatar_detach` → `suspended`；`suspended` 不运行处理器，只完成未完成的保存义务（:53/104）。

### 冻结前的验证（实施轮运行时）

1. 加载失败 → 不产生可见 Avatar，重试可进 `loading`。
2. 迁移期间请求 → 显式失败，不隐形排队；移交失败来源 Avatar 回 `active`。
3. 断线重入 → 宽限期内直接回 `active`，不重新加载（:53）；宽限期到进 `closing`（:41）。

### 缺口核对

- 名称与签名按上表，在最小运行时落地时一次固定；保留引擎专用命名空间，不把引擎方法混入业务对象（D27 风险 3）。
- 不把 message handler 当完整生命周期；协议前置未齐（D14/D15）不开放新 Avatar 业务接口（D27「不做/延后」）。

## D01 目标进程职责迁移：五列验收表

来源：todo.md:42；显式 BLOCKED。D01 推荐保持现有职责与单二进制，按真实调用链逐段收口。

### 现有角色（单二进制，`--role` 选择）

`src/node.cpp`/`src/test_cases.cpp`/`src/selftest.cpp` 实测角色：`login`、`data`、`coordinator`、`gateway`、`gameServer`、`apiServer`、`probe`、`crossServer`。单二进制复用角色、CLI 与模板（D01 成本 1）。

### 五列验收表（入口—调用方—被调用方—持久结果—失败行为）

按真实调用链（D01 推荐：按调用链逐段收口，不再做名字迁移）：

| 入口 | 调用方 | 被调用方 | 持久结果 | 失败行为 |
|---|---|---|---|---|
| 登录请求 | 客户端 → gateway | login 签发短期 HMAC 凭据（D04） | 凭据绑定 data 已确认授权（D04） | 验签失败拒绝；不创建会话 |
| 路由查询 | login/gateway | coordinator `MLR1 ROUTE GET` | 实例放置记录（D06） | 无路由 → 拒绝，不隐式创建 |
| 角色加载 | coordinator | data `MLD1 GET` + gameServer `avatar_load`（D27） | 完整持久快照（D11） | 加载失败：不发布 Avatar，拒绝进入（D27） |
| 修改 | gameServer | `PersistentStore::save`（D17） | 版本化快照 + `committed_revision_`（D11） | STALE/CONFLICT/in-flight 拒绝（D17） |
| 跨宿主移交 | coordinator | MIG TAKEOVER（D02） | 源冻结、目标确认、`generation` 递增（D02） | 移交失败源回 `active`；旧写者不复活（D13） |
| 保存确认 | data | `PersistentStore::confirm`（D11） | durable ACK 后才清脏（D11） | confirm 失败保持 dirty，重试（D26） |

### 最小链路与解除条件

「用现有角色启动最小链路，缺哪段补哪段」：最小链路 = 登录分配 → coordinator 定位 → data 加载快照 → gameServer 承载 → 修改 → 保存确认 → 换宿主继续且旧宿主不能再写（决策文件「先理解怎么选」）。**解除条件是正常登录、保存和跨宿主移交真实通过**——需运行时环境，实施轮执行；本节记录验收表与缺口。

### 缺口核对

- 现有单二进制已按角色启动（`test_cases.cpp:2127` login/data 成对、`:2142` apiServer）；跨宿主移交的运行时闭环未验证（D02/D30）。
- 不新增 router/Base/Cell 进程（D01「不做/延后」）；不凭帮助文本一致就勾选总项；真实负载均衡延后。

## D04 login、data、可选 apiServer 契约与接线

来源：todo.md:65；部分完成但缺归属事实、授权与签发路径。D04 推荐沿用 login 签发短期 HMAC 凭据的既定方案，凭据必须绑定 data 已确认的授权；apiServer 继续可选，先保证持久受理，再单独完成实际执行。

### login 时序（分配 → data 授权 → 签发 → 宿主确认）

| 步 | 动作 | 事实 |
|---|---|---|
| 1 分配 | login 确定角色身份 | login 角色定义（`include/mistlane/roles.hpp:13`）；`MLS1` 服务身份帧（`node.cpp:169`）含 role/instance/token |
| 2 data 授权 | data 确认该角色归属 | `TicketVerifier`（`node.cpp:719`）；凭据绑定 data 已确认授权，不凭自报 |
| 3 签发 | login 签发短期 HMAC 凭据 | `ticket_secrets`（`node.cpp:122`）、`ticket_verdict_name`（:153）；短期，非 JWT（D04 成本 1） |
| 4 宿主确认 | 凭据送当前宿主 | 宿主经 `MLD1`/`MLR1` 校验；宿主 IP 不固化在订单（D04「不做/延后」） |

「验签通过 ≠ 取得写权」：凭据只证明身份，写权由 D06 放置记录 + D02 generation/fence 判定。

### API 受理记录到投递结果的状态设计

`ApiSqliteAcceptanceStore`/`ApiMemoryAcceptanceStore`（`node.cpp:2724/2726`）——apiServer 可选。受理→投递状态：

| 状态 | 含义 | 投递结果 |
|---|---|---|
| 已受理 | API 平台回调记录入 store | 不视为奖励已到账（D04 风险 3） |
| 已投递 | 请求送当前宿主 | 稳定角色标识定位宿主（D06 放置记录） |
| 已确认 | 宿主回 durable ACK | 才视为投递成功 |
| 失败/重试 | 投递失败或宿主不可达 | 按 D26 重试预算；耗尽进入只读/待恢复 |

### 重复投递幂等键与归属查询来源

- **幂等键**：API 订单按稳定角色标识 + 请求 id 幂等（`node.cpp:2131`「Applied 和 Duplicate 回答同一行：按请求 id 幂等」）；非幂等请求超时显示「结果未知」，不静默再执行（D09）。
- **归属查询来源**：coordinator 放置记录（D06），不固化宿主 IP 于订单。
- **持久/共享重放声明**：跨重启/多实例部署的前置任务；单实例也不能宣称重启后仍有防重放保证（D04 立即动作）。`ApiSqliteAcceptanceStore` 用 journal 持久化受理，属可共享重放的前置，多实例共享声明待部署验证。

### 缺口核对

- 不另选 JWT 服务或每客户端证书方案（D04「不做/延后」）；不把宿主 IP 固化在订单里；不把持久受理包装成完整支付到账。

## D05 工具、文档同步与整链路验收（可重复场景清单）

来源：todo.md:75；运行时前置条件未齐。D05 推荐保留总项未完成，只验收已经真实接通的能力；文档需分别说明模型验证、端点测试与真实多进程验证。

### 五类可重复场景及成功/失败预期

| 场景 | 步骤 | 成功预期 | 失败预期 |
|---|---|---|---|
| 登录 | 客户端 → gateway → login 签发 → coordinator 路由 → 宿主确认 | 短期 HMAC 凭据签发，宿主接受会话（D04） | 验签失败拒绝；无路由不隐式创建（D01） |
| 切场景 | 同进程场景引用变化 | Avatar 保持 `active`，只有实例引用变化，不保存/不做状态往返（D27） | 准备未完成不可进入；引用归零才释放（D24） |
| 迁移 | 跨进程移交（D02 四步） | 源冻结→目标加载确认→`generation` 递增→旧写者拒写 | 移交失败源回 `active`；旧写者不复活（D13） |
| 回调 | API 平台回调 → apiServer 受理 → 投递 → 宿主 ACK | 受理入 store，投递到当前宿主，durable ACK 才确认（D04） | 宿主不可达按重试预算；耗尽只读/待恢复（D26） |
| 多监听 | 普通 gateway / 复用连接 / cross gateway / MLC2+MLE1 | 各组合按 D09 支持矩阵正确路由 | 未支持的组合显式失败，不静默错发 |

### 过时文案核对

逐项检查「最终序号缺失」「TAKEOVER 缺失」「完全没有传输接线」「未接传输」「未接 coordinator」等表述（D05 立即动作）。落档轮已核对 `docs/` 下无残留过时断言；唯 `RELEASE_NOTES.md:24-25` 有过时句（见 D18 修订，本次实施轮一并修正）。

### 证据表与文档三分类

- **模型验证**：selftest 角色分组（`selftest.cpp:149-183`：coordinator.*/probe.control 等）。
- **端点测试**：多进程端点（`test_cases.cpp`：gateway/login/data/apiServer 组合）。
- **真实多进程验证**：最小链路（D01 解除条件），待运行时。

「历史部分测试通过，不能证明后续新增的整链路行为」——总项保持未完成（D05 推荐）；不把本轮只读核对写成新测试通过。

## D08 通用 Entity 身份与生命周期（状态表）

来源：todo.md:127；真实业务生命周期仍未收口。D08 推荐以通用 Entity 承接身份和生命周期，Avatar、Monster 复用机制；先服务一个最小测试实体。

### 状态表：loading/active/transferring/closing 能否接请求、跑计时器、保存

依据 `docs/zh/avatar-runtime.md:36-55` 状态机与 EntityRuntime（`entity_runtime.hpp`），把已有原语纳入状态表（D08 立即动作）：

| 状态 | 能否接请求 | 能否跑计时器 | 能否保存 | 说明 |
|---|---|---|---|---|
| `loading` | 否（显式失败拒绝，绝不隐形排队，:51） | 否 | 否 | 已向归属侧请求数据；任何处理器不得瞄准该角色 |
| `active` | 是（唯一运行 `message` 处理器与 `avatar_tick` 的状态，:52） | 是 | 是（唯一允许持久写，:52） | 承载版本为当前值时允许 |
| `transferring` | 否（无处理器、无 tick，:54） | 否 | 是（移交前强制 `avatar_save`，D27） | 移交进行中；在途工作在移交开始前完成（VM 串行） |
| `suspended` | 否（不运行处理器，:53） | 否 | 是（未完成保存义务仍会执行，:53） | 会话已断开；完成义务而非承载游玩 |
| `closing`/`closed` | 否 | 否 | 否 | 顺序清理完成；该角色在本宿主不存在 |

### 生命周期转移

`loading → active`（加载成功，进入被接受）/ `loading → closed`（加载失败，:37-38）；`active → suspended`（会话解除绑定，:39）/ `active → transferring`（移交进行中，:42）；`suspended → active`（宽限期内重新接入，:40）/ `suspended → closing`（宽限期到或显式关闭，:41）；`transferring → closed`（目标已接受，:43）/ `transferring → active`（移交失败或超时，:44）；`closing → closed`（清理完成，:45）；`closed → [*]`（:46）。

### 验证（设计阶段列，实施轮运行时执行）

1. **加载失败**：`avatar_load` 失败 → 不发布 Avatar、拒绝进入；状态 `loading → closed`（:38/58），重试可回 `loading`。
2. **重复创建**：同一 `incarnation` 二次创建 → coordinator 拒绝（D06 修改权）；已有 `active` 时不覆盖。
3. **销毁后迟到应答**：`closed` 后的迟到消息按 `incarnation` 拒绝（D06 验收 3），EntityRuntime 丢弃未知/迟到响应（`entity_runtime.hpp:77`）。

### 缺口核对

- 只有 spawn/destroy 原语尚不足以覆盖加载失败、断线和迁移边界（D08 风险 3）——`loading/transferring/suspended` 的运行时接线待实施；本节为状态表设计。
- 不新增 Base/Cell；不实现旧玩法；未满足旧协议门控前不新增面向业务的 Avatar API（D08「不做/延后」）。

## D10 全量基线、增量、重连与观察者版本（消息顺序）

来源：todo.md:134；现有为进程内模型。D10 推荐先把已验证的基线/增量模型接上一种真实传输，重连先重新取基线；只有协商兼容的接收者才接收对应字段。

### 消息顺序规定

| 事件 | 顺序 | 说明 |
|---|---|---|
| 加入 | 先基线、后增量 | 观察者先收到完整基线，再收增量；避免半更新副本（D10 风险） |
| 离开 | 停止发送增量，可发离开标记 | 观察者清理该实体 |
| 重新连接 | 先重新取基线 | 重连多花少量带宽，换取更易证明的正确性（D10 取舍） |
| owner 变化 | 私有属性在 owner 变更时重新基线 | 所有者私有属性不因混合版本或重连误发给其他观察者（D10 风险 3） |
| 摘要不兼容 | 拒绝协商，请求基线 | 只有协商兼容的接收者接收对应字段（D10 推荐） |

### 验收（设计阶段列，实施轮运行时执行）

1. **缺号整批拒绝**：收到不连续增量（缺号）→ 整批拒绝，重新拉基线（D10 立即动作）。
2. **重新拉基线**：重连/摘要不兼容 → 重取基线，不从中间增量续传（不做断点续传，D10「不做/延后」）。
3. **私有属性不泄漏**：owner 私有属性只发当前 owner；owner 变化时重新基线且旧观察者不残留私有字段。

### 缺口核对

- 现有基线/增量为进程内模型；接一种真实传输（MLE1/MLD1）待运行时。
- 不以进程内字节测量代替实际网络性能（D10「不做/延后」）；不把同步受众当作空间 AOI。

## D12 Lua 安全替换与新契约实体迁移（四类资源处理表）

来源：todo.md:140；网络实体生命周期仍是前置。D12 推荐先支持契约不变、处理器边界上的安全替换；新契约通过新进程加载，再按交接协议迁移。

### 四类资源的处理表（替换前排空与失败恢复）

「仍在执行的请求/新请求/定时器/未确认保存」四类（D12 立即动作）：

| 资源 | 替换前排空 | 替换失败恢复 | 说明 |
|---|---|---|---|
| 仍在执行的请求 | 允许在途完成（VM 串行，D08）；替换不打断 | 旧 VM 保留 | 处理器边界上安全替换，不交错改状态 |
| 新请求 | 替换窗口内拒绝/排队至新 VM | 旧 VM 继续接受 | 有界队列留在会话与传输层（D27） |
| 定时器 | 替换前清空或随旧 VM 退役 | 旧 VM 定时器恢复 | 计时器有明确归属，避免两套脚本交错 |
| 未确认保存 | 替换前强制 `save` + 等待 `confirm`（D17） | 保持 dirty，旧 VM 重试 | 不清脏；`PersistentStore` 单一入口 |

### 替换验收

1. **替换前排空**：SIGHUP 替换前在途完成、定时器清空、未确认保存 `confirm` 后才切换。
2. **替换失败恢复旧绑定**：新 VM 加载失败 → 保留旧服务（已有加载失败保留旧服务机制，D28）；旧写者不因失败复活（D13 栅栏后只向前）。

### 跨进程迁移复用 D02 栅栏顺序

新契约通过新进程加载，再按交接协议（D02 四步）迁移；跨进程迁移必须复用 D02 的栅栏顺序（`record_fence` + generation）。不在活跃 VM 中原地更换契约（D12「不做/延后」）；不在旧代已作废后恢复旧写者。

### 缺口核对

- SIGHUP 成功不代表对象状态、权限和计时器已迁移（D12 风险 3）；完整热升级待真实运行时闭环。
- 安全替换先缩小到脚本替换（契约不变），复用已有加载失败保留旧服务机制（D28）。

## D13 新旧客户端、兼容与迁移失败验证（三类用例）

来源：todo.md:141；端到端验证缺落点。D13 推荐保留现有兼容闸和黄金向量，待运行时可用后再做两版客户端与两版契约的最小组合验收。

### 三类用例

| 用例 | 客户端 | 契约 | 验收 |
|---|---|---|---|
| 1 旧+旧 | 旧客户端 | 旧契约 | 字节能解码；业务状态安全（基线兼容） |
| 2 新+新 | 新客户端 | 新契约 | 新字段默认值；版本过滤 |
| 3 旧观新 | 旧客户端 | 新契约 | 观察新宿主；私有属性不泄漏（D10） |

「同时验证字节能解码和业务状态能安全迁移」：用例 1/2 覆盖编解码；用例 3 覆盖混合观察者场景（D13 风险 3）。不穷举历史版本（D13 取舍）——先覆盖新增字段/默认值、被拒绝的不兼容变更和保存失败。

### 同一服务器二进制摘要

三类用例记录同一服务器二进制摘要（构建标识，D35 诚实开发构建标识）；保证「字节能解码」和「业务状态能安全迁移」在同一二进制下对照。

### 失败恢复（栅栏后只向前）

栅栏后失败只向前恢复——不能把「回滚」理解为旧写者复活（D13 立即动作）：`generation` 递增后旧写者被 `record_fence` 拒绝，失败恢复重新加载目标而非回退旧写权。保存失败：`confirm` 未到保持 dirty，重试（D17/D26）。

### 缺口核对

- 现有兼容闸与黄金向量（跨语言编码测试）保留；生成新 SDK 不代表旧客户端在混合观察者场景下仍安全（D13 风险 3）。
- 不仅凭 SDK golden 宣称整链路兼容（D13「不做/延后」）；前置运行时未齐时不伪造通过记录。

## D14 Avatar 抽象的旧协议前置门控（三个验收条件）

来源：todo.md:143；显式 BLOCKED。D14 推荐保留「补齐协议证据后再做新 Avatar API」的限制；通用测试实体可用于验证机制，但不能冒充原游戏兼容实现。

### 三个验收条件（关联 D15/D16）

| 条件 | 所需证据 | 来源项 | 满足时 |
|---|---|---|---|
| 1 协议源 | `.sproto` 文本及引用类型、生成工具与版本/命令、匹配提交/构建标识 | D15 索取清单 | 可从源定义重新生成服务器与客户端产物 |
| 2 生成入口 | 生成工具与版本/命令、对应客户端绑定 | D15 | 建立可追溯关系（源→产物） |
| 3 客户端绑定 | 对应客户端绑定、脱敏样例及可使用范围 | D15/D16 | 真实请求/响应闭环验收 |

### 门控规则

- 「补齐协议证据后再做新 Avatar API」：暂缓的是业务接口承诺（D14 取舍）；已存在的通用原语仍可复核（D08/D27）。
- 缺协议源时凭猜测设计方法，会把不确定性固化成 API（D14 风险 3）——不新增原游戏 Avatar API；不将 Protobuf 依赖当成旧 sproto 契约证据（D14「不做/延后」）。
- 若维护者未来明确放弃旧兼容，须单独声明新的产品边界，不能默认绕过门控（D14 立即动作）。

## D15 原始协议、生成脚本和正式客户端绑定（索取清单）

来源：todo.md:144；显式 BLOCKED，真正需要外部材料。D15 推荐先向维护者索取同一版本的最小材料包；拿不到时保留「无法证明旧兼容」，不靠反编译补成正式来源。

### 索取清单（待用户：材料本身为维护者私有数据，本轮未发送索取消息）

| 项 | 内容 | 用途 |
|---|---|---|
| 1 `.sproto` 文本 | 及引用类型 | 从源定义重新生成服务器与客户端产物 |
| 2 生成工具 | 与版本/命令 | 建立可追溯关系（源→产物） |
| 3 客户端绑定 | 对应客户端绑定 | 真实请求/响应闭环验收（D14 条件 3） |
| 4 匹配提交/构建标识 | 与服务器同版本 | 保证源与产物同源 |
| 5 脱敏样例 | 及可使用范围 | 验证而不泄露业务数据 |

没有源文件时，先请维护者提供备份/旧客户端工程/构建记录的查找线索（D15 立即动作）。本轮未发送任何索取消息；不把 `sproto.spb`、`Protocol.lua` 或 `ProtoMaker.lua` 当完整生成流程；不把恢复出的字段猜成业务真相；不把私有旧业务材料加入公开源码（D15「不做/延后」）。

### 缺口核对

- 二进制 schema 可恢复部分结构，却无法自证生成选项、业务权限和来源版本（D15 风险 3）——正式兼容承诺等待证据。
- 允许继续离线整理可恢复事实；「无法证明旧兼容」保留为结论，不靠反编译补成正式来源。

## D02 coordinator 注册、归属与迁移（四步时序）

来源：todo.md:48，及 :51/:58 保留阻塞说明。D02 推荐 coordinator 管放置与交接编排、home data 管写入资格；查询到路由不能直接获得写权。复用既有 journal、generation 与 TAKEOVER。

### 四步时序

| 步 | 动作 | 凭证 | 重复请求 | 崩溃后查询 |
|---|---|---|---|---|
| 1 源停止处理 | 源宿主停止接受该实体的新处理器；完成在途；`avatar_transfer_out` 取状态快照（D27） | 源持有当前 `generation` 写权（`HomeSequenceAuthority`） | 重复 STOP → 幂等返回当前状态 | 源崩溃：coordinator 按路由记录发现源不可达，走 TAKEOVER |
| 2 data 确认冻结 | data 冻结该实体的写入资格；源的最后写入经 `record_fence` 拒绝（`journal.cpp:369/544`） | data 收到源确认的 `generation` | 重复 FREEZE → 幂等确认 | data 崩溃：重放 journal 恢复 `record_fence` 状态（:559） |
| 3 目标加载并确认 | 目标宿主 `avatar_load`（D27）→ 用冻结快照恢复 → `MLR1 MIG TAKEOVER`（`node.cpp:2193`）确认加载 | 目标从冻结的 `record_fence` 继承 `generation` | 重复 LOAD → 目标拒绝/幂等 | 目标崩溃：coordinator 重试或回退源 |
| 4 coordinator 比较旧值后发布新路由 | coordinator 用记忆中的旧 `generation` 与源最后确认值比较；一致才 `MLR1 MIG COMMIT` 递增 `generation` 并发布 | coordinator 是唯一权威（D06 放置记录修改权） | 重复 COMMIT → 幂等；不一致 → 拒绝并令重取 | coordinator 崩溃：从 journal 恢复放置记录与 `generation` |

### 核对现有原语与线动词（只补缺口）

已有：`HomeSequenceAuthority`（`record.hpp:125`）、`fence`（:153）、`final_sequence`（:203）；`record_fence` 持久化与恢复（`journal.cpp:369/544/559`）；`MLR1 MIG` 全部动词（`node.cpp:2026-2041` 含 TAKEOVER :2193）；`PersistentStore::save/confirm/restore`（D11）。

**缺口（实施轮补，不重造）：** 放置记录表持久化（D06 缺口）；coordinator 的「比较旧值」目前是内存协调面，需持久化比较基准才能跨崩溃正确。不重复实现 SQLite journal 或 TAKEOVER；不以注册表成员消失当成安全接管（D02「不做/延后」）。

### 写权与路由的区别

查询到路由（`MLR1 ROUTE GET`）只能给出「实体在哪」，**不能**直接获得写权；写权由 `generation` + `record_fence` 判定（D02 推荐）。交接期间允许短暂不可用，优先避免状态分叉（D02 取舍）——旧写者无法通过过期路由继续写。
