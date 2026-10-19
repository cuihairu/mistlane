# BigWorld-like protocol system: evolution notes

Status: research note, checked against repository documents on 2026-10-19. This extends the inventory and gaps in [Protocol analysis](./protocol-analysis.md); it does not repeat that audit. “BigWorld-like” means a coordinated set of contracts, codecs, transport, routing, entity replication and generated client bindings. It does not mean wire compatibility with BigWorld or KBEngine.

The external references below are research material, not existing Mistlane conclusions. Links were checked on 2026-10-19. BigWorld material is historical documentation; KBEngine is used as an open implementation/reference, not as a claim that every engine has identical behavior.

## Reference system checklist

Seven independently checkable parts make up the comparison. Together they describe an integrated protocol system rather than a serialization library.

| # | Part | BigWorld / KBEngine reference evidence | What the part covers |
|---|---|---|---|
| 1 | Entity contract definitions | [KBEngine entity definition](https://kbengine.github.io/docs/programming/entitydef.html) documents `entities.xml`, per-entity `.def`, properties, method sides and property scopes. Historical [BigWorld Server Programming Guide, entity definitions](https://howarduong.github.io/github.io/doc/html/server_programming_guide/ch02.html) describes `.def` as the script communication interface. | Entity type registry; properties, persistence/sync scope, callable methods, execution side and stable identifiers. |
| 2 | Engine message catalogue and codec | [KBEngine communication protocol format](https://kbengine.github.io/docs/programming/kbe_message_format.html) and [client SDK programming](https://kbengine.github.io/docs/programming/clientsdkprogramming.html) describe engine/client message definitions and imported message/entity definitions. [KBEngine `messages.xml` sample](https://github.com/kbengine/kbengine/blob/master/kbe/res/server/messages.xml) is a source-level catalogue. | Engine control messages and game-defined entity messages, directions, IDs, payload grammar, errors and serialization rules. The entity contract and engine message catalogue are related but distinct surfaces. |
| 3 | Network layer and packet framing | [KBEngine communication protocol format](https://kbengine.github.io/docs/programming/kbe_message_format.html) describes the engine protocol framing/message representation; [server configuration](https://kbengine.github.io/docs/configuration/kbengine_defaults.html) exposes packet length behavior. | Connection establishment, packet boundaries, message dispatch, size limits, reconnect and transport adapters. TCP/WebSocket/KCP support is implementation/plugin-specific; the cited protocol docs do not make all transports universal. |
| 4 | Application components and forwarding path | [KBEngine server layout](https://kbengine.github.io/docs/concepts/layout.html), [startup/shutdown](https://kbengine.github.io/docs/startup_shutdown.html), and [client SDK programming](https://kbengine.github.io/docs/programming/clientsdkprogramming.html) show Loginapp/Baseapp/Cellapp roles and client message/import flows. | LoginApp admission and handoff, Gate/BaseApp client leg, BaseApp/CellApp entity calls, component routing and internal forwarding. This is the application-level path that carries protocol messages. |
| 5 | Entity visibility and observer synchronization | [KBEngine entity definitions](https://kbengine.github.io/docs/programming/entitydef.html) specify property scopes; [server configuration](https://kbengine.github.io/docs/configuration/kbengine_defaults.html) documents View radius and witness behavior. | Observer selection/AOI, visibility lifecycle, property/method replication and bandwidth policy. A codec alone does not determine recipients or when state is sent. |
| 6 | Cross-server and world/space management | [KBEngine concepts and server layout](https://kbengine.github.io/docs/concepts/serverlayout.html) describe distributed server components; [entity definition](https://kbengine.github.io/docs/programming/entitydef.html) describes entity parts and migration-relevant data. | Spaces/cells, placement, entity migration/ghosting, ownership, transfer ordering and recovery across processes or machines. These semantics depend on runtime coordination, not just message schemas. |
| 7 | Client SDK and contract distribution | [KBEngine client SDK programming](https://kbengine.github.io/docs/programming/clientsdkprogramming.html) documents client plugin integration and importing engine/entity protocol definitions; the [UE4 demo](https://github.com/kbengine/kbengine_ue4_demo) points to generating plugins from demo assets. | Generated/imported message IDs, entity types, typed proxies, dispatch and client codec bindings kept consistent with server definitions. |

## Legacy game-fogs to Mistlane

The legacy client path is evidenced in [player and scene model](./player-scene-model.md#protocol-and-method-contracts-before-abstraction): `game/Data/Data.lua` loads binary `game/proto/sproto.spb`; `game/lib/sp.lua` encodes/decodes; `game/proto/Protocol.lua` maps names to IDs; `PacketHandler` registrations expose client handlers. The original `.sproto` text source and reproducible generation path were not located. This section maps that legacy starting point to Mistlane, rather than treating its generated binary as a source contract.

| BigWorld-like part / legacy evidence | Mistlane evidence and current position | Classification |
|---|---|---|
| Entity definitions; KBEngine `.def`/XML and game-fogs unrecovered sproto schema | XML entity/type contract validates IDs, emits project XSD/release lock and deterministic `.sproto` schema. It has no runtime wiring. [Entity contract](./entity-contract.md#one-contract) | **Evolved** — an explicit source-of-truth entity contract exists; it is not a recovered legacy schema. |
| Message catalogue and binary codec; game-fogs `.spb` + `sp.lua` | sproto schema generation and fixture golden vectors exist; there is no standard sproto runtime/codec integration. [Generated artifacts](./entity-contract.md#generated-artifacts), [legacy path](./player-scene-model.md#protocol-and-method-contracts-before-abstraction) | **Evolving** — schema generation exists; runtime execution does not. |
| Packet framing/network adapters | Mistlane has TCP length framing, WS payload mapping, HTTP listeners, and separately specified MLC2/MLS1/MLM1 controls. These are transport/runtime contracts, not sproto RPC integration. [Protocol analysis](./protocol-analysis.md#protocol-surface-inventory), [architecture](./architecture.md#execution-and-connection-model) | **Evolving** — generic transport exists; codec dispatch, negotiation and client protocol path do not. |
| Login/Gate/Base/Cell message forwarding; legacy Gate route and handler dispatch | Mistlane has gateway, login/data roles, MLR1 registration/placement/routes and crossServer forwarding; no full entity invocation pipeline or equivalent Base/Cell split. [Process roles](./process-roles.md#current-implementation), [architecture comparison](./architecture-comparison.md) | **Evolving** — service routing exists, but game-fogs handler semantics and full entity calls are not reproduced. |
| Observer/AOI property synchronization; legacy `SC_CharDataUpdate=4005` and manual dirty behavior | Entity property replication has a design, but no Avatar runtime, recipient set, change tracker or sproto notifications. [Entity contract](./entity-contract.md#automatic-property-replication), [legacy path](./player-scene-model.md#protocol-and-method-contracts-before-abstraction) | **Evolving** — target semantics are documented; runtime is absent. |
| Cross-server/world ownership and migration | MLC1 static affinity/admission, MLR1 directory and MLD1 partial return endpoint exist; ownership lease and entity transfer state machine do not. [Cross-server](./cross-server.md), [cross return](./cross-return-protocol.md) | **Evolving** — partial transport/control/data pieces exist; complete placement, migration and ownership protocol do not. |
| Client SDK code generation and synchronized message/entity bindings | JavaScript/C# SDKs are proposed bundle outputs, but no generator or runnable client binding exists. The legacy generated `Protocol.lua` is not a reconstructable source. [Generated artifacts](./entity-contract.md#generated-artifacts), ledger gaps [88–91](./avatar-runtime.md#deliberately-not-decided-here) | **Not evolved** — no generated SDK release path. |
| Compatibility release pipeline for server/client definitions | The prototype has explicit IDs and release lock; protocol byte vectors, schema compatibility diff and client/runtime conformance are open. [Protocol analysis, gap 7](./protocol-analysis.md#-gaps) | **Evolving** — some contract checks exist, no interoperable protocol release gate. |

Counts: **evolved 1, evolving 6, not evolved 1**. “Evolved” means a Mistlane artifact already performs the analogous contract role. It does not claim legacy behavior or wire compatibility. The remaining details and contradictions in the earlier cross-document audit remain tracked in [protocol-analysis](./protocol-analysis.md).

## sproto implementation route

Decision: use sproto for entity/RPC message encoding. XML `.def` and type files remain the handwritten contract; sproto is generated output. This follows [entity-contract.md](./entity-contract.md#design-choice), where the Server bundle includes sproto schema, Lua bindings and digests as planned artifact contents. The current build now emits validated deterministic schema alongside XSD, release lock and digest; it does not bind Lua, run entity calls or build an SDK.

| Stage | Concrete work | Acceptance evidence shape |
|---|---|---|
| 1. XML contract to sproto schema | Keep `.def` as source; run the existing semantic checker before emission. Preserve provenance and mark original game-fogs sproto text unrecovered. Map types, fields, methods and request/response or one-way notifications; use stable `E{entityId}_{Server|Client}_{methodId}` names. Protocol tags are `entityId * 131072 + side * 65536 + methodId`; accepted ranges are entity ID ≤16383 and method ID ≤65535. | `contract.tool` verifies mappings, stable naming and repeatable output; the CLI validates first and emits XSD, lock, digest and schema only after every input passes. |
| 2. Codec boundary and golden vectors | Keep outer transport framing separate. Lock scalar, array, nested and boundary values in fixture-only canonical TLV vectors with round-trip checks. | `sproto.golden` checks exact fixture bytes and decoding. These are **not upstream sproto wire bytes**; replacing this adapter with standard sproto is a later seam. |
| 3. Lua prototype and dispatch | Generate/bind Lua method descriptors, resolve entity/caller context and dispatch request/response or one-way methods. | Runnable Lua prototype covers a server and client method with typed results and rejection cases. |
| 4. RPC harness and lifecycle | Define correlation, timeout/cancel, duplicate handling, response ownership and stale entity-generation behavior. | In-process and two-process harness covers request/response, push, timeout, stale reference, wrong caller, disconnect and duplicate handling. |
| 5. SDK compatibility | Generate JS/C# bindings from the shared schema and compare supported client versions. | JS/C# encode/decode standard golden vectors; compatibility checks detect removed, renumbered or retyped published IDs. |
| 6. Release lock and publication gate | Persist protocol tag assignments and schema digest in the release lock; compare compatibility with a previously published bundle. | Reproducible bundle and compatibility report block breaking changes before publication. |

These are research gates, not dates or a product commitment. Phase boundaries are intentionally evidence-based: no later stage should be described as complete based solely on generated files existing.

## Implemented status and evidence

The first two stages are **partially implemented**. `mistlane contract build` runs the existing XML semantic validation first, then emits XSD, lock, digest and deterministic `.sproto`. `examples/contracts/avatar.def` is a newly constructed minimal fixture, not recovered game-fogs source. RPC names follow `E{entityId}_{Server|Client}_{methodId}`. `contract.tool` checks output stability and naming; `sproto.golden` checks scalar, array, nested and boundary fixture vectors byte-for-byte. The fixture encoder is a small project test format, not sproto serialization; no third-party sproto code was added.

Stages 3–6 are not implemented: Lua prototype/dispatch, RPC lifecycle harness, SDK compatibility, and release-lock publication gate. Upstream parser validation and standard sproto wire vectors remain open within stage 2.

## Scope and exclusions

This is a notebook entry in the evolution of the older `game-fogs/sproto` project. It records a possible route from the extant client sproto binary and Lua codec path toward an explicit, generated, cross-runtime protocol system. It does not claim implementation completeness, promise interoperability with BigWorld/KBEngine, or define a commercial delivery schedule. Productization, hosted service design and cloud backup are outside this note.

## External references

All external references in this note were checked on **2026-10-19** and are references, not prior Mistlane conclusions:

- [KBEngine documentation index](https://kbengine.github.io/docs/) — official project docs index for entity, transport, server and client SDK sections.
- [KBEngine entity definition](https://kbengine.github.io/docs/programming/entitydef.html) — `.def`, entity registry, methods and property scopes.
- [KBEngine communication protocol format](https://kbengine.github.io/docs/programming/kbe_message_format.html) — packet/message protocol reference.
- [KBEngine server layout](https://kbengine.github.io/docs/concepts/layout.html) — distributed component roles.
- [KBEngine client SDK programming](https://kbengine.github.io/docs/programming/clientsdkprogramming.html) — client protocol imports and entity definitions.
- [KBEngine server configuration](https://kbengine.github.io/docs/configuration/kbengine_defaults.html) — packet length behavior, View radius and witness options.
- [KBEngine demo assets Avatar.def](https://github.com/kbengine/kbengine_demos_assets/blob/master/scripts/entity_defs/Avatar.def) — concrete entity definition example.
- [KBEngine UE4 demo](https://github.com/kbengine/kbengine_ue4_demo) — client integration and SDK generation reference.
- [Historical BigWorld Server Programming Guide](https://howarduong.github.io/github.io/doc/html/server_programming_guide/ch02.html) — `.def` and `entities.xml` background; historical third-party mirror.
- [sproto schema and RPC](https://github.com/cloudwu/sproto#rpc-api) — upstream schema, RPC request/response, parser and codec reference.
