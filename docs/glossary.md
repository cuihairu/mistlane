# Glossary

Status: source-derived terms for the [six-layer model](./source-layer-analysis.md). The synchronized Chinese table is [术语表](./zh/glossary.md). Sources use commit `2b57842dd513b3153a7795afbc2af16939609c49`.

| Term | English | Definition | Source |
|---|---|---|---|
| 进程角色 | Process role | One of seven runnable service personalities selected before transport starts. | `include/mistlane/roles.hpp:6-16` |
| 进程配置 | Node configuration | The validated fields and CLI/config-file inputs that describe one process. | `include/mistlane/node.hpp:18-70` |
| 配置文件 | Config file | A versioned `KEY=VALUE` file expanded into the same flags as the command line. | `src/main.cpp:103-125` |
| 监听器 | Listener | One configured TCP, WebSocket or HTTP endpoint owned by a node process. | `src/main.cpp:84-101` |
| MLC1 | MLC1 | Gateway cross-routing and admission control protocol. | `src/node.cpp:1226-1348` |
| MLC2 | MLC2 | Optional payload correlation protocol that adds a request identifier. | `src/node.cpp:1226-1348` |
| MLS1 | MLS1 | Internal service identity handshake used to authenticate a service leg. | `src/node.cpp:1478-1523` |
| MLM1 | MLM1 | Multiplexing protocol that carries logical sessions over an authenticated upstream. | `src/node.cpp:1668-1894` |
| MLR1 | MLR1 | Registry control protocol for membership, routes, placement and migration. | `src/node.cpp:2214-2793` |
| MLE1 | MLE1 | Entity endpoint envelope for typed request, reply and correlation traffic. | `include/mistlane/entity_endpoint.hpp:32-52` |
| MLD1 | MLD1 | Framed home-record protocol for loading, submitting, fencing and acknowledgement. | `include/mistlane/record.hpp:178-341` |
| MLA1 | MLA1 | Signed external API protocol with replay-aware acceptance semantics. | `include/mistlane/api.hpp:18-115` |
| MLT1 | MLT1 | Short-lived signed login ticket format riding the join path. | `include/mistlane/entry_ticket.hpp:15-70` |
| PROBE1 | PROBE1 | Readiness and overload probe protocol served by the probe listener. | `src/node.cpp:296-436` |
| RegistrySession | RegistrySession | The coordinator-side session that parses and executes registry verbs. | `src/node.cpp:2214-2793` |
| RegistryMember | RegistryMember | The member-side client that registers and refreshes a service in a registry. | `src/node.cpp:2795-2922` |
| RegistryClient | RegistryClient | The client used to query a member or placement route. | `src/node.cpp:2926-3010` |
| MigrationTransaction | MigrationTransaction | The typed prepare/event/commit/abort/retire state machine for ownership transfer. | `include/mistlane/migration.hpp:116-364` |
| HomeGenerationAuthority | HomeGenerationAuthority | Coordinator-side source of ownership generations used to reject stale writers. | `include/mistlane/migration.hpp:72-104` |
| WriterLease | WriterLease | Bounded ownership token that names the allowed writer and expiry. | `include/mistlane/migration.hpp:104-114` |
| LuaService | LuaService | One node-owned sandboxed Lua VM and its host API surface. | `include/mistlane/lua_service.hpp:91-105` |
| RedisRuntime | RedisRuntime | The configured Redis connection wrapper used by script APIs. | `src/lua_service.cpp:187-210` |
| Lua 沙箱 | Lua sandbox | The closed Lua library set, instruction cap and thread model around hosted scripts. | `src/lua_service.cpp:110-159` |
| EntityContract | EntityContract | Parser, compatibility lock, digest, XSD and binding-generator facade for entity definitions. | `include/mistlane/entity_contract.hpp:94` |
| SprotoCodec | SprotoCodec | The sproto-based implementation of entity payload encoding and decoding. | `include/mistlane/entity_codec.hpp:23` |
| EntityEndpoint | EntityEndpoint | Decoder and responder for the runtime `MLE1` entity envelope. | `include/mistlane/entity_endpoint.hpp:42` |
| PropertyStore | PropertyStore | Prototype in-memory store for entity property values and dirty tracking. | `include/mistlane/entity_property.hpp:56` |
| SyncReceiver | SyncReceiver | Prototype receiver that applies ordered entity synchronization events. | `include/mistlane/entity_sync.hpp:55` |
| PropertyChannel | PropertyChannel | Prototype channel that emits property updates and tracks interests. | `include/mistlane/entity_sync.hpp:157` |
| PersistentStore | PersistentStore | Prototype persistence interface and byte serialization boundary for entities. | `include/mistlane/entity_persist.hpp:45` |
| EntityRuntime | EntityRuntime | Process-side runtime seam that binds entity dispatch into the host. | `include/mistlane/entity_runtime.hpp:12` |
| 网关实体关联 | GatewayEntityCorrelation | Bounded table that rewrites outer transport correlation into fresh inner entity IDs. | `include/mistlane/entity_gateway.hpp:23` |
| RecordStore | RecordStore | Cache and durable backend interface for versioned home records. | `include/mistlane/record.hpp:178-341` |
| HomeSequenceAuthority | HomeSequenceAuthority | Cursor authority that enforces consecutive per-character record sequences. | `include/mistlane/record.hpp:125-160` |
| ReturnSendQueue | ReturnSendQueue | Ordered per-character queue that releases changes only after durable acknowledgement. | `include/mistlane/record.hpp:342-452` |
| MigrationJournal | MigrationJournal | SQLite backend for migration, authority, placement, record and fence durability. | `src/journal.cpp:342-398` |
| ApiVerifier | ApiVerifier | Signature, timestamp and command verifier for signed MLA1 requests. | `include/mistlane/api.hpp:66` |
| ApiAcceptanceStore | ApiAcceptanceStore | Atomic create-or-read interface for replay-safe external command acceptance. | `include/mistlane/api.hpp:98-115` |
| TicketVerifier | TicketVerifier | Constant-time verifier for signed and expiring MLT1 credentials. | `include/mistlane/entry_ticket.hpp:56` |
| TicketIssuer | TicketIssuer | Stateless issuer that signs MLT1 credentials with a configured secret. | `include/mistlane/entry_ticket.hpp:197` |
| Diagnostics | Diagnostics | Process-wide Crashpad, Prometheus and script metric registry. | `include/mistlane/diagnostics.hpp:22` |
| ProbeServer | ProbeServer | Listener-side readiness and overload responder. | `src/node.cpp:296-436` |
| DashboardServer | DashboardServer | Embedded HTTP page and metrics publisher for a node. | `src/node.cpp:438-560` |
