# 业务语义 bot 与内置仪表盘

本文对源码逐条核对而成，不是凭空描述：下面每一帧都是服务端源码实际解析的格式（`src/node.cpp`、`include/mistlane/entity_envelope.hpp`、
`include/mistlane/entity_endpoint.hpp`、`include/mistlane/entity_codec.hpp`）。

## 线协议取证：一个业务帧到底是什么

传输分帧（所有 TCP 角色）：`u32 BE 长度 | payload`，payload 上限 1 MiB（`max_frame`）。

会话上的握手（发生在第一个业务 payload 路由之前）：

| 帧 | 方向 | 含义 |
|---|---|---|
| `MLS1 <role> <instance> <token>` → `MLS1 OK` | 双向 | 服务身份；以 `--service-token T` 启动的节点只接受 MLS1 对端，且该对端的帧被归因为 `caller = server` |
| `MLC2` → `MLC2 OK` | 客户端→服务端 | 关联模式：此后每帧都是 `u32 BE 请求 id | payload`，回复回显该 id，推送携带 id 0 |

业务 payload 类型（实体运行时，由 `gameServer/crossServer --contract FILE.def` 承载）：

```
请求:  "MLE1" | envelope（39 字节，大端） | sproto 类型化参数
回复:  "MLE1" | correlation u64 BE | EntityReply
```

Envelope 的线上顺序（注意：C++ `EntityEnvelope` 结构体里 `incarnation` 声明在 `entity_id` *之前*，而线上先传 `entity_id`——
由 `encode_entity_envelope` 做映射，所以永远不要按位置初始化该结构体）：

| 偏移 | 字段 | 宽度 |
|---|---|---|
| 0 | `MLE1` 标记 | 4 |
| 4 | scope | u32 |
| 8 | type_id | u32 |
| 12 | entity_id | i64 |
| 16 | incarnation | u32 |
| 20 | side（0 客户端，1 服务端） | u8 |
| 21 | method_id | u16 |
| 23 | correlation_id | u64 |
| 31 | hosting_generation | u32 |
| 35 | deadline_ms | u32 |
| 39 | payload | 其余 |

`EntityReply`：`0x00 | 返回 payload`，或 `0x01 | 错误码`（`0x01` 类型 + 码 `0..9`；只有 `INVALID` 追加 `u16 len | reason`）。
`correlation_id == 0` 表示通知：没有回复。同一个实体地址上第二次出现的 `correlation_id` 是 `CONFLICT`。

参数与返回值都走 schema 驱动的 sproto 编解码（`SprotoCodec`，与生成的 JS/C# SDK 字节兼容）：小端 tag 块，其后是字段数据
（小整数走带内整数词，字符串/字节/结构体走带长度的前缀）。

调用方归属由传输侧判定，从不写在 envelope 里：经 MLS1 识别（或多路复用内部）的对端以 `caller = server` 调用；普通客户端链路是
`caller = client`，在 owner 归属落地之前对 `caller="server"` 方法始终保持 `FORBIDDEN`。这正是场景 bot 要出示 MLS1 的原因。

实体生命周期：实体只有被路由之后才能被寻址。脚本用 `mistlane.entity.spawn(scope, type_id, incarnation, entity_id [, generation])`
播种路由表；向未播种的地址发请求得到 `UNROUTED`，`hosting_generation` 不对得到 `STALE`。

## 场景 bot（`mistlane bot --scenario business`）

新参数（全部可选；不带 `--scenario` 时 bot 的 echo/expect 行为与之前逐字节一致——ctest 的 `cli.bot` 覆盖该路径）：

```
mistlane bot --url tcp://HOST:PORT --scenario business --contract FILE.def \
  [--service-token T] [--spawn] [--scope N --entity-type N --incarnation N
  --generation N] [--entity-base N] [--rooms N] [--heartbeat-every N]
  [--ramp-ms N] [--clients N] [--count N] [--interval-ms N] [--timeout-ms N]
```

每条连接与 echo 模式一样串行请求/响应：

1. 可选的 `MLS1 <bot> bot-<c> <token>` 握手（期望 `MLS1 OK`），
2. 可选的 `spawn <entity-base+c>` 文本帧（期望 `spawned`；由宿主脚本路由实体），
3. `login`（token 为 `bot-<c>`）、`joinRoom`（`1 + c % rooms`），
4. `count` 个房间步：每第 `heartbeat-every` 步是 `heartbeat`，其余是 `move`（x = step，y = connection），每步都做应答校验
   （关联回显 + OK 回复类型）。

`--ramp-ms` 把各连接的启动时刻在窗口内错开（KBEngine `defaultAddBots` 斜坡三元素的最小形态：总数 × 步长 × 延迟）。
输出会追加 `scenario=business logins_ok= joins_ok= moves_ok= heartbeats_ok=`；任何校验失败都会让该连接失败、整轮以非零码退出。

参考宿主文件随 `examples/` 提供：`contracts/business.def`（Player 实体：login/joinRoom/move/heartbeat/leaveRoom，自包含，bot 与节点加载同一份文档）
和 `business.lua`（spawn/rooms 消息处理加实体实现表，把房间占用发成 `rooms` 脚本 gauge）。

## 独立压测与故障注入驱动（`tools/bots`）

`mistlane-bots` 是仓库内另一个独立可执行文件，与上面内嵌的 `mistlane bot` 共用同一套业务语义与会话层（framed TCP、MLS1、
MLE1 实体步、MLD1 record 腿），由 TOML 计划文件驱动：

- **load 模式**：计划声明并发连接数、ramp 窗口、目标速率与时长；统计输出完成/失败数、目标达成率与
  p50/p95/p99 延迟，并按消息类型分组；
- **inject 模式**：主动注入坏契约帧、迟到/重复 ACK、慢客户端与断连风暴等故障面，验证节点的 durable-ack、序号缺口（gap）
  与容量（full）防护在压力下确实成立；某个防护被击穿时该轮以非零码退出并如实报告。

构建与用法见 [tools/bots/README.md](https://github.com/cuihairu/mistlane/blob/main/tools/bots/README.md)，示例计划见同目录
`plan-example.toml`。它面向本仓自己的节点做问题发现，不是通用压测产品。

## 内置仪表盘（`--dashboard-listen IP:PORT`）

一个只读监听器，提供：

- `GET /` 或 `/dashboard`——单页仪表盘（`include/mistlane/dashboard.hpp`，内联进二进制；零外部资源、无 CORS、无构建步骤），
- `GET /metrics`——与 `--metrics-listen` exposer 完全相同的 Prometheus registry 文本（`Diagnostics::metrics_text()`），
  因此页面与抓取端不可能出现口径差异，
- 其他路径——404。连接数（64）、请求字节（单行 2 KiB）与时间（5 s）都有上界，与 probe 服务一致。

面板：连接数（`mistlane_active_sessions`，含 sparkline）、房间（`mistlane_script_gauge{name="rooms"}`——脚本经
`mistlane.metrics.gauge(name, value)` 声明的业务状态）、消息吞吐（`mistlane_messages_total` 的 tx/rx 速率）、请求延迟
（`mistlane_request_seconds` sum/count）、事件循环延迟 + RSS，以及实体裁决（`mistlane_entity_results_total`）。每 2 秒刷新。

脚本 gauge API 经 `ScriptMetricsSink` 接线（由 `Diagnostics` 实现，family `mistlane_script_gauge`，最多 32 个不同名字）。
只要设置了 `--metrics-listen` 或 `--dashboard-listen` 之一，registry 就会构建，所以只开 dashboard 的节点同样提供完整指标集。

## 验证

`mistlane test --case business.bot`（ctest：`mistlane_business_bot`，suite `cli`）针对真实子进程驱动整条链：一个带 service token、
metrics 与 dashboard 监听的契约宿主 gameServer；一个带 ramp 的 4 连接 bot；页面/404 检查；以及一致性断言——dashboard
`/metrics` 等于 exposer 的 readiness 行，`rooms` gauge 等于 2，实体 `ok` 裁决等于 32，`mistlane_messages_total` 的 rx 等于 tx
等于 40（每个读到的帧都恰好产生一个回复）。
