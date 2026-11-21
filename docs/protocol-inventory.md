# Protocol inventory

Status: built on 2026-11-21 from the recovered schema ([protocol contract recovery](./protocol-contract-recovery.md)) plus a game-fogs handler-registration and error-code audit. This lists the legacy protocol's wire messages with their numbers, field layouts, direction, handler and error codes; per-handler authorization and version-compatibility rules are recorded as pending/absent. Cross-language page: [简体中文](./zh/protocol-inventory.md); data: `contracts/fogs-proto-inventory.json`; generator: `tools/sproto_inventory.py`.

## What the inventory captures

`contracts/fogs-proto-inventory.json` is a machine-readable listing of **646 protocols**, each with:

- **id** (numeric protocol number) and **name**;
- **request_type / response_type** and their **request_fields / response_fields** (each field's number/tag, name and type, with `*` array and `[key]` map markers);
- **direction** — `client-request` (has a `PacketHandler`), `internal-state` (has a `StatePacketHandler`), `server-push` (no request type), `server-response` (request+response, no handler in this server role), or `undefined`;
- **client_handlers / state_handlers** — the handler function each world registers per protocol (`{World: "GuildApi.applyGuild", ...}`).

It also carries the **error-code table** (`game/Common/EC.lua` → ~197 `EC.NAME = number` entries).

## Method

- Schema: decoded from `game-fogs/game/proto/sproto.spb` (see [protocol contract recovery](./protocol-contract-recovery.md)).
- Handlers: `PacketHandler[Protocol.X] = fn` in `game/{Hegel,World,CrossWorld}/Rpc/PacketHandler.lua` (client requests) and `StatePacketHandler[Protocol.X] = fn` in `game/{Hegel,World,CrossWorld,Cache,Pay}/Rpc/StatePacketHandler.lua` (internal state messages); `Protocol.X` resolves to the numeric id via `Protocol.lua`.
- Error codes: `game/Common/EC.lua`.

## Findings

- **Direction distribution:** 361 `client-request`, 190 `internal-state`, 69 `server-push`, 61 `server-response`, 8 `undefined`; 526 protocols have at least one registered handler.
- **Unresolved handler references (4):** `ANA_DoBattleFromHegel`, `ANA_GetNextInfoFromHegel`, `ANA_SycPlayerList`, `ANA_UploadPlayer` are registered as handlers but appear in **neither** `Protocol.lua` nor the compiled schema — they reference protocols with no wire definition in this schema (dead or renamed refs).
- **Conflicting error-code keys:** `EC.lua` defines `CM_NOGOLD` and `LG_INVALID_AID` **twice with different values** (e.g. `LG_INVALID_AID = 10000205` and `LG_INVALID_AID = 90006`); the inventory keeps the last definition and flags this.
- **No version-compatibility rules:** no protocol/client version gating was found in the game-fogs Lua sources.

## Authorization model (observed)

`HegelService:handleClientMsg` resolves the handler by protocol id (`ph[h.type]`), then looks up the character by `h.aid`; the handler is called only if the character exists. Per-handler permission checks are **not centralized** — each handler applies its own checks, so per-message authorization remains an entry-by-entry audit (see [players, scenes and transfers](./player-scene-model.md#protocol-and-method-contracts-before-abstraction)). This is recorded as the observed identity model, not as a completed permission audit.
