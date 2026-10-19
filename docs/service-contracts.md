# login, data and apiServer service contracts

Status: contract design (2026-10-19); implementation as noted per section. This page defines what the `login`, `data` and `apiServer` roles speak and who connects to them, feeding the cross return protocol (which needed a home-side endpoint decision) and the remaining migration items. Responsibilities stay those of the [process roles](./process-roles.md); ownership rules are the [cross return protocol's](./cross-return-protocol.md).

## Decisions this page makes

- **The home-side endpoint for MLD1 is the `data` role itself.** A home-region gateway may front it for transport, but the durable-ack authority is the data service — not a coordinator, not a gameServer. This closes the open question on the return-protocol page.
- **All three roles reuse the existing runtime surface** — framed TCP sessions with optional `MLS1` identity, the Lua `message`/`http` hooks, `MLR1` registry access, and the existing HTTP stack for apiServer. No new transport layer is introduced for them.
- **login and data run as plain script services** (`--role login`/`--role data` start with `--script`, the standard hooks and framed sessions, like a gameServer without scenes). Login's placement lookup and the data-side MLD1 record endpoint are implemented; the authentication, durable load and persistence flows remain contract-only. apiServer is already running and gains contract documentation, not new code.
- **The entry credential between services is a short-lived HMAC-SHA256 ticket** (decided 2026-10-19 under the standing 2026-10-14 authorization): after its own credential check, login issues one ticket over (realm, character, target instance, expiry, nonce) keyed by a per-realm pre-shared secret from config, and the receiving gameServer/crossServer verifies it before accepting an entry or transfer — so a host never loads a character on a bare claim. Rejected: per-client PKI/mTLS (needs CA and rotation operations this deployment does not run), an external JWT/auth service (a new process and dependency on the entry hot path), and no framework ticket at all (hosts would trust unvalidated claims, breaking the single-writer rule). `MLS1` pre-shared tokens stay the service-leg identity; the sandbox opens no crypto libraries, so issue/verify are future C++ primitives and nothing is implemented yet.

## login

login owns the entry flow: verify the account, present the character list, obtain a placement and hand the player to a gameServer. It is client-facing and never holds a writable character.

| Aspect | Contract |
|---|---|
| Transport | Framed TCP (or WS via a gateway) like any session role; `MLS1` identity on internal legs when tokens are configured |
| Peers | Clients and gateways inbound; coordinator registry outbound (`MLR1 QUERY`/`MLR1 PLACE` as a registry client); data outbound for character lists/saves it must fetch before entry |
| Lua surface | Same `message`/`init`/`start`/`tick`/`stop`/`shutdown` hooks; a login handler answers auth, charlist and entry requests; no scene or Avatar objects |
| Placement | login asks the coordinator `MLR1 PLACE gameServer <scope>` and returns the placed address in the entry response; it must not embed addresses in durable records (responsibility rule), and an `MLR1 EMPTY` answer is an entry refusal, not an error to swallow |
| Entry context | The entry response carries origin realm, character identity and return route — the same participant fields the cross return protocol defines — so gameServer and later crossServer speak one context shape |
| Auth boundary | Credential verification is an application Lua decision; between services the entry credential is the decided HMAC-SHA256 short-lived ticket (see the decisions above) — not implemented yet, so today a host still cannot validate an entry context beyond transport identity |
| State | login holds at most connection state and pending-placement records; nothing durable. After the entry response the character's route belongs to gameServer/coordinator, not login |

Wiring status: the role starts as a script service (`--role login` with `--script`, framed sessions and the standard hooks), and `--coordinator ADDR:PORT [--scope NAME]` wires the registry client: Lua `mistlane.place(role, scope?, callback)` asks `MLR1 PLACE` per lookup over a fresh connection and hands the placed address (or a reason) back to the script. The entry-ticket scheme is decided (HMAC-SHA256, per-realm pre-shared secret — see the decisions above) but not implemented: it waits on C++ crypto primitives, since the Lua sandbox opens no crypto libraries. Not implemented either: the full entry flow around placement.

## data

data owns the durable character store of its home region: the only writer of record, the MLD1 endpoint, and the load path that fills a gameServer or login request.

| Aspect | Contract |
|---|---|
| Transport | Framed TCP with `MLS1` identity expected on every leg (members present `--upstream-token`); a gateway may front it, but clients of record are services, never player devices |
| Peers | gameServer/crossServer inbound (loads, MLD1 submissions, saves); login inbound (character lists); nothing outbound except the database layer |
| Durable authority | data acks MLD1 `SUBMIT` with `ACK durable` only after its own write is durable; generation fencing and per-character sequence dedup are data-side responsibilities (see the return protocol) |
| Load path | A load request returns the character's durable state plus its current hosting generation; a generation the requester already holds lets the home side reject stale claims |
| Until persistence lands | The data-side persistence chain (entity-contract item) is not implemented. Until it exists, data cannot emit truthful durable acks; an interim implementation must not send `ACK durable` at all, so senders stay in their retry/read-only state — an honest degradation, not a shortcut |
| Excluded | Business schemas, SQL, cache-table semantics; the storage engine and dirty-tracking belong to the entity-contract chain |

Wiring status: the role shell starts (`--role data` with `--script`), `--service-token` enforces `MLS1` identity on every leg, and the MLD1 endpoint now runs (`MLD1 LOAD`/`SUBMIT` over framed TCP): the generation fence, the per-character sequence discipline and resend dedup are implemented in the framework, and the endpoint answers `MLD1 RECEIVED` — a receipt, never a durability claim, so senders keep their retry queue exactly as the protocol requires. Still not implemented: the persistence chain, therefore no `ACK durable` anywhere today and records are process-local (a restart answers `MLD1 EMPTY`); the durable load path answers from the same in-memory record and no further.

## apiServer

apiServer is implemented and keeps its shape; this section records its contract boundary.

| Aspect | Contract |
|---|---|
| Transport | HTTP/HTTPS by default; optional WS/WSS message listeners per the existing listener model |
| Peers | Platform/operator systems inbound. It is not on the player path and must not be a hidden game-message router |
| Lua surface | `http(request)` with `before_request`/`after_request`/`on_error` hooks; WS lifecycle hooks on upgraded listeners |
| Identity | Trusted-network today; `MLS1` does not apply to HTTP. Platform callbacks need an authentication decision (signing/secret) before production use — unchanged by this page |
| Idempotency | Persistent, idempotent callback acceptance (the platform-callback item) remains unimplemented; handlers today are request-scoped |
| Addressing | Reaching a player goes through the character's home services, never a pinned process address; apiServer has no registry membership and makes no placement decisions |

## What is deliberately not here

- Client-facing credential formats and account systems: application domain, deliberately outside the framework. The service-side ticket scheme itself is decided (HMAC-SHA256 short-lived ticket, per-realm pre-shared secret); what is not here is its implementation — gated on C++ crypto primitives, since the Lua sandbox opens no crypto libraries.
- Character state schema and dirty semantics: the entity-contract chain.
- Settlement and reward rules: gameplay.

Legacy evidence and target boundaries: [players, scenes and cross-region execution](./player-scene-model.md); return protocol: [cross return protocol](./cross-return-protocol.md).
