# 协议清单

状态：2026-11-21 从已恢复 schema（[协议契约恢复](./protocol-contract-recovery.md)）加 game-fogs 处理器注册与错误码审计构建。本页列出旧业务协议的线上消息及其编号、字段布局、方向、处理器与错误码；逐处理器授权与版本兼容规则记录为待办/缺失。英文页：[Protocol inventory](../protocol-inventory.md)；数据：`contracts/fogs-proto-inventory.json`；生成器：`tools/sproto_inventory.py`。

## 清单捕获了什么

`contracts/fogs-proto-inventory.json` 是 **646 个协议**的机器可读列表，各带：

- **id**（数字协议编号）与 **name**；
- **request_type / response_type** 及其 **request_fields / response_fields**（每个字段的编号/tag、名称与类型，带 `*` 数组与 `[key]` map 标记）；
- **direction** — `client-request`（有 `PacketHandler`）、`internal-state`（有 `StatePacketHandler`）、`server-push`（无请求类型）、`server-response`（请求+响应、本服务端角色无处理器）、或 `undefined`；
- **client_handlers / state_handlers** — 每个世界为协议注册的处理器函数（`{World: "GuildApi.applyGuild", ...}`）。

还携带**错误码表**（`game/Common/EC.lua` → 约 197 条 `EC.NAME = number`）。

## 方法

- Schema：从 `game-fogs/game/proto/sproto.spb` 解码（见[协议契约恢复](./protocol-contract-recovery.md)）。
- 处理器：`game/{Hegel,World,CrossWorld}/Rpc/PacketHandler.lua` 中 `PacketHandler[Protocol.X] = fn`（客户端请求），`game/{Hegel,World,CrossWorld,Cache,Pay}/Rpc/StatePacketHandler.lua` 中 `StatePacketHandler[Protocol.X] = fn`（内部状态消息）；`Protocol.X` 经 `Protocol.lua` 解析为数字 id。
- 错误码：`game/Common/EC.lua`。

## 发现

- **方向分布：** 361 个 `client-request`、190 个 `internal-state`、69 个 `server-push`、61 个 `server-response`、8 个 `undefined`；526 个协议至少有一个已注册处理器。
- **未解析处理器引用（4 个）：** `ANA_DoBattleFromHegel`、`ANA_GetNextInfoFromHegel`、`ANA_SycPlayerList`、`ANA_UploadPlayer` 被注册为处理器，但**既不在** `Protocol.lua` **也不在**编译 schema 中——它们引用了本 schema 无线上定义的协议（死引用或已改名）。
- **冲突错误码键：** `EC.lua` 把 `CM_NOGOLD` 与 `LG_INVALID_AID` **定义了两次且值不同**（如 `LG_INVALID_AID = 10000205` 与 `LG_INVALID_AID = 90006`）；清单保留最后定义并标记此问题。
- **无版本兼容规则：** game-fogs Lua 源码中未找到协议/客户端版本门控。

## 授权模型（观察）

`HegelService:handleClientMsg` 按协议 id 解析处理器（`ph[h.type]`），再按 `h.aid` 查找角色；仅当角色存在才调用处理器。逐处理器权限检查**未集中**——各处理器自行检查，故逐消息授权仍是逐项审计（见[玩家、场景与跨服模型](./player-scene-model.md#协议、方法定义与抽象前置条件)）。此处记录的是观察到的身份模型，不是已完成的权限审计。
