# Business-semantic bot and embedded dashboard

Verified against the tree, not invented: every frame below is the one the
server source actually parses (`src/node.cpp`, `include/mistlane/entity_envelope.hpp`,
`include/mistlane/entity_endpoint.hpp`, `include/mistlane/entity_codec.hpp`).

## Wire forensics: what a business frame really is

Transport framing (all TCP roles): `u32 BE length | payload`, payload bounded
by 1 MiB (`max_frame`).

Handshakes on a session (before the first business payload routes):

| frame | direction | meaning |
|---|---|---|
| `MLS1 <role> <instance> <token>` → `MLS1 OK` | either side | service identity; a node started with `--service-token T` accepts **only** MLS1 peers and the accepted peer's frames are attributed `caller = server` |
| `MLC2` → `MLC2 OK` | client→server | correlation mode: every later frame is `u32 BE request id | payload`, replies echo the id, pushes carry id 0 |

Business payload kind (entity runtime, hosted by `gameServer/crossServer
--contract FILE.def`):

```
request:  "MLE1" | envelope (39 bytes, big-endian) | sproto-typed arguments
reply:    "MLE1" | correlation u64 BE | EntityReply
```

Envelope wire order (note: the C++ `EntityEnvelope` struct declares
`incarnation` *before* `entity_id`; the wire carries `entity_id` first —
`encode_entity_envelope` does the mapping, so never positionally initialize
the struct):

| offset | field | width |
|---|---|---|
| 0 | `MLE1` marker | 4 |
| 4 | scope | u32 |
| 8 | type_id | u32 |
| 12 | entity_id | i64 |
| 16 | incarnation | u32 |
| 20 | side (0 client, 1 server) | u8 |
| 21 | method_id | u16 |
| 23 | correlation_id | u64 |
| 31 | hosting_generation | u32 |
| 35 | deadline_ms | u32 |
| 39 | payload | rest |

`EntityReply`: `0x00 | returns payload`, or `0x01 | error code` (`0x01` kind +
code `0..9`; only `INVALID` appends `u16 len | reason`). `correlation_id == 0`
is a notify: no reply exists. A `correlation_id` seen twice at one entity
address is `CONFLICT`.

Arguments and returns ride the schema-driven sproto codec (`SprotoCodec`,
byte-compatible with the generated JS/C# SDKs): little-endian tag block, then
the field data (in-band integer words for small values, length-prefixed
strings/bytes/structs).

Caller attribution is transport-side, never in the envelope: an MLS1-identified
(or mux-internal) peer invokes as `caller = server`; a plain client leg is
`caller = client` and stays `FORBIDDEN` on `caller="server"` methods until
owner attribution ships. That is why the scenario bot presents MLS1.

Entity lifecycle: entities are addressed only after they are routed. The
script seeds the route table with `mistlane.entity.spawn(scope, type_id,
incarnation, entity_id [, generation])`; a request to an unseeded address is
`UNROUTED`, a wrong `hosting_generation` is `STALE`.

## Scenario bot (`mistlane bot --scenario business`)

New flags (all opt-in; without `--scenario` the bot keeps its echo/expect
behavior byte-for-byte — `cli.bot` covers that path in ctest):

```
mistlane bot --url tcp://HOST:PORT --scenario business --contract FILE.def \
  [--service-token T] [--spawn] [--scope N --entity-type N --incarnation N
  --generation N] [--entity-base N] [--rooms N] [--heartbeat-every N]
  [--ramp-ms N] [--clients N] [--count N] [--interval-ms N] [--timeout-ms N]
```

Per connection, serial request/response like the echo mode:

1. optional `MLS1 <bot> bot-<c> <token>` handshake (expects `MLS1 OK`),
2. optional `spawn <entity-base+c>` text frame (expects `spawned`; the host
   script routes the entity),
3. `login` (token `bot-<c>`), `joinRoom` (`1 + c % rooms`),
4. `count` room steps: every `heartbeat-every`-th is a `heartbeat`, the rest
   are `move` (x = step, y = connection), each answered and validated
   (correlation echo + OK reply kind).

`--ramp-ms` staggers the connection starts across the window (the KBEngine
`defaultAddBots` ramp trio in its minimal form: total × step size × delay).
Output adds `scenario=business logins_ok= joins_ok= moves_ok= heartbeats_ok=`;
any validation failure fails the connection and the run exits nonzero.

Reference host files ship in `examples/`: `contracts/business.def`
(Player entity: login/joinRoom/move/heartbeat/leaveRoom, self-contained so bot
and node load the same document) and `business.lua` (spawn/rooms message
handlers plus the entity implementation table, publishing room occupancy as
the `rooms` script gauge).

## Embedded dashboard (`--dashboard-listen IP:PORT`)

A read-only listener serving:

- `GET /` or `/dashboard` — the single-page dashboard (`include/mistlane/dashboard.hpp`,
  inlined in the binary; zero external resources, no CORS, no build step),
- `GET /metrics` — the same Prometheus registry text the `--metrics-listen`
  exposer serves (`Diagnostics::metrics_text()`), so the page and a scrape can
  never disagree,
- anything else — 404. Connections are bounded in count (64), request bytes
  (one 2 KiB line) and time (5 s), like the probe server.

Panels: connections (`mistlane_active_sessions`, sparkline), rooms
(`mistlane_script_gauge{name="rooms"}` — script-declared business state via
`mistlane.metrics.gauge(name, value)`), message throughput (rate of
`mistlane_messages_total` tx/rx), request latency (`mistlane_request_seconds`
sum/count), event-loop lag + RSS, and entity verdicts
(`mistlane_entity_results_total`). Refresh every 2 s.

The script gauge API is wired through `ScriptMetricsSink` (implemented by
`Diagnostics`, family `mistlane_script_gauge`, at most 32 distinct names).
The registry is built when either `--metrics-listen` or `--dashboard-listen`
is set, so a dashboard-only node still serves the full metric set.

## Verification

`mistlane test --case business.bot` (ctest: `mistlane_business_bot`, suite
`cli`) drives the whole chain against real child processes: a contract-hosting
gameServer with service token, metrics and dashboard listeners; a ramping
4-connection bot; page/404 checks; and consistency assertions — dashboard
`/metrics` equals the exposer's readiness line, `rooms` gauge equals 2,
entity `ok` verdicts equal 32, and `mistlane_messages_total` rx equals tx
equals 40 (every frame read produced exactly one reply).
