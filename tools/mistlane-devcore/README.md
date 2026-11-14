# mistlane-devcore

IDE-agnostic TypeScript core for mistlane devtools. Zero FFI, zero C++ engine
changes: every source reads a surface the engine already exposes. The VS Code
shell in `../mistlane-vscode` is one consumer; a Visual Studio or other host
would consume this same package unchanged.

## Protocol (mirrored from the engine, not guessed)

| Wire fact | Engine source |
|---|---|
| Frame: `u32 BE length \| payload`, 1 MiB bound | `include/mistlane/transport.hpp` |
| MLS1 hello `MLS1 <role> <instance> <token>` -> `MLS1 OK`, only for tokened nodes | `src/node_internal.hpp`, `src/return_sender.hpp` |
| MLC2 `MLC2` -> `MLC2 OK`; afterwards every payload is `u32 BE request id \| body`, pushes carry id 0 | `src/session_plane.hpp` |
| Entity request `"MLE1" \| envelope(39B BE) \| payload`; reply `"MLE1" \| correlation u64 \| EntityReply` | `include/mistlane/entity_envelope.hpp`, `src/business_plan.hpp` |
| EntityReply: `0x00 \| payload`, or `0x01 \| u8 code` (codes 0..9; only INVALID appends `u16 len \| reason`) | `include/mistlane/entity_envelope.hpp`, `include/mistlane/entity_dispatch.hpp` |

The envelope wire order is scope u32 | type_id u32 | entity_id i64 |
incarnation u32 | side u8 | method_id u16 | correlation_id u64 |
hosting_generation u32 | deadline_ms u32 — entity_id before incarnation on
the wire even though the C++ struct declares incarnation first
(docs/business-bot.md: never positionally initialize the struct).

## Data sources

- **Metrics** (`MetricsPoller`): polls `GET /metrics` on the node's
  `--dashboard-listen` / `--metrics-listen` HTTP server (Prometheus text
  0.0.4, plain GET, no handshake — `src/observability.hpp`).
- **Log** (`FileTailPoller`): file-tail poller with tail -F semantics
  (truncation, rotation, late-appearing files). The engine logs to
  stdout/stderr; the scaffolded ops scripts redirect them into
  `var/log/<instance>.log` (`src/scaffold.cpp`). Socket push is the phase-two
  upgrade and needs no client format change.
- **Process** (`ProcSampler`): CPU ticks and RSS from
  `/proc/<pid>/stat` + `/proc/<pid>/status`.
- **Attach** (`AttachSession`): one facade owning any mix of the three, with
  typed events and per-source status (up/down/waiting/off).

## Usage

```ts
import { AttachSession } from "@mistlane/devcore";

const session = new AttachSession({
  metrics: { baseUrl: "http://127.0.0.1:21002", intervalMs: 1000 },
  log: { path: "var/log/gameServer.log" },
  proc: { pid },
});
session.on("metrics", (event) => render(event.metrics));
session.on("log", (event) => appendLines(event.lines));
session.on("proc", (sample) => plot(sample.cpuPercent, sample.rssBytes));
session.attach();
// ... session.detach();
```

Entity probe over a correlated session:

```ts
const transport = new MistlaneTransport({
  host, port, correlated: true,
  identity: { role: "devtools", instance: "ide", token },
});
await transport.connect();
const reply = await transport.callMethod(
  { scope: 1, typeId: 2, entityId: 9501n, incarnation: 1, hostingGeneration: 1 },
  1, // login
);
```

## Development

```
pnpm --ignore-workspace install
pnpm --ignore-workspace build   # tsc -> dist/
pnpm --ignore-workspace test    # vitest, 69 tests
```

`--ignore-workspace` is required: the repo root has a pnpm workspace that
would otherwise swallow the install. Tests bind fixed high port ranges and
exercise a real TCP fake node plus live file/proc fixtures.
