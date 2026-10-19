# login、data 与 apiServer 服务契约

状态：契约设计（2026-10-19）；实现状态按各节标注。本页定义 `login`、`data` 与 `apiServer` 三个角色说什么协议、谁连接它们，为跨服回传协议（它需要一个归属侧端点决定）与后续迁移条目提供输入。职责边界仍以[进程职责](./process-roles.md)为准；归属规则见[跨服回传与唯一写入权协议](./cross-return-protocol.md)。

## 本页做出的决定

- **MLD1 的归属侧端点就是 `data` 角色本身。** 归属区服的 gateway 可以为它做传输前置，但持久确认的权威是 data 服务——不是 coordinator，也不是 gameServer。此项关闭回传协议页上的未决问题。
- **三个角色都复用现有运行时面**——带可选 `MLS1` 身份的帧式 TCP 会话、Lua `message`/`http` 钩子、`MLR1` 注册表访问，以及 apiServer 现有的 HTTP 栈。不为它们引入新传输层。
- **login 与 data 今天已可作为普通脚本服务运行**（`--role login`/`--role data` 带 `--script` 启动，标准钩子与帧式会话，形同没有场景的 gameServer）；服务流程——分配、加载、持久化——仍停留在契约层。apiServer 已在运行，本页补契约文档，不加代码。

## login

login 拥有进入流程：验证账号、展示角色列表、取得承载分配并把玩家交给 gameServer。它面向客户端，且绝不持有可写角色。

| 方面 | 契约 |
|---|---|
| 传输 | 与会话角色一致的帧式 TCP（或经 gateway 的 WS）；配置令牌时内部链路使用 `MLS1` 身份 |
| 对端 | 入向：客户端与 gateway；出向：coordinator 注册表（以注册表客户端身份 `MLR1 QUERY`/`MLR1 PLACE`）、data（入场前必须取回的角色列表/存档） |
| Lua 面 | 同一套 `message`/`init`/`start`/`tick`/`stop`/`shutdown` 钩子；login 处理器应答认证、角色列表与进入请求；没有场景或 Avatar 对象 |
| 承载分配 | login 向 coordinator 发 `MLR1 PLACE gameServer <scope>` 并在进入应答中带回分配地址；不得把地址写进持久记录（职责规则）；`MLR1 EMPTY` 是进入拒绝，不是可吞掉的错误 |
| 进入上下文 | 进入应答携带来源区服、角色标识与返回路由——与跨服回传协议定义的参与者字段同形，使 gameServer 与后续 crossServer 使用同一种上下文 |
| 认证边界 | 凭据验证是应用层 Lua 决策；框架只提供传输、分帧与身份，不提供账号、凭据或超出连接本身的会话 |
| 状态 | login 至多持有连接状态与未完成的承载记录；没有任何持久数据。进入应答之后，角色路由归属 gameServer/coordinator，而不是 login |

接线状态：已可作为脚本服务启动（`--role login` 加 `--script`，帧式会话与标准钩子）。未实现：注册表客户端与分配流程，以及其后的凭据/认证设计。

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

接线状态：角色外壳已可启动（`--role data` 加 `--script`），`--service-token` 已能对每条链路强制 `MLS1` 身份。未实现：加载路径、MLD1 端点与持久化链——MLD1 确认等待持久化链。

## apiServer

apiServer 已实现且保持现状；本节记录其契约边界。

| 方面 | 契约 |
|---|---|
| 传输 | 默认 HTTP/HTTPS；按现有监听模型可选 WS/WSS message 监听 |
| 对端 | 入向：平台/运营系统。它不在玩家路径上，也不得成为隐蔽的游戏消息路由器 |
| Lua 面 | `http(request)` 带 `before_request`/`after_request`/`on_error` 钩子；升级的监听上有 WS 生命周期钩子 |
| 身份 | 当前为可信网络；`MLS1` 不适用于 HTTP。平台回调需要认证决策（签名/秘钥）才能上生产——本页不改变这一点 |
| 幂等 | 持久、幂等的回调受理（平台回调条目）仍未实现；今天的处理器是请求作用域的 |
| 寻址 | 触达玩家走角色的归属服务，绝不固化进程地址；apiServer 没有注册表成员身份，也不做承载决策 |

## 此处刻意不定的事项

- login 凭据格式、凭据签名与会话令牌：绑定 login 实现切片的认证设计。
- 角色状态 schema 与脏语义：实体契约链。
- 结算与奖励规则：玩法。

旧证据与目标边界：[玩家、场景与跨服模型](./player-scene-model.md)；回传协议：[跨服回传与唯一写入权协议](./cross-return-protocol.md)。
