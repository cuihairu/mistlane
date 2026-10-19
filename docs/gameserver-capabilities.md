# gameServer capability assessment

This page inventories the legacy Hegel process — the gameServer equivalent — item by item: process shape, scene templates and instances, timing, synchronization and release. It separates generic mechanics from original gameplay and configuration, and states what each item means for Mistlane. Evidence is quoted from the game-fogs sources; no code or assets are imported. It complements the [model page](./player-scene-model.md) (Session/Avatar, transfer, map data layers) and the [migration ledger](./migration-status.md) (timer and utility rows already decided); it does not re-decide those.

## Process shape

| Observation | Legacy evidence | Generic or gameplay | Mistlane meaning |
|---|---|---|---|
| The C++ shell is a host, not a domain | `d_hegel/ServerMain.cpp`: config file plus `--index` process number, log config, RAII MySQL init, `FogsProcess` over one `io_service`, signal-driven graceful stop | Generic | Mistlane keeps the same split in principle: one process, C++ hosting, Lua domain |
| Only gateways connect in | `d_hegel/HegelServer.cpp::handleAccept/onRegister`: accepted connections register through `LuaCallHandler::registerConn`, land in `gates_` keyed by gateway id, and every later frame is pushed into one Lua VM via `pushRpcMsg` | Generic | Mistlane's current gameServer instead accepts clients directly and talks MLR1 upstream; a gate-only accept model is a deployment shape, not a capability gap |
| One repeating C++ tick drives all Lua time | `HegelServer::startTick/doTick` call `LuaCallHandler::onTick`, which invokes the `L_doTick` global with wall-clock milliseconds; the interval comes from Lua `C_startTimer` (`game/Util/Timer.lua` starts it at 1000 ms by default) | Generic | Mistlane runs a fixed 100 ms `tick()` hook with no argument (see the [ledger](./migration-status.md#legacy-lua-callback-audit)); both are single-driver designs |
| ~20 C API bindings | `d_hegel/CAPI.cpp`: listen, timer, dead-loop monitor, connect server, UDP client, map load/info/unload, pathfinding and coordinate conversions (`generatePath`/`isStraightReachable` registered, `generateHighWay` commented out), MD5, clocks, operation log | Mixed | Mistlane exposes a module table (`mistlane.*`) instead of globals; map/navigation bindings are absent and are not derived from the legacy signatures |

The Lua side carries the entire domain: 60+ gameplay module directories under `game/Hegel/` (battle, guild, pets, quests, shops, arenas and the rest) plus the framework directories (`Scene`, `GameScene`, `Server`, `Rpc`, `Async`, `Util`). The gameplay directories are excluded from migration wholesale.

## Scene templates and instances

| Item | Legacy behavior and evidence | Generic or gameplay | Mistlane boundary |
|---|---|---|---|
| Template to instance | `SceneLogic.newScene` shallow-copies the template row and splits identity: `o.tid = meta.id`, `o.id = tonumber(instId)`; `Scene:new` marks `keep = true` when tid equals id | Generic mechanism, gameplay values | Target model already states definition vs instance; a copy-with-independent-id rule is the transferable part |
| Persistent scenes | `SceneManager.init` creates one instance per template row where `isInitScene` (`type == SCENE_TYPE_INIT`) and marks it `dontClear`; `Scene:clear` refuses to run under that flag and logs a stack trace | Generic mechanism, gameplay values | Persistent vs temporary scenes with different release policies — matches the target contract |
| Temporary clones | `SceneLogic.createCloneScene` allocates dynamic instances from a process-local counter starting at 1,000,000; the source has a `TODO 确保每个hegel生成的mapid唯一` — cross-process uniqueness is unhandled | Generic mechanism, incomplete | Instance ids must be unique per placement scope; the legacy scheme proves nothing beyond one process |
| Instance lookup and creation on demand | `SceneManager.guaranteedGet(sceneId, tid)` falls back to cloning when neither the instance id nor the template id is present | Generic | Entering an existing scene must not clone or reload; on-demand creation needs a real existence check, which the fallback conflates |
| Capacity and idleness | `Scene:init` sets `maxNum` from the template row or `host_config.hegel.hegel_Limit`; `Scene:isIdle` reports `type > 2` or `#aids < maxNum` | Mechanism, gameplay thresholds | The placement query (`MLR1 PLACE`) currently uses the whole process; per-scene capacity stays a gameServer-local decision |
| Excluded | `SceneType` values, `minLv` gates, `gametp`/activity/boss wiring, `SCENEBATTLE` rate tables, `SceneMob` waves/revive/stay-mob payloads, team and camp semantics, achievement/analytics side effects in `changeToReborn` | Gameplay | Not migrated; not re-implemented by another name |

## Timing

The legacy gameServer has exactly one process-wide driver: the C++ tick calls `L_doTick(ms)`, and all Lua scheduling lives in `game/Util/Timer.lua` over `TimerQueue` — the two-tier surface (min-heap plus sub-5-second bucket array, 100 000-task cap, lazy removal, `xx1` sentinel removing a failing repeating task) is already compared in the [migration ledger](./migration-status.md). What the ledger does not record:

- **No scene tick runs.** `SceneManager.startTimer()` contains only the commented-out `--Timer.setInterval(SceneManager.updateState, 100)`; `SceneManager.updateState` has no other caller. The per-scene 100 ms cadence that the code shape implies was never active.
- **The only per-scene timer is position sync.** `Scene:init` starts `Timer.setInterval(self.syncChar, UPDATE_INTERVAL, self)` with `UPDATE_INTERVAL = 1000`, and only when the template sets `isSync`; `Scene:clear` removes it along with the scene's own timer table.
- **Error isolation is per task**, `xpcall` with per-function `Statistics` timing; a repeating task reschedules from the current tick (`t.ts = ms`), so its cadence restarts rather than accumulating drift.

For Mistlane this means scene-level timing (per-scene cadence, timers bound to a scene's life, invalidation on release) is a design item, not an extraction item: the legacy evidence shows the shape but also that the generic form was never exercised end to end.

## Synchronization

| Item | Legacy behavior and evidence | Generic or gameplay | Mistlane boundary |
|---|---|---|---|
| Dirty position batching | `Scene:updatePos` records `syncDirty[aid]`; `Scene:syncChar` (1000 ms) sends every client the other characters' dirty entries and clears the batch | Generic shape | Per-scene interest batching is a target mechanism; nothing in the current runtime implements it |
| Leader-only position reports | `updatePos` ignores reports from non-leader team members and copies the leader's position to followers (`syncPosByLeaveTeam` mirrors it on team change) | Gameplay rule inside a generic gate | Client reports are not server movement simulation; the target must not inherit leader-authority as a physics model |
| Chunked entry payloads | `Scene:enter` splits the visible-character list with `TableUtil.splitArr(chars, 20)` and sends one `SC_Sync` frame per chunk, then mob/wave/self-mob payloads, before the `SC_CharEnter` broadcast | Generic shape, gameplay payload | Bounded initial-state delivery is a target mechanism; the 20-per-frame number is a tuning value, not a contract |
| Template-gated broadcasting | Every broadcast and sync in `Scene` is guarded by `TemplateReader.isNull(self.isSync)` — scene types without the flag stay silent | Gameplay configuration | Opt-in synchronization per scene type is reasonable; the flag's semantics stay with the game |
| Character field notifications | `SC_CharDataUpdate = 4005` through `Charactor.set`, already documented on the [model page](./player-scene-model.md) | — | Property sync is governed by the [entity contract](./entity-contract.md), not by this page |

## Release

The release path is the weakest part of the legacy evidence:

- `Scene:clear` frees membership, removes the scene's timers and respects `dontClear` — this is the only fully implemented piece.
- `Scene:destory` is an empty `-- TODO` (scene-comment: instances die "when all players leave or time runs out"), so instance completion by emptiness or timeout does not exist.
- `SceneLogic.leaveScene(c, clearCurScene)` calls `SceneManager.delete(curScene)` when asked to clear the current scene — `curScene` is not a local in that function (the resolved variable is `ttid`), so the branch deletes nothing. The one caller path that promises cleanup does not clean up.
- `GameSceneIndex.afterClose` shows the activity-shaped pattern instead: send rewards, transfer every remaining player out after a 3 s `Timer.setTimeout`, reset bookkeeping; evacuation is per-template code, not a framework service.

Mistlane has no scene objects to release today (SIGTERM drains sessions, not scenes). The release contract — idle detection, player evacuation, timeout, static-data reference counting — must be designed with the scene model (todo item on map/scene lifecycle); the legacy code cannot serve as the specification.

## What transfers and what does not

**Generic mechanics (candidates, none implemented in Mistlane):** single-driver tick with a bounded, error-isolated task scheduler; template-copy instance creation with independent ids and a persistent flag; per-instance membership with capacity and idle tests; dirty-batching sync on a fixed cadence; bounded chunked initial-state delivery; release that invalidates timers and refuses on persistent instances.

**Excluded (original gameplay and configuration):** the 60+ gameplay module trees and their template tables, scene type semantics, level and achievement gates, team/camp/revive/battle-state rules, mob waves and stay mobs, rank/reward flows, and every XML/config value quoted above (they are evidence, not defaults).

**Not established by the sources:** any of the generic mechanics as a proven contract — the scene tick is commented out, the destructor is a TODO, the cleanup branch is dead code, cross-process instance ids are an open TODO, and active navigation is commented out in `d_hegel/CAPI.cpp` (see the [model page](./player-scene-model.md)).

## Role mapping

- **gameServer** hosts scene instances as generic lifecycle/spatial/sync mechanisms plus Lua gameplay rules — the observed C++/Lua split carries forward; the current Mistlane gameServer provides the Lua execution shell and admission, nothing of the scene model yet.
- **crossServer** shares the same mechanisms with a different template set: the legacy code reuses the same `Scene` classes and gates cross behavior with `ServerUtil.isCrossServer`/`P_SIDX` checks and an `IS_CROSS` table (`GameSceneIndex.setScene`), not a separate runtime.
- **coordinator** gains nothing from this page: a scene's tid/id pair is a process-local object identity, not an ownership record; placement and transfer states are governed by the [responsibility contract](./process-roles.md) and remain unimplemented.

Legacy locations: `d_hegel/ServerMain.cpp`, `d_hegel/HegelServer.cpp::startTick/doTick/onRegister`, `d_hegel/CAPI.cpp` (registration block), `game/HegelMain.lua`, `game/Util/Timer.lua::doTick`, `game/Util/TimerQueue.lua`, `game/Hegel/Scene/SceneManager.lua::init/guaranteedGet/updateState/startTimer/delete`, `game/Hegel/Scene/SceneLogic.lua::newScene/createCloneScene/changeOne/leaveScene/doSwitchHegel`, `game/Hegel/Scene/Scene.lua::new/init/enter/leave/syncChar/updatePos/clear/destory`, `game/Hegel/GameScene/GameSceneIndex.lua::setScene/afterOpen/transferAllPlayer/afterClose`.
