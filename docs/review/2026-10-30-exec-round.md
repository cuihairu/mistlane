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
