# Native cross-server routing and overload protection

## Legacy design and evidence

The old game coordinated regional World/Gate processes through CrossWorld and executed cross gameplay on multiple CrossHegels. Distributed CrossHegel indices existed, but those processes connected to the first serverList CrossWorld entry. Cross RPC forwarding, JSON re-encoding, global state and broadcasts concentrated work there. Adding gameplay workers did not automatically scale that coordinator.

ServerManager.pickupCrossHegel first preferred an online node below HegelLimit; when all were full it still randomly selected an online node. That was a soft preference, not hard admission. ServerConnBase.pushQueue appended to a deque without an item/byte cap at that entry point; callers and buffering need further tracing.

These observations do not establish the historical crash cause. Core dumps, OOM logs, RSS/CPU, queue bytes, connections, room population and Lua tick delay are needed to distinguish memory exhaustion, a native crash and saturation. The public source package contains analysis, not original business code or production configuration.

## Implemented topology

```mermaid
flowchart LR
  A["Realm A"] --> C["cross · group/room affinity"]
  B["Realm B"] --> C
  C --> Q{"Admission"}
  Q -->|full| R["Reject · no silent reassignment"]
  Q -->|room hash 0| S0["shard 0 · Lua VM · final quota"]
  Q -->|room hash 1| S1["shard 1 · Lua VM · final quota"]
  S0 --> X["Bounded rooms"]
  S1 --> Y["Independent rooms"]
```

cross and shard are native mistlane roles over internal framed TCP. The same group/room maps to the same shard across realms using fixed 64-bit FNV-1a modulo the ordered upstream list. A session owns one upstream connection with serial request/response; no shared-connection multiplexing, unbounded application send queue or automatic replay is introduced. A full stateful room is rejected rather than silently split.

```sh
build/debug/mistlane --role shard --listen 127.0.0.1:7201 --script examples/cross.lua --route-epoch 1 --max-room-sessions 16
build/debug/mistlane --role shard --listen 127.0.0.1:7202 --script examples/cross.lua --route-epoch 1 --max-room-sessions 16
build/debug/mistlane --role cross --listen 127.0.0.1:7300 --upstream 127.0.0.1:7201 --upstream 127.0.0.1:7202 --route-epoch 1 --max-room-sessions 16
```

Run each command in a separate terminal. Limits are illustrative, not measured capacity.

## Handshake and application context

The first framed payload is ASCII: MLC1 1 realm-a group-1 room-42. Fields are protocol marker, route epoch, source realm, cross group and room. Identifiers contain 1–64 ASCII letters, digits, underscores or hyphens; the handshake is at most 256 bytes. Epoch is a positive decimal integer up to 1000000000.

cross admits and forwards the handshake to the selected shard. The shard performs final admission and returns MLC1 OK 1. Subsequent payloads reach Lua message with the context as its second argument. Failures return MLC1 ERR followed by INVALID, EPOCH, ROOM_FULL, GROUP_FULL or SHARD_FULL (or UPSTREAM for an incompatible shard response), then close. Transport errors and global connection admission can close without a response. Successful admission is neither authentication nor durability.

| Option | Default | Meaning |
|---|---|---|
| --max-room-sessions | 64 | Connections for one group/room |
| --max-group-sessions | 128 | Connections for one group |
| --max-shard-sessions | 256 | Connections to one destination shard |
| --max-connections | 1024 | Node limit, further reduced by memory admission |
| --memory-budget-mib | 256 | Conservative network admission envelope of 6 MiB + 64 KiB per session; default effective cap is 42 |
| --route-epoch | 1 | Matching configuration generation on cross and shards |

Ingress quotas are local to each cross process. Each shard enforces final quotas across all its ingress connections, preventing multiple routers from bypassing its room cap. Group quotas across different shards are not globally coordinated. Disconnection/failure/timeout releases admission and empty room records are removed.

These limits count **connections, not distinct authenticated players**. Applications must validate tickets, duplicate logins and realm/group/room authorization. Use trusted private networking: mTLS and signed tickets are not implemented.

The memory envelope bounds admitted sessions before protocol buffers are allocated. TCP checks the 1 MiB length before body allocation; Drogon has body and WS message limits. This is an estimate, not a hard RSS quota, and excludes Lua state, TLS, allocator overhead and some third-party buffers. Lua can allocate a large response before the transport rejects its size. Measure actual memory and apply process/cgroup limits.

## Reconfiguration and failures

Changing upstream order/count remaps rooms. Stop new joins, drain affected rooms or migrate application state, then update route epoch and configuration together. Epoch rejects differing generation numbers; it does not validate identical upstream lists.

A failed shard closes affected sessions without automatic replay or stateful failover. Other shards remain usable. Recovery needs application snapshots, idempotent request IDs, reconnect and compensation. Durable route tables, leases, discovery, distributed transactions and online migration remain unimplemented. Native support means generic routing/admission, not restored legacy gameplay.

## Capacity plan

Measure representative room workloads and message sizes, using tick delay, P99 latency, CPU/RSS and output backlog to set a hard room cap with headroom. Partition large activities into independent matches/rooms with one state owner each; more shards increase room capacity, not the capacity of one unbounded room. Use AOI/subscriptions, batching and broadcast frequency limits. Keep persistence asynchronous and bounded.

Reject full rooms or use an external bounded queue with retry jitter. Exercise slow consumers, slow/down shards, burst joins, disconnect churn and recovery after capacity is released. Observe admission rejections, active sessions, bytes, tick drift, timeouts and reconnect rates. Production metric collection and a sustained load generator are not yet integrated; functional tests are not capacity evidence.

## Internal diagnostics status

An optional internal probe listener (`--probe-listen IP:PORT`, off by default) answers liveness and readiness queries inline on the Asio event loop, so any reply itself proves the loop is making progress; a TCP connection or OS keepalive only establishes transport-level reachability. The old code has a Lua call timeout hook (`C_monitorDeadLoop`), but no CPU/memory profiler. That hook can interrupt some runaway Lua execution and does not identify hot functions or explain native stalls.

| Signal | Purpose | Design boundary |
|---|---|---|
| Prometheus metrics | Track active sessions, admission rejects, payload sizes and latency, Lua callback/tick duration, event-loop timer lag, and RSS/CPU where available | Scrape from a private metrics listener; keep labels low-cardinality (role/shard/result, never player/room IDs). |
| Internal probe | Report process responsiveness and readiness/capacity state | Ships as `--probe-listen` with the PROBE1 protocol below. Readiness false blocks new joins but preserves existing room ownership. |
| Crashpad | Preserve a local minidump after native crashes | Start the out-of-process handler and use a bounded local database; no upload endpoint by default. Archive matching symbols per build. |
| Profiler | Attribute CPU and allocation cost to code paths | External sampler against the running process only. No in-process sampler: self-sampling spends the same CPU budget it measures. See the runbook below. |

### Internal probe protocol (PROBE1)

`--probe-listen IP:PORT` opens a second, internal-only listener that speaks a one-line protocol. Answers are produced on the event loop, so any reply doubles as a liveness signal; orchestrators and load balancers can poll a separate port without touching player traffic.

```
PROBE1 <nonce> liveness    -> PROBE1 <nonce> ok alive
PROBE1 <nonce> readiness   -> PROBE1 <nonce> ok ready|notready active=N capacity=N lag_ms=F
                           -> PROBE1 <nonce> err version|auth|command|malformed
```

- One request per connection, LF-terminated; the server closes after one reply and drops idle connections after 2 seconds. `<nonce>` is `[A-Za-z0-9._:-]{1,64}` and is echoed so stale replies can be discarded.
- `liveness` proves event-loop progress. `readiness` additionally reports whether the node admits new sessions: `active` is the established session count, `capacity` is `--max-connections` clamped by the memory-budget envelope (printed at startup as `session admission limit`), and `lag_ms` is the last 100 ms tick-lag observation.
- Readiness is hysteresis-gated: a node counts as saturated once `active >= --overload-ratio * capacity` and only flips between `ready`/`notready` after `--ready-hysteresis` consecutive 100 ms ticks. Defaults: ratio 0.9, hysteresis 3.
- When `MISTLANE_PROBE_TOKEN` is set in the node environment, every request must carry the token as a fourth field (`PROBE1 <nonce> <token> readiness`) or the answer is `err auth`; the token never travels on the player-facing listener.

Cross nodes accept upstreams as `IP:PORT[:PROBE]`. With a probe port, a cross session consults the shard once, immediately before forwarding its first room join: an explicit `notready` rejects the client with `MLC1 ERR NOT_READY`, while connection errors or malformed replies reject with `MLC1 ERR UPSTREAM`. Established rooms are never re-probed and keep flowing while the shard reports notready; upstreams without a probe port forward ungated.

### Profiling and stress runs

Profiler route decision (2026-10-18): use an external sampler against the running process. No in-process sampler: a sampler inside the Asio loop spends the same CPU budget it measures, so it cannot establish its own overhead. gperftools exists as a vcpkg port but stays off the link line for the same reason; reconsider it only as an opt-in diagnostic build if the target host also blocks external sampling.

No sampler overhead numbers are claimed: this development sandbox cannot collect any. `perf` 7.0.14 is installed but `kernel.perf_event_paranoid=4` without `CAP_PERFMON` refuses to open events ("Failure to open any events for recording"), `strace -c -p` fails with `ptrace(PTRACE_SEIZE): Operation not permitted`, and `eu-stack`/`pprof` are not installed. Collecting sampler overhead on the target host is part of the environment drills. There, the workflow is `perf record -F 99 -g -p PID -- sleep 30` under `CAP_PERFMON` with `kernel.perf_event_paranoid<=1` (fall back to `--call-graph fp` if DWARF unwinding is too costly), symbolized via build-id matching against the archived symbols.

A repeatable stress run needs no external tooling: bring up a shard plus one cross node with `--metrics-listen`, then drive it with the bundled generator and scrape `/metrics` before, during and after:

```
mistlane bot --url tcp://127.0.0.1:<cross-port> --clients 64 --count 500
```

Read `mistlane_request_seconds`, `mistlane_messages_total`, `mistlane_event_loop_lag_seconds`, RSS and admission rejections from the same scrape. Runs are only comparable when the scenario script, room layout, upstream list and generator flags are identical; otherwise the numbers are noise.

The optional `/metrics` listener currently exports readiness, active sessions, admission rejections, Lua callback duration, resident memory, per-direction message and payload-byte counters (`mistlane_messages_total`, `mistlane_payload_bytes_total`), client-request latency (`mistlane_request_seconds`), upstream failures and deadline timeouts (`mistlane_upstream_errors_total`, `mistlane_upstream_timeouts_total`), and event-loop tick lag (`mistlane_event_loop_lag_seconds` plus a running max). All metrics are labeled with the role only and use fixed bucket boundaries. Crashpad can be enabled explicitly and keeps reports in a local database with uploads disabled. Crash-dump retention limits, CPU/memory profiler evaluation and overload drills in the target environment remain pending. Health-based automatic shard reassignment is excluded: a stateful room cannot safely move merely because its owner is temporarily unready. These additions provide overload evidence; they do not prove a capacity fix.
