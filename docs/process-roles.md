# Process responsibilities and naming

This page defines Mistlane's target responsibilities using the agreed names `gameServer`, `crossServer` and `coordinator`. **A documented role does not imply an implemented service or CLI option.** The executable remains `mistlane`; current arguments are documented in the [guide](./guide.md). Apollo's naming is outside this change.

## Responsibilities and boundaries

| Target name | Responsibilities | Boundary |
|---|---|---|
| `gateway` | Game TCP/WS/WSS ingress, connections and sessions, forwarding to the current player host, downstream delivery, broadcast sending, limits and backpressure | Owns connections and route caches, not authoritative gameplay state, instance rules or business database mutations |
| `login` | Account authentication, character listing/selection, requesting placement and the pre-game entry flow | Does not run scenes or battles; authentication success does not imply that a character is ready in a game process |
| `coordinator` | Process registration/readiness/load, placement, player/scene ownership and transfer states, channel/server/cross-region transfer coordination | Does not run scene ticks or battles, forward every gameplay message, or hold full character state; a missed heartbeat alone cannot authorize a second active owner |
| `gameServer` | Regional character objects, persistent maps, dynamic instances, monsters, combat, activities and reward rules; loading/saving through data | Multiple scenes and players per process; character data and scene behavior follow the legacy combined hosting model, without a Base/Cell responsibility split |
| `crossServer` | Shared cross-region scenes, instances, battles and activities; participant runtime state, origin and return context | Gameplay execution, not the old CrossWorld coordination role; does not imply a separate account system or player database |
| `data` | Ownership-aware loading, cache and persistence; transactions, write concurrency rules, save outcomes and errors; database integration | Does not decide combat/reward rules; receiving a save message is not proof of durable completion |
| `apiServer` (optional) | External HTTP/HTTPS endpoints and callbacks, WebSocket upgrades/lifecycle/messages, ingress authentication and raw-request validation, bounded submission and internal business calls | Application-specific payment verification; acknowledgement follows durability contracts; gameplay services own reward rules; upgraded frames use WebSocket callbacks |

Roles and listener protocols are separate. Both game ingress and the HTTP service need independently configured WS/WSS listeners, including simultaneous cleartext/TLS listeners. WS is not a separate process role. Multiple-listener support remains unimplemented.

## External ingress capability and process deployment

The agreed name is `apiServer`, consistent with gameServer/crossServer. Here it serves external business APIs such as platform callbacks, payment notifications and operations; gateway remains the real-time game-client ingress. API server is an existing term: [Nakama's server-to-server documentation](https://heroiclabs.com/docs/nakama/server-framework/runtime-examples/server-to-server/) describes attaching HTTP handlers to an API server. This is naming evidence, not a claim of identical deployment boundaries.

HTTP/HTTPS and WS/WSS are listener/callback capabilities. Enabling them does not require another process. The old code has no unified external API process: d_pay and d_payserver serve payment responsibilities, while d_world/HttpServer.hpp embeds HTTP within World.

apiServer is an optional external-system ingress responsibility, not a renamed legacy process. Applications may deploy it independently for a separate security, capacity or release boundary, or embed the ingress module in an existing service. Payment business contracts remain application-defined. gateway serves game clients; external ingress handles platform callbacks/management and their WebSocket connections. Shared protocols do not imply shared identity or permissions.

## Coordinator scope

Regional coordination and cross-region-group coordination use the same responsibility in separate deployment scopes. A regional coordinator manages its gameServer fleet; a group coordinator manages crossServer placement. They exchange placement, transfer and return information. A single global coordinator is not required on every game's hot path.

coordinator owns placement decisions and registration state. Deployment scripts or a supervisor such as systemd own process startup, restart and resource limits.

Each scope needs an explicit authority for ownership changes. Election, recovery and prevention of duplicate hosting still need design; starting multiple ordinary coordinators does not establish high availability.

## Main flows

1. **Entry:** client → gateway → login; login requests placement; gameServer loads through data, creates the character and enters a scene; readiness precedes completed route publication and the client notification. Allocation/loading must have explicit intermediate states.
2. **Gameplay:** client ↔ gateway ↔ current gameServer/crossServer. The game service executes player/scene logic; coordinator is not a compulsory per-message hop.
3. **Local scene change:** leave the old Scene, update location, enter the target and update required summaries. No inter-process character copy occurs.
4. **Process transfer:** coordinator arranges a destination; the old host stops relevant writes and creates recoverable state; the destination accepts/acknowledges, the route changes and the old host releases. Failure handling must prevent two writable character owners. The handshake and recovery protocol remain unimplemented.
5. **External callbacks:** external platform → external ingress module (optionally a separate apiServer process) → durable acceptance; resolve the current host, invoke business validation/idempotency/persistence, then notify online clients. Offline execution must be designed together with character activation and write ownership.

## Cross-region data ownership

Entering crossServer changes execution placement, not the account, character identity or home data domain. Validated transfer context includes origin, character identity, hosting generation and return information. Current placement is resolved through coordinator; orders and durable records must not pin a gameServer address.

The target keeps permanent data in its home region while moving character execution to crossServer. Cross-region changes return through home-side services to home data; crossServer need not hold every region's database addresses. The old CrossWorld → home World → Data/Cache path and its limits are documented in [players, scenes and cross-region execution](./player-scene-model.md). Mistlane's forwarding protocol, durable acknowledgement, generation/idempotency, transfer/write authority, retries/offline and failed return still need design. Data edges denote that service path, not direct cross-region database writes or concurrent character writers.

## Legacy correspondence

| game-fogs component | Target responsibility |
|---|---|
| Gate | gateway |
| Login | login |
| Coordination in World/CrossWorld | coordinator in separate scopes; global gameplay needs an explicit owner rather than being dumped into coordination |
| Hegel | gameServer |
| CrossHegel | crossServer |
| Data/Cache | data |
| Payment services and World-embedded HTTP | Preserve application responsibilities; apiServer is optional consolidation, not a renamed legacy unified ingress process |

The old `requesthegel2hegel` transfer passes through Gate to change the route and forward data. That is an observed implementation, not a commitment to reproduce its handoff protocol.

## Current code and migration boundary

| Current CLI role | Implemented runtime | Remaining work |
|---|---|---|
| `gateway` | TCP/WS/WSS forwarding, upstream per connection; `--routing cross` selects static cross-region routing/admission | Player identity, dynamic host routes, bidirectional push and transfer integration |
| `gameServer` | Lua message execution, lifecycle/reload, direct TCP/WS/WSS; existing HTTP adapter retained | Character/scene/instance hosting and persistence chain |
| `crossServer` | TCP cross context, Lua execution and final connection admission | Character transfer, cross-region scenes and home-data updates |
| `apiServer` | Lua HTTP/HTTPS hooks, HTTP by default, optional WS/WSS message transport | Platform business callbacks, reliable internal calls and multiple listeners |
| `login`, `coordinator`, `data` | Names/responsibilities registered; CLI explicitly rejects startup as not implemented | Authentication, placement/ownership, home-data services and integration |

The single executable remains mistlane. Existing roles have migrated; the independent generic forwarding role is removed and static cross ingress is a gateway routing policy, not a complete coordinator. Old CLI aliases are rejected. MLC1/PROBE1, room/group keys and max-shard-sessions quota retain their protocol meanings; they do not introduce process roles.

## Acceptance

The root `todo.md` tracks service identity/communication → coordination/ownership → characters/scenes → cross-region transfer/data writes → ingress/tooling. A runtime rename must update validation, Lua ServiceContext, scaffold, help, tests, metric labels and both locales together. Existing review-remediation tasks remain valid.


See [players, scenes and cross-region execution](./player-scene-model.md) for map data, scene instances, Session/Avatar and C++/Lua boundaries.
