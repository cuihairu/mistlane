# login、data 与 apiServer 服务契约

状态：契约设计（2026-10-19）；实现状态按各节标注。本页定义 `login`、`data` 与 `apiServer` 三个角色说什么协议、谁连接它们，为跨服回传协议（它需要一个归属侧端点决定）与后续迁移条目提供输入。职责边界仍以[进程职责](./process-roles.md)为准；归属规则见[跨服回传与唯一写入权协议](./cross-return-protocol.md)。

## 本页做出的决定

- **MLD1 的归属侧端点就是 `data` 角色本身。** 归属区服的 gateway 可以为它做传输前置，但持久确认的权威是 data 服务——不是 coordinator，也不是 gameServer。此项关闭回传协议页上的未决问题。
- **三个角色都复用现有运行时面**——带可选 `MLS1` 身份的帧式 TCP 会话、Lua `message`/`http` 钩子、`MLR1` 注册表访问，以及 apiServer 现有的 HTTP 栈。不为它们引入新传输层。
- **login 与 data 已可作为普通脚本服务运行**（`--role login`/`--role data` 带 `--script` 启动，标准钩子与帧式会话，形同没有场景的 gameServer）。login 的分配查询、data 侧 MLD1 记录端点、进入凭据 C++ 原语与凭据的 `MLC1` 线形已实现；宿主的凭据验证与秘钥配置、持久加载与持久化流程仍是契约层。apiServer 已在运行，本页补契约文档，不加代码。
- **服务间的进入凭据定为短时效 HMAC-SHA256 凭据**（2026-10-19 依据 2026-10-14 既有授权代决）：login 在自身凭据校验之后，以区服预共享秘钥（来自配置）对（realm、character、目标 instance、承载 generation、有效期、nonce）签发一张凭据，接收侧 gameServer/crossServer 先验证凭据再接受进入或移交——宿主绝不在裸声明上加载角色。被否选项：每客户端 PKI/mTLS（需要本部署不具备的 CA 与轮换运维）、外部 JWT/认证服务（新增进程，且把依赖压进入口热路径）、以及框架不出凭据（宿主将信任未验证声明，破坏唯一写入权规则）。C++ 原语已作为 MLT1 落地（`include/mistlane/entry_ticket.hpp`）：以共享的 `hmac_sha256` 对规范字节签发与验证，区服秘钥表、对照到达 character/instance/generation 的身份校验、含有效期末刻的过期判定、时钟回退闭环与有界已见声明表，外加线形（`entry_ticket_hex`/`parse_entry_ticket`：规范字节加原始签名的十六进制，作为 `MLC1` 可选 `tk` 尾缀组上行，绑定握手声明的 character 与承载版本）——但没有真实秘钥、没有秘钥配置加载、没有运行验证器的宿主，声明表仍按进程隔离，`Replay` 只对单一接收方诚实。`MLS1` 预共享令牌仍是服务链路身份。

## login

login 拥有进入流程：验证账号、展示角色列表、取得承载分配并把玩家交给 gameServer。它面向客户端，且绝不持有可写角色。

| 方面 | 契约 |
|---|---|
| 传输 | 与会话角色一致的帧式 TCP（或经 gateway 的 WS）；配置令牌时内部链路使用 `MLS1` 身份 |
| 对端 | 入向：客户端与 gateway；出向：coordinator 注册表（以注册表客户端身份 `MLR1 QUERY`/`MLR1 PLACE`）、data（入场前必须取回的角色列表/存档） |
| Lua 面 | 同一套 `message`/`init`/`start`/`tick`/`stop`/`shutdown` 钩子；login 处理器应答认证、角色列表与进入请求；没有场景或 Avatar 对象 |
| 承载分配 | login 向 coordinator 发 `MLR1 PLACE gameServer [NS <namespace>] <scope>` 并在进入应答中带回分配地址；不得把地址写进持久记录（职责规则）；`MLR1 EMPTY` 是进入拒绝，不是可吞掉的错误 |
| 进入上下文 | 进入应答携带来源区服、角色标识、归属 data 签发的承载 generation、返回路由及 login 凭据；gameServer 与后续 crossServer 使用同一参与者上下文 |
| 认证边界 | 凭据验证是应用层 Lua 决策；服务之间，进入凭据是已拍板的 HMAC-SHA256 短时效凭据，其 C++ 签发/验证原语已作为 MLT1 实现（见上方决定）。线形已绑定：凭据随 `MLC1` 的 `tk` 尾缀上行，解码失败的尾缀整次握手拒绝。验证已接线：配置 `MISTLANE_TICKET_SECRETS` 的 crossServer 在握手上验证凭据（失败答 `MLC1 ERR TICKET`，脚本经 `context.verified` 读取结论）；声明表按进程且易失。仍未接线：login 侧签发路径（Lua 沙箱不开密码库）与围绕分配的完整入场流程 |
| 状态 | login 至多持有连接状态与未完成的承载记录；没有任何持久数据。进入应答之后，角色路由归属 gameServer/coordinator，而不是 login |

generation 生命周期（设计，非运行时）：分配选出目标实例后，login 向归属 data 请求 `(realm, character, target instance)` 授权。归属 data 是唯一 generation 权威：新键从 1 开始；重新分配或接管须持久分配严格更大的 generation，栅栏/墓碑旧授权，并按[有序租约交接](./cross-return-protocol.md#写入租约交接)激活目标。中止或超时后不得复用已分配 generation。login 在进入上下文返回 data 签发的 generation，并与目标实例一起绑定进凭据；接收宿主在持久写前验证凭据及归属 data 的有效授权。宿主不能自发或推进 generation。当前 MLD1 LOAD 只观察内存记录，SUBMIT 接受更大数字不构成租约授权。授权/预留/激活线消息、持久权威与宿主的凭据验证（含区服秘钥配置）仍归归属执行缺口，尚未实现；凭据编码与线形已随 `MLC1` 的 `tk` 尾缀落地。

接线状态：已可作为脚本服务启动（`--role login` 加 `--script`，帧式会话与标准钩子）；`--coordinator ADDR:PORT [--namespace NAME] [--scope NAME]` 接通注册表客户端——Lua `mistlane.place(role, namespace?, scope?, callback)` 每次查询经新连接发起 `MLR1 PLACE`，把分配地址（或原因）交回脚本。进入凭据 C++ 原语已作为 MLT1 实现（规范字节签发/验证、区服秘钥、身份/过期/重放校验），凭据也随 `MLC1` 的 `tk` 尾缀上行，但没有宿主验证它：尚无区服秘钥配置，Lua 沙箱也仍不开启任何密码库。同样未实现：分配外围的完整进入流程。

## data

data 拥有其归属区服的持久角色存储：唯一的记录写入方、MLD1 端点，以及填充 gameServer 或 login 请求的加载路径。

| 方面 | 契约 |
|---|---|
| 传输 | 帧式 TCP，每条链路都要求 `MLS1` 身份（成员以 `--upstream-token` 呈递）；gateway 可作前置，但记录在案的对端是服务，从来不是玩家设备 |
| 对端 | 入向：gameServer/crossServer（加载、MLD1 提交、保存）、login（角色列表）；除数据库层外没有任何出向 |
| 持久权威 | data 仅在自身写入持久之后以 `ACK durable` 应答 MLD1 `SUBMIT`；generation 栅栏与按角色序号去重是 data 侧职责（见回传协议） |
| 加载路径 | 加载请求返回角色持久状态及其当前承载版本；请求方已持有的版本让归属侧能拒绝过期声明 |
| 持久链落地之前 | data 侧持久化链（实体契约条目）未实现。在其存在之前，data 无法发出真实的持久确认；过渡实现必须完全不发送 `ACK durable`，让发送方停留在重试/只读状态——这是诚实的降级，不是捷径 |
| 排除 | 业务 schema、SQL、缓存表语义；存储引擎与脏追踪归实体契约链 |

接线状态：角色外壳已可启动（`--role data` 加 `--script`），`--service-token` 对每条链路强制 `MLS1` 身份，MLD1 端点已在运行（帧式 TCP 上的 `MLD1 LOAD`/`SUBMIT`）：承载版本栅栏、按角色序号纪律与重发去重已在框架内实现，端点以 `MLD1 RECEIVED` 应答——回执而绝非持久性声明，发送方按协议要求继续保留重试队列。仍未实现：持久化链，因此今天任何地方都没有 `ACK durable`，记录仅在进程内（重启后应答 `MLD1 EMPTY`）；持久加载路径同样只从这份内存记录应答，别无更多。

## apiServer

apiServer 已实现且保持现状；本节记录其契约边界。

| 方面 | 契约 |
|---|---|
| 传输 | 默认 HTTP/HTTPS；按现有监听模型可选 WS/WSS message 监听 |
| 对端 | 入向：平台/运营系统。它不在玩家路径上，也不得成为隐蔽的游戏消息路由器 |
| Lua 面 | `http(request)` 带 `before_request`/`after_request`/`on_error` 钩子；升级的监听上有 WS 生命周期钩子 |
| 身份 | 当前为可信网络；`MLS1` 不适用于 HTTP。平台回调认证已定为类型化 `MLA1` 契约（规范字节 + `HMAC-SHA256` + 防重放窗，[协议分析](./protocol-analysis.md#external-api-surface-first-slice)），并已作为原生 `POST /mla1` 拦截接通 HTTP 栈，凭据仅由环境变量注入；轮换/撤销已决定并测试（凭据行可选 `valid_until`）；持久受理后端与归属事实来源仍未做 |
| 幂等 | 类型化受理语义已定：`(audience, principal, realm, character, idempotency_key)` 的 create-or-read，区分 `Inserted`/`Existing`/`Conflict`，附只读归属查询。在线处理器仍是请求作用域（未接线）；持久受理后端仍未实现 |
| 寻址 | 触达玩家走角色的归属服务，绝不固化进程地址；apiServer 没有注册表成员身份，也不做承载决策 |

## 此处刻意不定的事项

- 面向客户端的凭据格式与账号体系：应用域事务，刻意留在框架之外。服务侧凭据方案本身已拍板（HMAC-SHA256 短时效凭据、区服预共享秘钥），其 C++ 原语已作为 MLT1 实现；此处不有的是接线——宿主验证与秘钥注入，Lua 沙箱仍不开启任何密码库。
- 角色状态 schema 与脏语义：实体契约链。
- 结算与奖励规则：玩法。

旧证据与目标边界：[玩家、场景与跨服模型](./player-scene-model.md)；回传协议：[跨服回传与唯一写入权协议](./cross-return-protocol.md)。
