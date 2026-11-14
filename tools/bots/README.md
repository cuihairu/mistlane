# tools/bots

Standalone load and fault-injection driver for mistlane test nodes
(`mistlane-bots`). It is a separate executable from the server binary: it
rides the real session plane — framed TCP (`src/tcp_channel.hpp`), MLS1
service identity, spawn, MLE1 entity steps (`src/session_plane.hpp`,
`include/mistlane/entity_envelope.hpp`) and MLD1 record legs
(`include/mistlane/record.hpp`) — through the same CLI client the server
ships (`src/cli_client.cpp`) and the same business scenario the embedded
`mistlane bot --scenario business` uses (`src/business_plan.hpp`).

Scope is research & learning: bots find problems in this repository's own
nodes. It is not a general-purpose load product and makes no promises about
other servers or protocols.

## Build

```sh
cmake --preset linux-debug          # also builds the mistlane-bots target
cmake --build build/debug --target mistlane-bots
```

The binary lands beside the server (`build/debug/mistlane-bots`); the
`bots.smoke` CI case expects it there.

## Usage

```sh
mistlane-bots --plan plan.toml [--url tcp://IP:PORT] [--mode load|inject]
```

Plan files are a small TOML subset: `[table]` headers, `key = value` with
string/integer/boolean scalars and `#` comments. Unknown keys are refused
loudly (a typo must never silently disable a scenario), duplicate keys are
refused, and every number is range-checked. See `plan-example.toml`.

## Load mode (`[run] mode = load`, the default)

Drives the login -> joinRoom -> move/heartbeat business scenario per
connection and reports:

- one totals line: planned/completed requests, failed connections, elapsed
  seconds, achieved requests-per-second and overall p50/p95/p99;
- one `type=<login|joinRoom|move|heartbeat>` line per message kind with ok /
  fail counts and per-kind latency p50/p95/p99/max.

Plan keys (all optional unless stated):

| table | key | meaning |
|---|---|---|
| `[run]` | `url` | **required** (or `--url`); `tcp://IP:PORT` only |
| `[run]` | `concurrency` | connections, 1..512 (default 1) |
| `[run]` | `requests` | business steps per connection after login+join |
| `[run]` | `duration_s` | run until this many seconds; needs `requests` or this |
| `[run]` | `rate` | global target requests/second (shared pacing tickets) |
| `[run]` | `ramp_ms` | stagger connection starts across this window |
| `[run]` | `timeout_ms` | connect/request timeout (default 5000) |
| `[scenario]` | `contract` | **required**; the entity contract `.def` the host serves |
| `[scenario]` | `service_token` | present `MLS1 bots bots-<n> <token>` first |
| `[scenario]` | `spawn` | send `spawn <entity-id>` and expect `spawned` |
| `[scenario]` | `scope` / `entity_type` / `incarnation` / `generation` | envelope addressing |
| `[scenario]` | `entity_base` | first entity id; one per connection (default 1000) |
| `[scenario]` | `rooms` | join room `1 + connection % N` (default 1) |
| `[scenario]` | `heartbeat_every` | every Nth step after login/join is a heartbeat |

Method ids are resolved from the contract by name (`login`, `joinRoom`,
`move`, `heartbeat`); a renumbered contract fails at startup, not on the
wire. The run exits nonzero if any connection fails or nothing completed.

## Inject mode (`--mode inject`, `[run] mode = inject`)

Fault injection against a live node. Each probe asserts the exact behaviour
the implementation owes, and prints `guard=<name> verdict=held|BROKEN` plus
a detail word; the run exits nonzero if any guard broke, which is the signal
to file a todo item.

| guard | what it proves |
|---|---|
| `bad-frames.truncated-envelope` | an MLE1 frame shorter than the 39-byte envelope header answers `INVALID` "malformed envelope" |
| `bad-frames.bad-side` | an out-of-range `side` byte is refused by the decoder, not dispatched |
| `bad-frames.oversize-refused` | a declared frame longer than 1 MiB hits the transport bound and the node keeps serving afterwards |
| `late-acks.durable-dedup-gap-stale` | against a journal-backed data node: durable ACK on acceptance, `ERR STALE` for a late writer of an older generation, `ERR SEQ` fills no gap, resends dedup to the same durable ACK, and a 16-connection submit storm never regresses any accepted cursor |
| `slow-clients.backpressure-reply-after-stall` | readers that stall mid-request still receive a valid reply; healthy probes keep serving meanwhile |
| `disconnect-storm.survives-and-serves` | bursts of connect -> identify -> spawn -> first frame -> RST retire cleanly; a full business round-trip and `/metrics` (when `url_metrics` is set) still answer after the storm |

Inject plan keys: `[inject]` `contract` (required; falls back to
`[scenario] contract`), `service_token`, `spawn`, `scope`/`entity_type`/
`incarnation`/`generation`/`entity_base`, `url_data` (data role endpoint for
the MLD1 leg; without it the late-acks guard is reported as skipped),
`url_metrics` (Prometheus exposer for the post-storm health check),
`storm_rounds` (default 3), `storm_burst` (default 8), `slow_wait_ms`
(default 300), `storm_entity_base` (default 500000), and per-scenario
switches `bad_frames` / `late_acks` / `slow_clients` / `disconnect_storm`
(default all on).

## CI smoke

`mistlane test --case bots.smoke` (suite `cli`) starts a contract-hosting
gameServer and a journal-backed data node, runs a load plan, refuses a
malformed plan, runs the injection plan, and requires every guard to hold
and both nodes to stop cleanly.
