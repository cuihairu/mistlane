# Legacy MMO backend and modern server architecture

## Classification

Yes. The source is an MMO-style backend: it has long-lived regional worlds, gateways, login and data services, Lua gameplay workers, and cross-region gameplay. The architecture is a multi-process online game server, not a match-only backend.

The important qualification is that “MMO backend” describes the product shape. It does not mean the implementation already has modern distributed ownership, failure recovery, or horizontal scaling. The old design is a service-oriented process layout from an earlier generation, with configuration and routing strongly tied to a shared deployment environment.

## What the old design does

```mermaid
flowchart LR
  C[Clients] --> G[Gateway processes]
  G --> L[Login / World / gameplay processes]
  W[World coordination] <--> H[Gameplay workers]
  H <--> X[Cross-world coordinator]
  X <--> XH[Cross-world gameplay workers]
  W --> D[Data/cache service]
  H --> D
  D --> SQL[(MySQL)]
```

The repository shows separate native executables and Lua entrypoints for gateway, World, gameplay, cross-world and supporting services. Deployment scripts generate shared and per-service Lua configuration, compute port ranges and process counts, then start services in a prescribed order. Runtime scripts expect a particular current directory and derive binary, config, Lua, log and pid paths from it. The older command line accepts an explicit service config, instance index and log config.

This separation remains useful: gateways, game logic, storage and control services have different resource needs and failure modes. The weak point is the shared coordinator path. Multiple cross-game workers do not help if they all depend on one coordinator for global routing, broadcasts or shared state. The old capacity fallback could also select an already-full gameServer, and one queue append path had no local item/byte cap. These are concrete overload risks, but they do not prove the cause of the historical crash.

## Comparison with common modern patterns

| Area | Legacy backend evidence | Common modern pattern | Mistlane today |
|---|---|---|---|
| Process packaging | Many purpose-built binaries with separate Lua/config entrypoints | A small set of reusable roles, packaged consistently and deployed declaratively | One binary with `gateway`, `gameServer`, `crossServer` and `apiServer` roles |
| Configuration | Shared Lua globals plus generated per-daemon files; scripts rely on working-directory conventions | Explicit per-instance configuration, versioned deployment config, secrets supplied separately | `mistlane init` generates per-instance shell-assignment files with root-relative paths; launchers translate them to CLI arguments, and the runtime also parses a native `KEY=VALUE` file through `--config` |
| Game-state ownership | World/gameplay/cross services coordinate through application-specific routing and RPC | A world, zone, room or match has one clear authoritative owner; partitions are explicit and bounded | Static room hash across a configured crossServer list, local hard admission, no leases or state migration |
| Cross-server scaling | Cross coordinator can remain a hot point while gameplay workers are added | Partition by independent state domains; separate control-plane operations from hot gameplay data paths | Cross forwards to shards but remains a single ingress per deployment and uses static upstream order |
| Overload handling | Worker preference can fall back to a full gameServer; queue sizing needs wider tracing | Admission, queue bytes and task budgets are bounded; reject or defer work before the main loop stalls | Connection, frame, rate and room limits exist, plus local readiness gating; no global bytes/queue budget or measured capacity |
| Failure recovery | Custom registration, reconnect and persistence behavior is tied to the old framework | Explicit health/readiness, drain, fencing/epochs, idempotency, snapshots and recovery policy | Timeouts and epoch mismatch; PROBE1 readiness gates new cross joins; no discovery, drain protocol, replay or migration |
| Operations | Shell scripts generate configs, start/stop processes, inspect pids/logs | systemd or an orchestrator manages process lifecycle, restart policy, resource limits and rollout | `mistlane init` generates local cluster scripts and a systemd unit template; a runtime container image (deploy/Dockerfile) and its validation drill (tools/container_smoke.sh) deliver the deployment workflow |
| Observability | Logs and a Lua long-call interruption hook | Metrics, traces, profiles and native crash dumps answer different diagnostic questions | Optional Prometheus scrape endpoint, Crashpad local capture (validated by the dump drill) and the internal PROBE1 listener are wired; external-sampler profiling remains |

“Modern” does not require Kubernetes, microservices, or an actor framework in every game. Modernization means making ownership, resource limits, placement, health, deployment and recovery contracts explicit. A single well-bounded process can be more reliable than many services joined by unbounded queues.

## Two modern game-server shapes

Session-based games allocate a dedicated process per match/session. Agones provides atomic allocation from ready server fleets and fleet autoscaling; this fits disposable sessions and short matches better than a persistent shared world. [Agones allocation](https://agones.dev/site/docs/reference/gameserverallocation/) · [Agones autoscaling](https://agones.dev/site/docs/reference/fleetautoscaler/).

Persistent-world games usually partition the world into regions, zones, instances or rooms. Each partition has one active state owner, while gateways, account services, matchmaking and persistence are supporting services. A server-authoritative match system similarly keeps one match state on one host for consistency, then distributes independent matches across nodes; Nakama documents this model and its tick-driven match lifecycle. [Nakama authoritative multiplayer](https://heroiclabs.com/docs/nakama/concepts/multiplayer/authoritative/).

The old game is closer to the persistent-world model. It should not be rewritten as “one container per room” without measuring room lifetime, cross-room interactions, world persistence and player migration costs.

## Recommended modernization path

1. Keep the one-owner-per-room principle and hard admission. Measure Lua tick lag, request latency, CPU/RSS, active sessions, room sizes and queued bytes before selecting capacity values.
2. Separate control-plane work (service registration, placement, deployment) from gameplay traffic. Local readiness gating ships; leases/fencing and cluster placement must still precede automatic owner selection. A failed health probe must not silently move live state to another crossServer.
3. Split hot global activities into independently owned rooms or zones; use bounded messaging, subscriptions/AOI, batching and asynchronous persistence. Do not solve one overloaded room by adding workers that still share the same state owner.
4. Add observability in layers: Prometheus for trends and saturation, Crashpad for native postmortem dumps, a sampling profiler for CPU attribution, and internal probes for process/event-loop responsiveness. A Lua timeout hook is only an interruption guard.
5. Start with explicit per-instance config and systemd for a single host. Adopt a game-server orchestrator only when fleet placement, warm capacity, rolling updates or multi-host allocation justify it.

## Mistlane assessment

Mistlane modernizes dependencies and extracts generic process roles; it is not yet a modern MMO runtime. Its gateway cross routing/crossServer code is a research implementation of stable room affinity and local overload rejection. Cross nodes already gate new room joins on a crossServer's PROBE1 readiness. It lacks dynamic service discovery, route leases, room migration, globally coordinated limits, durable request handling and capacity tests. The optional Prometheus endpoint and Crashpad capture are initial diagnostics, not a complete operations platform. Its deterministic hash can remap rooms when the ordered crossServer list changes, so topology changes require drain or application-level migration.

The project initializer now generates instance-specific configuration, a foreground launcher, local start/stop/status/log tools and a systemd unit template. Internal liveness/readiness endpoints exist and a profiler route is decided (external sampler; none ships in-process). The target-environment drills named below are delivered (crash drill with dump retention, metrics scraping, probe overload, stress profiling and the container stress drill in [deployment](./deployment.md) / [profiling](./profiling.md)); what remains open is the coordination-plane decision itself, now informed by the recorded evidence below.

## Deferred decision: what grows the coordination plane

Which of these options to adopt stays gated on the target-environment stress drills (ledger item); the comparison itself is fixed now so the drills can measure the numbers that discriminate between rows.

| Option | What it adds | What it solves | What it costs | Choose when |
|---|---|---|---|---|
| Static lists + MLR1 membership + route records (today) | Nothing new | Discovery, placement lookups, reconnect routing to the current host | No ownership handover: a dead host just drops its routes and reconnects go back through `MLR1 ROUTE EMPTY` → entry flow | Now — this is the drills' baseline |
| Leases/fencing over route records | Renewal/expiry plus a fence token on MLD1 writes | A partitioned or crashed host cannot keep writing after its lease dies | Timer machinery plus two new failure modes (renewal loss, clock skew); takeovers need explicit fencing verdicts | If the drills show a host outliving its control connection, or capacity work needing live takeover |
| Room drain/migration | Cross-node participant moves with target-side admission on real state | Topology changes without losing gameplay | `MLC1` context is per-join today; the whole Avatar/transfer chain must exist first | Only after the Avatar runtime ships — never before |
| Container orchestration | Fleet placement, warm capacity, rolling updates | Operations at fleet scale | An orchestrator cannot see rooms; without drain it evicts live state owners | When multi-host fleet operations actually exist |

What the drills should therefore record: control-plane failover time (member retire → registry reflects it), reconnect-routing staleness (`MLR1 ROUTE` verdicts after a host kill), join-rejection rates under churn, and whether any observed partition lets a host keep accepting writes — those four numbers pick the row.

### What the drills recorded (2026-11-28)

The four discriminating numbers, from the delivered drills (see [validation records](./validation.md), [profiling](./profiling.md) and [deployment](./deployment.md)):

1. **Control-plane failover is bounded by TCP teardown plus one poll, not by a lease timeout.** A `SIGKILL`ed member leaves the registry within one 200 ms `ESTATUS` poll — the coordinator observes the transport close before the next poll fires, which is why the migration tests can only commit past a `SIGSTOP`ed (unreachable but still-publishing) destination, never a killed one (`entity.migrate.recover`). Graceful retirement clears routes immediately; a coordinator restart restores durable placements and answers `MLR1 ROUTE EMPTY` until the hosting member re-registers.
2. **Reconnect-routing staleness is the re-placement path, not a stale write path.** After a host dies its route answers `MLR1 ROUTE EMPTY` and the entry flow re-places the character; after a completed entity migration the route and `MLR1 HOST LIST` follow the destination in the same transaction (`entity.migrate.inflight`).
3. **Join rejection under churn is measured and configurable.** In real containers: the default flood guard stopped all 32 clients of the documented baseline leg (the finding that made the guard configurable — `--rate-capacity`/`--rate-refill` with a `mistlane_rate_limit_stops_total` counter); with tuned guards plus the admission envelope (limit 337) and room quotas on both sides, 128×781 = 99968/99968 joins succeeded across a compose cluster; the probe face pins the 64-connection cap, overlong-line close, silent-connection read limit, saturation hysteresis (`notready`→`ready`) and cross `NOT_READY` admission with rejection counters.
4. **No drill produced a host that kept accepting writes past its control connection.** Writes are fenced by generation at the data leg regardless of membership: the data-issued generation grant fences durable submits at the granted generation, the ownership fence verdict beats a standing lease, a transfer-frozen source answers `NOT_READY`, a pre-transfer generation answers `STALE` from the hosting fence, post-commit copies answer `UNROUTED`, and `MLD2` loads adopt the row's generation before restore (a `RECEIVED` mid-transfer aborts and stays dirty).

Mapped against the table's own "choose when" cells (the comparison stays fixed; this records where its triggers stand):

- **Static lists + membership + route records** — still the standing choice; no drill evidence demands more.
- **Leases/fencing over route records** — its firing condition (a host outliving its control connection, or a live-takeover need) was never produced; generation fencing at the data leg already denies the partitioned-writer outcome the row exists to prevent. Adding renewal/clock machinery would buy a failure mode the drills never observed.
- **Room drain/migration** — its precondition ("only after the Avatar runtime ships") is now met: the entity/scene transfer chain moves state cross-host under target-side admission end to end (freeze → fence → commit → adopt, `DESTINED`-seeded scenes, ordered switch gate). Whether to generalize that primitive into whole-room migration remains a deliberate decision, not a gap forced by the drills.
- **Container orchestration** — its trigger (real multi-host fleet operations) does not exist; all deployment evidence is single-host compose clusters.

The decision itself stays with the maintainer; the drills are done and the numbers above are the input it was gated on.
