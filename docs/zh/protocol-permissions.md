# 协议逐消息权限审计

状态：2026-11-22 对恢复的 game-fogs 协议（`contracts/fogs-proto-inventory.json`，game-fogs commit `37b640a`，handler/错误码文件至 HEAD 零漂移）审计。这是协议契约的证据（todo 第 73 项）；审计对象是**恢复出的旧 game-fogs 契约**，而非 mistlane 引擎。英文页：[English](../protocol-permissions.md)；机器可读登记册：`contracts/fogs-permission-audit.json`（复现工具：`tools/sproto_permission_audit.py`）。

## 分发模型

客户端请求由网关与两个服务分发，且两个服务的**约定不同**：

```
gateway  game/Common/CService/Server/GateService.lua:244 onlineMsg
         local c = Client.getUser(cid, h.type)   -- 认证会话用户
         if not c then return end                -- 无用户则丢弃
         local nmsg = sp.forward(h.type, context, c.aid, h.tag)  -- h.aid = c.aid（服务端写入）
         type>2000..4000 -> C_callWorld(nmsg) 否则 -> C_callHegel(c.hid, nmsg)

Hegel    game/Common/CService/Server/HegelService.lua:242 handleClientMsg
         闸：CharactorManager.getObjByAid(h.aid) 必须返回角色，否则不调用 handler；
              然后 ph[h.type](h, p, c)                              -- handler 拿到 c

World    game/Common/CService/Server/WorldService.lua:370 onGwMsg，`if not h.cid` 分支
         ph[h.type](gid, h.aid, p)                                  -- handler 拿到 aid
         分发内无角色存在性闸；handler 须自行对 aid 设闸

CrossWorld / Pay / Cache 无客户端分发路径（CrossWorld 有一个孤立注册 SyncCrossHegelConn，
其 service 从不读 PacketHandler）。
```

**与安全相关的关键事实**：网关用 `sp.forward(..., c.aid, ...)` 把 `h.aid` 强制改写为连接认证的身份，故分发身份可信；而 payload `p` **原样透传**、完全由客户端控制。因此任何以 payload 字段（`p.aid`、`p.tarAid`…）为操作主体的操作，只受 handler 自身检查保护——没有针对 payload 主体的集中归属闸。这正是账本所记的「非集中式」模型（todo 第 73 项）：全局闸覆盖分发身份，每个 payload 作用域的操作都依赖逐 handler 检查。

handler 函数是薄 API 包装（`Module.func`），转发到 Logic 层（`ModuleLogic.func`），真正的实现与授权检查在 Logic 层；审计扫描两层。

## 登记册

`contracts/fogs-permission-audit.json` 把每个客户端请求注册（`PacketHandler[Protocol.X] = Module.func`，361 条消息）解析到其定义函数，并记录签名、角色上下文、守卫模式与首个 Logic 调用：

- **361** 条 client-request 消息、**361** 条注册、**361** 条解析、**0** 条未解析。
- 角色上下文：**252** 个 `c`（Hegel）、**107** 个 `aid`（World/CrossWorld）、**2** 个 `h`；**0** 个 handler 完全没有角色上下文。
- 检到守卫模式：**95** 个；**266** 个无检出守卫（dispatch 作用域操作按构造安全：主体是服务端写入身份）。
- **26** 个 handler 引用 payload 提供的主体（`p.aid` / `p.tarAid` / …）；其中 **24** 个无检出的 ownership 守卫（见下）。

## Payload 作用域候选（24 个，无检出 ownership 守卫）

候选非同质；抽样结论分三类。机械分类是供复核的证据，不是裁决：

- **跨角色设计**（目标为他人、有资格校验）：婚姻（`MarriageLogic.propose/acceptProposal/oneKeyPropose`）、师徒（`MentorLogic.mentorInvite/promise/transferExp/acceptExp/release`）。目标按设计即主体；校验的是*目标*的状态而非归属——合理。
- **身份校验**：`BattleBossLogic.assistBoss` 在转发前比较 `c.data.aid == p.tarAid`（拒绝助战自己）——显式的分发对 payload 身份检查。
- **需核验**：World 单邮件路径（`MailAPI.extractMail(gw,aid,p)` → `MailLogic.extractMail(aid, p.aid)`、`MailAPI.getOneMail(gw,aid,p)` → `MailLogic.getOneMail(aid, p.aid)`）把客户端可控的 `p.aid` 当作邮件所有者转发。`extractMail`/`getOneMail` **未在 World MailLogic 模块定义**（`game/World/Mail/MailLogic.lua` 无此二函数），故单邮件路径存在未定义调用风险；若该对函数将来解析成功，`p.aid` 的归属必须与服务端写入的 `aid` 比对。

其余候选（房间加入/开始、第一武道会录像、角色详情、`C_DoMoBai`、`GB_SHOW_BOOKDETAIL`、`SC_StartBattle`）同样取 payload 主体，未逐一核验；每个都应在依赖恢复契约前解析到其 Logic 体，检查是否与分发身份（`aid`/`c.data.aid`）有归属比较。

## 异常

- **CrossWorld 孤立注册**：`PacketHandler[Protocol.SyncCrossHegelConn]` 注册于 `game/CrossWorld/Rpc/PacketHandler.lua`，但 `CrossWorldService` 从不分发 `PacketHandler`——注册为死项。
- **0 个 handler 无角色上下文**：每个已解析 handler 都拿到 `c`、`aid` 或 `h`，故没有客户端请求能在作用域内缺少服务端提供的身份。

## 局限

审计是机械登记册加抽样类别的定向核验。24 个 payload 作用域候选在裁决前都需 Logic 链归属检查（目标是否与分发身份相等）；登记册记录已发现的守卫模式，使复核可复现。
