# Hook registry: the single lifecycle hook surface

Status: design note (2026-11-22). This page is the human-readable face of `contracts/hooks.xml`, the single registry every lifecycle hook lands in. The registry closes the vocabulary (unknown elements or values are errors, not defaults), gives every hook a home in the [avatar lifecycle oracle](./avatar-lifecycle.md) (an object-level hook names an oracle state, a process-level hook names a declared trigger), and checks the mapping onto the [avatar runtime's](./avatar-runtime.md) Lua hook surface is complete in both directions. `tools/check_hooks.py` is the validator; it passes on the registry as committed. Nothing here changes CLI behavior, and no hook is implemented: this page and the registry fix names, arguments, timing, failure semantics and placement, not the code.

## The registry

| Hook | Level | Home (oracle state or trigger) | Failure | Returns | Lua surface |
|---|---|---|---|---|---|
| `avatar_load` | object | `loading` | refuse | avatar-table | `avatar_load` |
| `avatar_enter` | object | `active` | refuse | none | `avatar_enter` |
| `avatar_leave` | object | `active`, `suspended` | logged | none | `avatar_leave` |
| `avatar_tick` | object | `active`, `visiting` | logged | none | `avatar_tick` |
| `avatar_save` | object | `active`, `suspended`, `reconnecting`, `visiting`, `closing` | abort | state-table | `avatar_save` |
| `avatar_transfer_out` | object | `transferring` | abort | snapshot | `avatar_transfer_out` |
| `avatar_transfer_in` | object | `transferring`, `visiting` | abort | none | `avatar_transfer_in` |
| `avatar_detach` | object | `suspended` | logged | none | `avatar_detach` |
| `avatar_fail` | object | `loading`, `active`, `suspended`, `reconnecting`, `transferring`, `visiting`, `closing` | logged | none | `avatar_fail` |
| `onEnterWorld` | object | `active`, `visiting` | logged | none | — |
| `onEnterAOI` | object | `active`, `visiting` | logged | none | — |
| `onLeaveAOI` | object | `active`, `visiting`, `closing` | logged | none | — |
| `onDailyReset` | process | trigger `day-boundary` | abort | none | — |
| `onAvatarDailyReset` | object | `active`, `visiting` | abort | none | — |
| `onCrossVisitAdmit` | object | `loading` | refuse | verdict | — |
| `onCrossVisitStart` | object | `visiting` | abort | none | — |
| `onCrossVisitEnd` | object | `transferring`, `closed`, `purged` | logged | none | — |
| `onLeaseTaken` | object | `visiting` | abort | none | — |
| `onLeaseLost` | object | `visiting`, `closed`, `purged` | logged | none | — |

## The Lua mapping

The registry carries the mapping as a `lua` attribute; the table above is its human form. The avatar runtime's Lua surface is nine hooks, and each maps to exactly one registry entry (and no registry entry claims a name outside that set), so the mapping is complete both ways:

| Lua surface hook | Registry entry | Shared contract |
|---|---|---|
| `avatar_load` | `avatar_load` | Creates the avatar; a failed load publishes nothing |
| `avatar_enter` | `avatar_enter` | A session attaches; refusal is explicit |
| `avatar_leave` | `avatar_leave` | A session detaches; never blocks |
| `avatar_tick` | `avatar_tick` | Node tick, live avatars only |
| `avatar_save` | `avatar_save` | The only forced-save entry |
| `avatar_transfer_out` | `avatar_transfer_out` | Source snapshot; a failure returns the avatar |
| `avatar_transfer_in` | `avatar_transfer_in` | Destination takes over; a failure keeps the source |
| `avatar_detach` | `avatar_detach` | Disconnect; the avatar goes suspended |
| `avatar_fail` | `avatar_fail` | Any hook failed or timed out |

## Why the registry exists

Before it, hook names were scattered across the lifecycle pages and the Lua surface, and nothing guaranteed a hook had a place in the state machine. The registry's two rules are what the validator enforces, and they are the acceptance:

- **No orphan hook.** Every object-level hook names at least one avatar lifecycle state it is legal in; every process-level hook names a declared trigger. A hook with neither is refused by the validator.
- **The mapping is total.** Every Lua surface hook is registered, and every registry entry that maps to the Lua surface names a real surface hook.

Both are checked by `tools/check_hooks.py`, which runs against `contracts/hooks.xml` and prints a one-line verdict. It is a standalone script with no new dependency, so wiring it in is a tooling decision, not a code change — the minimum-intrusion rule this slice follows.

## Relationship and errata

| Concept here | Baseline page | Relation |
|---|---|---|
| The nine-hook Lua surface and its failure table | [avatar runtime, Lua hook surface](./avatar-runtime.md) | Mapped one-to-one; the registry adds placement and thread, changes nothing |
| `suspended`/`reconnecting` refinement and the reset hooks | [reconnect and daily reset](./reconnect-and-daily-reset.md) | `onDailyReset`/`onAvatarDailyReset` registered with the same ordering and failure semantics |
| Cross-visit hooks and lease surface | [cross-server lifecycle](./cross-server-lifecycle.md) | Registered with the same call guarantees |
| AOI edges | [avatar lifecycle, segments 2 and 3](./avatar-lifecycle.md) | `onEnterWorld`/`onEnterAOI`/`onLeaveAOI` registered against `active`/`visiting` |
| Authored-contract XML idiom | [entity contracts and generation](./entity-contract.md) | The registry follows the same closed-vocabulary, explicit-id idiom as the contract chain |

No conflict was found: every registered hook maps to a state or edge the lifecycle pages already name, and no hook renames or contradicts a baseline statement.

## Deliberately not decided here

- The implementation of any hook, and the binding of the registry into a loader: later implementation slices.
- A generated output from the registry (descriptors, a Lua stubs file): a tooling slice; the validator is the only script here.
- Whether `avatar_fail` may run during a VM replacement: the avatar runtime's best-effort rule stands.
- The `reason` vocabulary for `avatar_fail` and the `verdict` vocabulary for `onCrossVisitAdmit`: the closed sets on the [avatar runtime](./avatar-runtime.md) and [cross-server lifecycle](./cross-server-lifecycle.md) pages.

## Verification

The registry passes `tools/check_hooks.py` as committed: 19 hooks registered, 9 mapped onto the Lua surface, vocabulary closed, no orphan hooks. There is no runtime acceptance — no hook is implemented — so validation records must not claim hook behavior. The page-level check is the validator plus the relationship table above tracing every hook to the page that defines its semantics.

Related: [avatar lifecycle and oracle](./avatar-lifecycle.md), [avatar runtime](./avatar-runtime.md), [reconnect and daily reset](./reconnect-and-daily-reset.md), [cross-server lifecycle](./cross-server-lifecycle.md), [spawn and destroy runtime contract](./spawn-destroy-contract.md).
