# Source layer analysis

Status: source-derived abstraction recorded on 2026-11-07 at commit `2b57842dd513b3153a7795afbc2af16939609c49`. The six layers below are responsibility boundaries, not directory boundaries. Cross-language pages: [简体中文](./zh/source-layer-analysis.md); term table: [Glossary](./glossary.md).

## Layer model

| Layer | Responsibility | Representative implementation | State |
|---|---|---|---|
| L1 Process and role layer | Parse one process configuration, select one role, validate role-specific combinations, and dispatch to node, bot, contract or test commands. | `src/main.cpp:195`, `src/main.cpp:336-448`, `include/mistlane/roles.hpp:6-16` | None |
| L2 Transport and session layer | Terminate TCP/WebSocket/HTTP listeners, frame payloads, authenticate service links, multiplex upstream links, enforce overload controls, and forward session payloads. | `src/node.cpp:226-295`, `src/web.cpp:16-113`, `src/node.cpp:1146-1860` | Per-process memory |
| L3 Coordination and registry layer | Track scoped members and routes, answer placement queries, and run the typed migration transaction. | `src/node.cpp:2214-2793`, `src/node.cpp:2795-3010`, `include/mistlane/migration.hpp:72-364` | Memory; optional SQLite journal |
| L4 Service execution layer | Host one sandboxed Lua VM per script role, expose business APIs and callbacks, and connect Redis and outbound HTTP. | `include/mistlane/lua_service.hpp:76-105`, `src/lua_service.cpp:136-1063`, `src/http_client.cpp:23-58` | Redis when configured; process memory |
| L5 Entity runtime layer | Parse entity contracts, encode sproto payloads, dispatch endpoint calls, track properties, synchronization and prototype RPC. | `include/mistlane/entity_contract.hpp:94`, `include/mistlane/entity_codec.hpp:23`, `include/mistlane/entity_runtime.hpp:12` | Process memory |
| L6 Persistence, admission and observability layer | Own durable records, return queues and writer fencing; verify API/ticket admission; expose metrics, probes and dashboards. | `include/mistlane/record.hpp:125-497`, `include/mistlane/api.hpp:66-115`, `include/mistlane/diagnostics.hpp:22` | SQLite when journal/API store configured; memory otherwise |

## Architecture walkthrough

### L1 Process and role layer

- **Implementation:** `main` dispatches `test`, `bot`, `contract`, embedded scaffold commands and the normal node path at `src/main.cpp:195-203`; scaffold templates live at `src/scaffold.cpp:88-180`.
- **Configuration schema:** `Config` is declared at `include/mistlane/node.hpp:18-70`; the config-file `KEY` to flag mapping is at `src/main.cpp:107-125`. Priority is file, then `MISTLANE_<KEY>`, then later explicit flags (`src/main.cpp:103-106` and `src/main.cpp:206-215`). Role-specific combination checks are at `src/main.cpp:336-448`.
- **Dedicated protocol or copy:** none; process wording belongs to [process roles](./process-roles.md) and getting started.
- **Calls:** no layer owns wire I/O. It constructs the configured L2 node (or another command target) and exits through normal process return.

### L2 Transport and session layer

- **Implementation:** TCP framing is `TcpChannel` at `src/node.cpp:226-295`; Drogon-backed web adapters are at `src/web.cpp:16-113`; one `Session` parses and dispatches protocol controls and business payloads at `src/node.cpp:1146-1860`; `MuxHub` multiplexes logical sessions at `src/node.cpp:1668-1894`. The generic transport seam is `include/mistlane/transport.hpp:12-24`.
- **Configuration schema:** listener definitions and transport choices ride `Config.listeners`, `--listener`, `--transport`, certificates, token, multiplex and overload fields declared at `include/mistlane/node.hpp:18-70`; listener file parsing begins at `src/main.cpp:84-101`.
- **Dedicated protocol and copy:** `MLS1`, `MLC1`, `MLC2` and `MLM1` are summarized in [protocol analysis](./protocol-analysis.md#protocol-surface-inventory); overload behavior is in [cross-server and overload](./cross-server.md).
- **Calls:** clients and browsers connect to this layer. `MLC1` asks a gateway route; `MLS1` authenticates an internal service leg; `MLC2` adds request correlation; `MLM1` carries logical sessions over one authenticated upstream. Business payloads then enter L4; `MLE1` is intercepted for L5 (`src/node.cpp:1601` and `src/node.cpp:1638-1648`).
- **State:** session buffers, route state, correlation tables and token buckets are per-process memory.

### L3 Coordination and registry layer

- **Implementation:** `RegistrySession` implements the registry-facing wire behavior at `src/node.cpp:2214-2793`; `RegistryMember` maintains an upstream registration at `src/node.cpp:2795-2922`; `RegistryClient` resolves placement at `src/node.cpp:2926-3010`; the transaction model is `MigrationTransaction` at `include/mistlane/migration.hpp:116-364`, with `HomeGenerationAuthority` and `WriterLease` nearby at `include/mistlane/migration.hpp:72-114`.
- **Configuration schema:** `Config` coordinates `--coordinator`, `--namespace`, `--scope`, route epoch and optional `--journal` (`include/mistlane/node.hpp:18-70`, `src/main.cpp:109-125`).
- **Dedicated protocol and copy:** `MLR1` and `MLR1 MIG` verbs are documented in [protocol analysis](./protocol-analysis.md#protocol-surface-inventory) and process roles.
- **Calls:** members register through authenticated MLR1; services query placement and routes; migration owners drive typed PREPARE/EVENT/COMMIT/ABORT/RETIRE transactions. This is a separate control plane from business forwarding.
- **State:** members, routes, migration and authority state are volatile unless `--journal` supplies `MigrationJournal`; SQLite tables are created at `src/journal.cpp:342-392`.

### L4 Service execution layer

- **Implementation:** `LuaService` is declared at `include/mistlane/lua_service.hpp:91-105` and implemented at `src/lua_service.cpp:136-1063`. The VM restricts libraries at `src/lua_service.cpp:147-151`, applies an instruction budget at `src/lua_service.cpp:110-115`, and registers nine API families at `src/lua_service.cpp:152-159`. `RedisRuntime` is at `src/lua_service.cpp:187-210`; outbound HTTP uses libcurl at `src/http_client.cpp:23-58`.
- **Configuration schema:** script and Redis selection come from `--script`, `--redis`, and `MISTLANE_REDIS_*`; their config keys are mapped at `src/main.cpp:109-125`, while runtime validation is in L1.
- **Dedicated protocol and copy:** Lua API and lifecycle are in [Lua and lifecycle](./lua.md); callback boundaries are in [HTTP, WebSocket and callbacks](./http.md).
- **Calls:** L2 delivers session payloads into script callbacks; L5 invokes bound entity tables; the script reaches Redis, HTTP, placement, ticket, return and metric adapters through registered APIs.
- **State:** script globals and async work are per-process memory; Redis state exists only when configured; MySQL appears only as an example adapter seam in `include/mistlane/mysql_pool.hpp:15-46`.

### L5 Entity runtime layer

- **Implementation:** contract parsing and generators are centered in `EntityContract` at `include/mistlane/entity_contract.hpp:94`; sproto encoding is `SprotoCodec` at `include/mistlane/entity_codec.hpp:23`; live endpoint decoding is `EntityEndpoint` at `include/mistlane/entity_endpoint.hpp:42`; runtime seams are declared at `include/mistlane/entity_runtime.hpp:12`; property, synchronization and persistence prototypes are in `include/mistlane/entity_property.hpp:56`, `include/mistlane/entity_sync.hpp:55`, `include/mistlane/entity_sync.hpp:157` and `include/mistlane/entity_persist.hpp:45`.
- **Configuration schema:** `--contract` selects the entity contract (`src/main.cpp:109-125`); L1 rejects contract combinations that do not fit the selected role.
- **Dedicated protocol and copy:** contract syntax, SDK generation, envelope and errors are in [entity contracts](./entity-contract.md); `MLE1` surface is also inventoried in protocol analysis.
- **Calls:** inbound `MLE1` is decoded before the Lua payload (`src/node.cpp:1338-1348`); the contract is loaded into the node at `src/node.cpp:3187-3208`; decoded entity work reaches L4-bound dispatcher tables, while property and persistence implementations remain prototype seams.
- **State:** contracts, codecs, properties, RPC call tables and gateway correlation are process memory; no Redis or MySQL state belongs to this layer.

### L6 Persistence, admission and observability layer

- **Implementation:** durable record and return semantics are in `include/mistlane/record.hpp:125-497`: `HomeSequenceAuthority` at `125`, `RecordStore` at `178`, `ReturnSendQueue` at `342`, and `WriterLease` at `456`; the SQLite backend is `MigrationJournal` at `src/journal.cpp:342-398`. MLA1 verification and acceptance interfaces are at `include/mistlane/api.hpp:66-115`, with the SQLite implementation at `src/api_store.cpp:64-110`; MLT1 ticket primitives are at `include/mistlane/entry_ticket.hpp:15-210`. `Diagnostics` is at `include/mistlane/diagnostics.hpp:22`, `ProbeServer` at `src/node.cpp:296-436`, and `DashboardServer` at `src/node.cpp:438-560`.
- **Configuration schema:** journal path, metrics/probe/dashboard listeners and admission secrets are represented by `--journal`, `--metrics-listen`, `--probe-listen`, `MISTLANE_MLA1_*`, `MISTLANE_TICKET_SECRETS`, and `MISTLANE_PROBE_TOKEN`; config keys are listed at `src/main.cpp:109-125`.
- **Dedicated protocol and copy:** `MLD1`, durable acknowledgement, MLA1 and service admission are covered by [cross return and single-writer protocol](./cross-return-protocol.md) and [service contracts](./service-contracts.md).
- **Calls:** L2/L3 ask this layer for probes and diagnostics; L4 asks it for ticket issuance and return submission; MLA1/API and MLT1/ticket callers cross process or network boundaries. MLD1 record traffic is a framed-TCP leg.
- **State:** SQLite stores migration/authority/placement rows, record rows/fences and durable API acceptance (`src/journal.cpp:354-378`, `src/api_store.cpp:64-110`); without explicit stores, queues, leases, tickets and counters remain memory.

## Why these boundaries

1. **Process choice precedes protocol behavior.** The seven runnable roles are a closed table (`include/mistlane/roles.hpp:6-16`) and `main` validates role-specific script, upstream, secret, contract and journal combinations before any socket exists (`src/main.cpp:336-448`). A layer for this prevents transport code from becoming another configuration parser.
2. **Wire termination and business execution are separate failure domains.** TCP framing is isolated in `TcpChannel` (`src/node.cpp:226-295`), session route selection and forwarding in `Session` (`src/node.cpp:1226-1348`), and VM construction/APIs in `LuaService` (`src/lua_service.cpp:136-1063`). Connection backpressure, framing errors and script errors therefore have different ownership.
3. **Registry is a control plane, not a business relay.** `RegistrySession` owns member, route and migration verbs (`src/node.cpp:2214-2793`), while `Session` forwards gameplay (`src/node.cpp:1226-1348`). Keeping them separate preserves the architecture's placement/return distinction rather than burying it in a gateway path.
4. **Script hosting is an adapter boundary.** Lua libraries are closed at `src/lua_service.cpp:147-151`, instructions are capped at `src/lua_service.cpp:110-115`, and host capabilities enter through named API registrations (`src/lua_service.cpp:152-159`). Scripts cannot expand their own I/O surface.
5. **Entity metadata is generation-time, while endpoint behavior is run-time.** XML parsing, lock/digest generation and C++/JS/C# output are concentrated in `EntityContract` (`include/mistlane/entity_contract.hpp:94`), while the small `MLE1` endpoint is separate (`include/mistlane/entity_endpoint.hpp:42`). Schema compatibility and live dispatch change at different rates.
6. **Durable versus volatile ownership is explicit.** `RecordStore`, fences, return queues and writer leases are separate types (`include/mistlane/record.hpp:178`, `include/mistlane/record.hpp:342`, `include/mistlane/record.hpp:456`), and SQLite durability appears only through `MigrationJournal` (`src/journal.cpp:342-392`). The boundary prevents an in-memory ACK from being described as durable.
7. **Admission is verified, replay-aware I/O.** MLA1 separates verification, acceptance key and durable store (`include/mistlane/api.hpp:66-115`); MLT1 uses constant-time HMAC verification (`include/mistlane/entry_ticket.hpp:182-190`). These concerns need their own lifecycle independent of gameplay routing.
8. **Metrics and readiness serve every layer but implement none of them.** `Diagnostics` centralizes Prometheus and Crashpad state (`include/mistlane/diagnostics.hpp:22`), while readiness is answered by `ProbeServer` (`src/node.cpp:296-436`). This makes observability a dependency direction, not a business feature.
9. **The model also documents extraction debt, not only intent.** `src/node.cpp` still contains L2, L3, probe and dashboard implementations (lines `1226-1894`, `2214-3010`, `296-560`); `src/lua_service.cpp` hosts script runtime plus many adapters (lines `136-1063`); `EntityContract` combines parser and generators (`include/mistlane/entity_contract.hpp:94`). The layer model gives future splits a target without claiming that every file already matches it.

## Related pages

- [Glossary](./glossary.md)
- [简体中文](./zh/source-layer-analysis.md)
- [Architecture and migration](./architecture.md)
- [Protocol analysis](./protocol-analysis.md)
