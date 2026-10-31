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
