# Protocol analysis

Status: cross-document audit against the design and implementation notes present on 2026-10-19. This continues the entity, service, cross-return and validation analyses; it does not restate their designs or imply that a documented contract is wired. The inventory distinguishes wire behavior, decided design and missing runtime work.

## Protocol surface inventory

| Layer | Surface and boundary | Source |
|---|---|---|
| Transport and listeners | TCP uses a four-byte big-endian length and up to 1 MiB payload; WS messages map to one payload. HTTP/HTTPS carries request/response; WS/WSS listeners and upgrades have separate game-session or Lua HTTP lifecycle routing. Multiple listeners are configured per process. | [architecture.md §Execution and connection model](./architecture.md#execution-and-connection-model), [http.md §Server, §Multiple listeners](./http.md#server) |
| Transport controls | Optional `MLC2` adds a u32 BE request ID inside each payload (reply echoes it, push uses 0); optional `MLS1` authenticates internal service legs; optional `MLM1` multiplexes logical sessions over an authenticated upstream. These are distinct layers; `MLC1` cross routing excludes MLC2/MLM1 modes. | [architecture.md lines 66–70](./architecture.md#execution-and-connection-model), [validation.md service identity/multiplex rows](./validation.md) |
| Routing and cross-server | `MLC1` is the gateway cross-routing/admission handshake, with optional all-or-none participant suffix (`ch`, `gen`, `ret`). It selects static room/group affinity and admission; it is not dynamic character placement or transfer. `PROBE1` supplies readiness/overload checks. | [cross-server.md](./cross-server.md), [cross-return-protocol.md §Participant and return context](./cross-return-protocol.md#participant-and-return-context), [architecture-comparison.md lines 53–60](./architecture-comparison.md) |
| Coordinator control | `MLR1` member registration, heartbeat, QUERY/PLACE and ROUTE SET/GET operate in a named scope. Registration carries readiness/load; current route records point to live registered members. No ownership lease or migration state machine is implemented. | [process-roles.md §Current implementation](./process-roles.md#current-implementation), todo.md:48–51 |
| Return and data | `MLD1 LOAD/SUBMIT` is a framed-TCP endpoint on `data`; it fences generations and enforces consecutive per-`(realm, character)` sequence plus duplicate receipt. `RECEIVED` is volatile receipt only; no durable ACK or storage chain exists. A per-participant sender queue boundary serializes contiguous submissions but is not wired to a crossServer runtime. | [cross-return-protocol.md §Return and persistence confirmation](./cross-return-protocol.md#return-and-persistence-confirmation-mld1), [service-contracts.md §data](./service-contracts.md#data), [validation.md MLD1 row](./validation.md) |
| Entity contract and synchronization | XML `.def` prototype fixes a subset of types, explicit IDs, generated project XSD and release lock. The designed invocation envelope is a framed payload with `EntityRef` addressing, independent correlation ID, generation/deadline and closed `EntityError`; property replication and persistence remain design/runtime work. | [entity-contract.md §One contract](./entity-contract.md#one-contract), [§Invocation envelope and error protocol](./entity-contract.md#invocation-envelope-and-error-protocol-decided-unimplemented), [§Automatic property replication](./entity-contract.md#automatic-property-replication) |
| Service contract | `login` is a client-facing script role and MLR1 placement client; `data` is the home record endpoint; optional `apiServer` is external HTTP ingress. MLS1 is service-leg identity. Login ticket decision is HMAC-SHA256, but issuance/verification is not implemented. | [service-contracts.md](./service-contracts.md), [process-roles.md §Cross-region data ownership](./process-roles.md#cross-region-data-ownership) |
| HTTP and error boundaries | HTTP gateway maps request/upstream outcomes to HTTP statuses; apiServer hooks and WebSocket callbacks have request/session-scoped error behavior. EntityError is a separate closed RPC error vocabulary. MLR1/MLD1/MLS1/MLC1 errors are protocol-specific tokens, not one shared error enum. | [http.md §Server and §Lua HTTP handlers](./http.md), [entity-contract.md §Invocation envelope and error protocol](./entity-contract.md#invocation-envelope-and-error-protocol-decided-unimplemented) |

## Consistency audit

Counts below are unresolved findings in this audit, not a count of every repeated wording. Three contradictions and six gaps remain actionable. A stale statement is counted as a contradiction where it asserts the opposite of a later fixed decision; missing encoding/ownership/timing detail is a gap.

### Contradictions

1. **Ticket state:** `cross-return-protocol.md:15,17,77` says there is no issuer yet and entry ticket/signing remains undecided; `service-contracts.md:10,23,26,58` and `todo.md:56,58` decide a login-issued HMAC-SHA256 ticket, while clearly marking its implementation absent. The design decision is settled; the cross-return page is stale.
2. **Whether return protocol is decided:** `architecture.md:21` calls return protocol/ack semantics unimplemented. `cross-return-protocol.md:3,46,74` and `service-contracts.md:7` say the protocol and data endpoint decision are fixed, while implementation is partial. This conflates undecided with unimplemented.
3. **Crash/restart dedup durability:** `cross-return-protocol.md:65` promises dedup after sender restart, but `cross-return-protocol.md:58`, `service-contracts.md:41`, and `todo.md:55` say current data records and dedup state are process-local and a data restart returns EMPTY. The restart claim can only hold after durable storage and durable sequence state exist.

### Gaps

1. **Entry context completeness:** the cross-return shape includes `generation`; `service-contracts.md:22` lists only origin realm, character and return route in login's entry response. It does not state where initial generation is issued, returned or advanced.
2. **Durable response grammar — resolved for format, partial for storage:** MLD1 now defines `ACK durable <realm> <character> gen <n> seq <m>` and sender validation; the endpoint still cannot emit it until durable storage exists. Lost ACK replay is the same idempotent key/cursor.
3. **Ownership enforcement vs endpoint checks — partial:** handoff order, late-source fencing, in-flight request and timer treatment are defined and represented by a tested lease state model. Coordinator/data/Avatar runtime ownership and persistent tombstone enforcement are not wired.
4. **Coordinator route vs ownership:** MLR1 ROUTE SET/GET records a live host (`architecture.md:70`, `todo.md:51`); coordinator role contract also names ownership/transfer states (`process-roles.md:11`). The relationship between ephemeral route record, durable home owner and transfer state transitions is not encoded.
5. **API callback acceptance:** `service-contracts.md:52–54` says trusted-network today and callback auth/idempotency remain open; no callback signature, replay window, durable acceptance response or ownership lookup message is specified. HTTP response lifecycle does not define business durability.
6. **Entity rollout gate:** XSD, ID/reserved lock and envelope/error decisions exist, but `todo.md:76–90` still has compatibility diff, EntityRef runtime semantics, compiler/SDK generation, nested sync and data persistence open. The XML prototype produces XSD/lock artifacts but no release-compatibility procedure, while the error envelope has no wire encoding/golden bytes or runtime binding. This leaves no interoperable artifact gate between format approval and runtime adoption.

### Resolved in todo:54 first slice

- **Sequence ordering (former contradiction 1):** the sender queue emits only contiguous sequences serially per participant, matching the endpoint's `stored + 1` check. `RECEIVED` releases the send slot but retains the change until durable acknowledgement.
- **Realm key (former gap 1):** endpoint fence/dedup state and sender queue use `(realm, character)`. Existing endpoint state is volatile, so no live migration is required; future persisted character-only keys need realm backfill or collision resolution.

### todo:54 second slice — gaps 3/4

- **Durable ACK grammar:** resolved at wire/codec/sender boundary; partial end-to-end because data storage does not exist and the endpoint never emits a false durable claim.
- **Lease handoff timing:** resolved as an ordered design and deterministic model; partial runtime enforcement because coordinator, durable home owner and Avatar lifecycle are not connected.

### Naming and semantics aligned (with boundaries)

- The invocation key uses `scope/type_id/entity_id/incarnation`; `EntityRef` uses that same address tuple. `hosting_generation` is an ownership fence, not entity incarnation (`entity-contract.md:243–254`).
- `realm` / “origin realm” / “home region or group” refer to the data ownership domain; `scope` is coordinator placement scope. They must not be treated as interchangeable (`cross-return-protocol.md:11`, `architecture.md:70`).
- `RECEIVED` means endpoint accepted into volatile process memory; `ACK durable` means home data committed durable storage. They are explicitly different (`cross-return-protocol.md:58`, `service-contracts.md:36–41`).
- Transport correlation (`MLC2`) and entity `correlation_id` are intentionally independent; protocol errors remain surface-specific rather than sharing EntityError codes (`entity-contract.md:237–275`).

## Open decisions by todo item

### todo.md:54 — crossServer return and single writer

| Blocker | Options | Recommendation |
|---|---|---|
| Sequence gaps vs out-of-order conflict; endpoint currently requires contiguous `stored+1`. | A. Keep strict in-order delivery and buffer/retry gaps at sender. B. Let data persist a bounded reorder window. | **A (implemented in this slice).** The queue serializes submissions and waits/retries gaps; the endpoint remains authoritative. |
| Character key omits realm in dedup description. | A. Key by character only and require globally unique IDs. B. Key by `(realm, character)` throughout. | **B (implemented in endpoint and queue).** Existing contract only guarantees realm-local uniqueness and MLD1 already carries realm. |
| Sender-crash recovery promise assumes data dedup survives restart, but current MLD1 is in-memory. | A. Restrict claim until storage exists. B. Add durable inbox/sequence table in same transaction as state. | **B (recommended; implementation gate).** Persist latest accepted state and sequence atomically before ACK durable; until then no sender can treat RECEIVED as recovery evidence. |
| Handoff/lease has no concrete request/reply sequence. | A. Coordinator-issued generation and data-side compare-and-swap takeover. B. Let timeout/heartbeat infer a new owner. | **A (decided here).** Home data issues the next generation and tombstones the prior one; coordinator only publishes route after source fence, destination load and acknowledgement. |
| Sender retry queue, read-only threshold and final return transition are not wired. | A. Per-character bounded queue with explicit read-only and final sequence barrier. B. Drop oldest or acknowledge receipt as completion. | **A (recommended).** Never drop or mistake receipt for durability; forced route change remains failed return. |

### todo.md:56 — login/data/apiServer contracts and wiring

| Blocker | Options | Recommendation |
|---|---|---|
| Login ticket decision is not reflected by cross-return status; credential primitives are unwired. | A. HMAC-SHA256 short-lived ticket issued by login with per-realm secret. B. mTLS/PKI or external JWT service. | **A (already decided; implementation pending).** Align cross-return page and make context invalid until ticket verifies. |
| Service contract entry context omits hosting generation and lacks issuance lifecycle. | A. Login asks data to reserve/advance generation before placement. B. Host invents generation at entry. | **A (recommended).** Data is sole generation authority; ticket binds realm, character, target instance, generation, expiry and nonce. |
| MLD1 has no durable-ack response grammar or storage transaction boundary. | A. ACK after durable state and sequence commit together; B. ACK on receipt. | **A (already decided).** Define a stable `ACK durable gen <n> seq <m>` response after atomic commit; keep `RECEIVED` explicitly non-terminal. |
| apiServer callback authentication/idempotency and durable response are open. | A. Application handler owns all three without framework contract. B. Specify signed callback verification, idempotency key and durable acceptance boundary. | **B (recommended).** Keep payment rules application-owned, but require verified signature + durable dedup before success response. |

### todo.md:48 and todo.md:76+ — coordinator and entity phases

| Blocker | Options | Recommendation |
|---|---|---|
| MLR1 routing has no owner/scene state transition or transfer protocol. | A. Extend MLR1 registry into gameplay state authority. B. Keep MLR1 as placement/route directory; define generation-fenced transfer in home data plus explicit coordinator state machine. | **B (recommended).** Keep coordinator control-plane only; define reserve → prepare → commit/abort and route publication order before migration code. |
| Entity contract phases do not define one release gate from XML to runtime. | A. Start runtime from current prototype. B. Freeze schema and error bytes, compatibility diff and golden vectors before runtime. | **B (recommended).** Complete format review → minimal codec/XSD/lock compatibility prototype → runtime; then add EntityRef behavior, nested replication and persistence behind separate gates. |
| Entity envelope/error design has semantic fields but no serialized golden representation; `EntityRef` is only a declared type. | A. Leave encoding to each runtime. B. Define canonical field encoding and error test vectors before C++/Lua/JS/C# bindings. | **B (recommended).** Preserve the already-decided field order and closed error set, publish vectors and reject unknown/malformed forms before runtime rollout. |

`apiServer` external request data is application-owned; any future data export or cloud backup is off by default.
