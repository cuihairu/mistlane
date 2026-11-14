# Mistlane DevTools (VS Code)

Thin VS Code shell over [`tools/mistlane-devcore`](../mistlane-devcore): attach
to a running mistlane node and watch its metrics curves, live log stream, and
entity contract surface. Read-only — the extension never mutates the node, and
the engine needs no changes to be attached to.

## Commands

- **Mistlane: Attach Node** — start every configured source and open the dashboard.
- **Mistlane: Detach** — stop all pollers.
- **Mistlane: Show Dashboard** — reveal the panel.

## Settings

| Setting | Meaning |
|---|---|
| `mistlane.dashboardUrl` | Node `--dashboard-listen` / `--metrics-listen` base URL (default `http://127.0.0.1:21002`) |
| `mistlane.logFile` | Log file to tail (the ops scripts land node output in `var/log/<instance>.log`) |
| `mistlane.pid` / `mistlane.pidFile` | Node process id for CPU/RSS sampling |
| `mistlane.contractPath` | Entity contract `.def` listed in the panel |
| `mistlane.metricsPollMs` / `mistlane.logPollMs` | Poll intervals (default 1000 / 500) |

## Panel

- CPU %, RSS, `mistlane_messages_total`, `mistlane_active_sessions` curves
  (all four from surfaces the node already serves: `/metrics` and `/proc`).
- Live log stream with filter/search: plain text is a case-insensitive
  substring, `/pattern/` a regex, `/pattern/i` with flags.
- Entity contract table (from the `.def`); clicking a row opens the file at
  that method's line.

## Working session example

```
mistlane --role gameServer --listen 127.0.0.1:21001 \
  --script examples/business.lua \
  --contract examples/contracts/business.def \
  --dashboard-listen 127.0.0.1:21002
```

Then set `mistlane.logFile` to wherever the ops script redirects the node's
stdout/stderr, `mistlane.pid` (or a pidfile) to the node process, and run
**Mistlane: Attach Node**.

## Development

```
pnpm --ignore-workspace install
pnpm --ignore-workspace run build      # esbuild bundle -> dist/extension.js
pnpm --ignore-workspace run typecheck  # tsc --noEmit
pnpm --ignore-workspace run test       # vitest (panel model)
pnpm --ignore-workspace run package    # vsce package -> mistlane-vscode-0.0.0.vsix
```

The devcore is imported directly from `../mistlane-devcore/src` and bundled by
esbuild — no published package dependency, no FFI. A Visual Studio or other
host would consume the same devcore unchanged.
