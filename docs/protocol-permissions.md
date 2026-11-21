# Protocol per-message authorization audit

Status: audited on 2026-11-22 against the recovered game-fogs protocol
(`contracts/fogs-proto-inventory.json`, game-fogs commit `37b640a`, no drift in the
handler / error-code files to HEAD). This is evidence for the protocol contract (todo
item 73); it audits the **legacy game-fogs** contract that was recovered, not the
mistlane engine. Cross-language page: [简体中文](./zh/protocol-permissions.md);
machine-readable registry: `contracts/fogs-permission-audit.json` (reproducer:
`tools/sproto_permission_audit.py`).

## Dispatch model

Client requests are dispatched by the gateway and two services, and the two services use
**different** conventions:

```
gateway  game/Common/CService/Server/GateService.lua:244 onlineMsg
         local c = Client.getUser(cid, h.type)   -- authenticated session user
         if not c then return end                -- no user => drop
         local nmsg = sp.forward(h.type, context, c.aid, h.tag)  -- h.aid = c.aid (server-set)
         type>2000..4000 -> C_callWorld(nmsg) else -> C_callHegel(c.hid, nmsg)

Hegel    game/Common/CService/Server/HegelService.lua:242 handleClientMsg
         gate: CharactorManager.getObjByAid(h.aid) must return a character, else the
               handler is not called; then ph[h.type](h, p, c)        -- handler gets c

World    game/Common/CService/Server/WorldService.lua:370 onGwMsg, `if not h.cid` branch
         ph[h.type](gid, h.aid, p)                                    -- handler gets aid
         no in-dispatch character-existence gate; the handler must gate on aid itself

CrossWorld / Pay / Cache register no client dispatch path (CrossWorld has one orphan
registration, SyncCrossHegelConn; its service never reads PacketHandler).
```

**The security-relevant fact:** the gateway rewrites `h.aid` from the authenticated
connection (`sp.forward(..., c.aid, ...)`), so the dispatch identity is trustworthy. The
payload `p` is forwarded **unchanged** and is entirely client-controlled. Consequently any
operation whose subject is read from the payload (`p.aid`, `p.tarAid`, …) is protected
**only** by whatever the handler itself checks — there is no central ownership gate for
payload-provided subjects. This is precisely the non-centralized model the ledger records
(`todo.md` item 73): the global gate covers the dispatch identity, and every
payload-scoped operation depends on a per-handler check.

The handler functions are thin API wrappers (`Module.func`) that forward to a Logic layer
(`ModuleLogic.func`) where the real body and any authorization live; the audit scans both.

## Registry

`contracts/fogs-permission-audit.json` resolves every client-request registration
(`PacketHandler[Protocol.X] = Module.func`, 361 messages) to its defining function and
records its signature, character context, guard patterns, and first Logic call:

- **361** client-request messages, **361** registrations, **361** resolved, **0** unresolved.
- character context: **252** `c` (Hegel), **107** `aid` (World/CrossWorld), **2** `h`; **0**
  handlers have no character context at all.
- guard patterns found: **95** handlers; **266** with no detected guard (dispatch-scoped
  operations are safe by construction: the subject is the server-set identity).
- **26** handlers reference a payload-provided subject (`p.aid` / `p.tarAid` / …);
  **24** of those have **no** detected ownership guard (see candidates below).

## Payload-scoped candidates (24, no detected ownership guard)

The candidates are not homogeneous; the sample verdicts below show three classes. The
mechanical classification is evidence for review, not a verdict:

- **cross-character-by-design** (target is another player, eligibility-checked): marriage
  (`MarriageLogic.propose/acceptProposal/oneKeyPropose`), mentor
  (`MentorLogic.mentorInvite/promise/transferExp/acceptExp/release`). The target is the
  subject by design; the checks validate the *target's* state, not ownership — legitimate.
- **identity-checked**: `BattleBossLogic.assistBoss` compares `c.data.aid == p.tarAid`
  (rejects self-assist) before forwarding — an explicit dispatch-vs-payload identity check.
- **requires-verification**: the World single-mail path
  (`MailAPI.extractMail(gw,aid,p)` → `MailLogic.extractMail(aid, p.aid)`,
  `MailAPI.getOneMail(gw,aid,p)` → `MailLogic.getOneMail(aid, p.aid)`) forwards the
  client-controlled `p.aid` as the mail owner. `extractMail`/`getOneMail` are **not defined
  in the World MailLogic module** (`game/World/Mail/MailLogic.lua` has no such functions),
  so the single-mail path is an undefined-call risk; if the pair ever resolves, the
  ownership of `p.aid` must be checked against the server-set `aid`.

The remaining candidates (room join/start, first-budokai video, character detail,
`C_DoMoBai`, `GB_SHOW_BOOKDETAIL`, `SC_StartBattle`) also take a payload subject and were
not individually verified here; each should be resolved to its Logic body and checked for
an `aid`/ownership comparison against the server-set identity before the recovered
contract is relied on.

## Anomalies

- **CrossWorld orphan registration**: `PacketHandler[Protocol.SyncCrossHegelConn]`
  registered in `game/CrossWorld/Rpc/PacketHandler.lua` but `CrossWorldService` never
  dispatches `PacketHandler` — the registration is dead.
- **0 handlers without a character context**: every resolved handler receives `c`, `aid`,
  or `h`, so no client request can act without a server-supplied identity in scope.

## Caveat

The audit is a mechanical registry plus targeted verification of the sample classes. The
24 payload-scoped candidates each need a Logic-chain ownership check (does the target
compare equal to the server-set identity?) before a verdict is safe; the registry records
the guard patterns found so the review is reproducible.
