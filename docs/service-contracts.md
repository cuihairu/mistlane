# login, data and apiServer service contracts

Status: contract design (2026-10-19); implementation as noted per section. This page defines what the `login`, `data` and `apiServer` roles speak and who connects to them, feeding the cross return protocol (which needed a home-side endpoint decision) and the remaining migration items. Responsibilities stay those of the [process roles](./process-roles.md); ownership rules are the [cross return protocol's](./cross-return-protocol.md).

## Decisions this page makes

- **The home-side endpoint for MLD1 is the `data` role itself.** A home-region gateway may front it for transport, but the durable-ack authority is the data service — not a coordinator, not a gameServer. This closes the open question on the return-protocol page.
- **All three roles reuse the existing runtime surface** — framed TCP sessions with optional `MLS1` identity, the Lua `message`/`http` hooks, `MLR1` registry access, and the existing HTTP stack for apiServer. No new transport layer is introduced for them.
- **login and data run as plain script services** (`--role login`/`--role data` start with `--script`, the standard hooks and framed sessions, like a gameServer without scenes). Login's placement lookup, the data-side MLD1 record endpoint, the entry-ticket C++ primitives and the ticket's `MLC1` wire form are implemented; host-side ticket verification with secret configuration, the durable load and the persistence flows remain contract-only. apiServer is already running and gains contract documentation, not new code.
- **The entry credential between services is a short-lived HMAC-SHA256 ticket** (decided 2026-10-19 under the standing 2026-10-14 authorization): after its own credential check, login issues one ticket over (realm, character, target instance, hosting generation, expiry, nonce) keyed by a per-realm pre-shared secret from config, and the receiving gameServer/crossServer verifies it before accepting an entry or transfer — so a host never loads a character on a bare claim. Rejected: per-client PKI/mTLS (needs CA and rotation operations this deployment does not run), an external JWT/auth service (a new process and dependency on the entry hot path), and no framework ticket at all (hosts would trust unvalidated claims, breaking the single-writer rule). The C++ primitives exist as MLT1 (`include/mistlane/entry_ticket.hpp`): issue and verify over the canonical bytes with the shared `hmac_sha256`, a per-realm secret map, identity checks against the arriving character/instance/generation, inclusive expiry, fail-closed clock rollback and bounded seen-claims — plus the wire form (`entry_ticket_hex`/`parse_entry_ticket`: hex of canonical bytes plus raw signature, carried as the optional `MLC1` `tk` tail group, bound to the join's character and hosting generation) — but no real secret, no secret config loader, no host that runs the verifier, and the claim table is per process, so `Replay` is honest only for one receiver. `MLS1` pre-shared tokens stay the service-leg identity.

## login

login owns the entry flow: verify the account, present the character list, obtain a placement and hand the player to a gameServer. It is client-facing and never holds a writable character.

| Aspect | Contract |
|---|---|
| Transport | Framed TCP (or WS via a gateway) like any session role; `MLS1` identity on internal legs when tokens are configured |
| Peers | Clients and gateways inbound; coordinator registry outbound (`MLR1 QUERY`/`MLR1 PLACE` as a registry client); data outbound for character lists/saves it must fetch before entry |
| Lua surface | Same `message`/`init`/`start`/`tick`/`stop`/`shutdown` hooks; a login handler answers auth, charlist and entry requests; no scene or Avatar objects |
| Placement | login asks the coordinator `MLR1 PLACE gameServer [NS <namespace>] <scope>` and returns the placed address in the entry response; it must not embed addresses in durable records (responsibility rule), and an `MLR1 EMPTY` answer is an entry refusal, not an error to swallow |
| Entry context | The entry response carries origin realm, character identity, home-data-issued hosting generation and return route, plus the login-issued ticket; gameServer and later crossServer use the same participant context |
| Auth boundary | Credential verification is an application Lua decision; between services the entry credential is the decided HMAC-SHA256 short-lived ticket, whose C++ issue/verify primitives are implemented as MLT1 (see the decisions above). The wire form is bound: the ticket rides the `MLC1` `tk` tail and an undecodable tail refuses the whole join. Nothing verifies it yet: no host runs the verifier, so a host still cannot validate an entry context beyond transport identity |
| State | login holds at most connection state and pending-placement records; nothing durable. After the entry response the character's route belongs to gameServer/coordinator, not login |

Generation lifecycle (design, not runtime): after placement selects a target instance, login requests a grant from home data for `(realm, character, target instance)`. Home data is the sole generation authority: a fresh key starts at 1; reassignment or takeover durably allocates a strictly greater generation, fences/tombstones the prior grant and activates the destination in the [ordered lease handoff](./cross-return-protocol.md#writer-lease-handoff). Allocated generations are never reused after abort or timeout. Login returns that data-issued generation in the entry context and binds it into the ticket alongside the target instance; the receiving host verifies the ticket and the active home-data grant before durable writes. Hosts cannot choose or advance their own generation. Current MLD1 LOAD only observes the in-memory record, and SUBMIT's acceptance of a higher number is not a lease grant. Grant/reservation/activation wire messages, durable authority and host-side ticket verification (per-realm secret configuration included) still need implementation under the ownership gap; the ticket's encoding and wire form exist as the `MLC1` `tk` tail.

Wiring status: the role starts as a script service (`--role login` with `--script`, framed sessions and the standard hooks), and `--coordinator ADDR:PORT [--namespace NAME] [--scope NAME]` wires the registry client: Lua `mistlane.place(role, namespace?, scope?, callback)` asks `MLR1 PLACE` per lookup over a fresh connection and hands the placed address (or a reason) back to the script. The entry-ticket C++ primitives are implemented as MLT1 (issue/verify over canonical bytes, per-realm secrets, identity/expiry/replay checks), the ticket rides the `MLC1` `tk` tail, and a crossServer started with `MISTLANE_TICKET_SECRETS` (per-realm secret rows, environment-injected like the MLA1 credentials) verifies the join's ticket — a failed verdict answers `MLC1 ERR TICKET` and the script reads the verdict as `context.verified`; claims stay per process and volatile. Not implemented either: a login-side issuance path (the Lua sandbox opens no crypto libraries) and the full entry flow around placement.

## data

data owns the durable character store of its home region: the only writer of record, the MLD1 endpoint, and the load path that fills a gameServer or login request.

| Aspect | Contract |
|---|---|
| Transport | Framed TCP with `MLS1` identity expected on every leg (members present `--upstream-token`); a gateway may front it, but clients of record are services, never player devices |
| Peers | gameServer/crossServer inbound (loads, MLD1 submissions, saves); login inbound (character lists); nothing outbound except the database layer |
| Durable authority | data acks MLD1 `SUBMIT` with `ACK durable` only after its own write is durable; generation fencing and per-character sequence dedup are data-side responsibilities (see the return protocol) |
| Load path | A load request returns the character's durable state plus its current hosting generation; a generation the requester already holds lets the home side reject stale claims |
| Store backend | `--journal FILE` enables the data-side SQLite record store (same boundary as the coordinator migration journal). With it data answers the committed `ACK durable` only after the payload/sequence row is durable; without it data must not send `ACK durable` at all, so senders stay in their retry/read-only state — an honest degradation, not a shortcut |
| Excluded | Business schemas, SQL, cache-table semantics; the storage engine and dirty-tracking belong to the entity-contract chain |

Wiring status: the role shell starts (`--role data` with `--script`), `--service-token` enforces `MLS1` identity on every leg, and the MLD1 endpoint now runs (`MLD1 LOAD`/`SUBMIT` over framed TCP): the generation fence, the per-character sequence discipline, resend dedup and the store-backed `ACK durable` are implemented in the framework. A store-backed data node answers `ACK durable` only after committing the record row, replays the committed ACK for a resend after restart and refuses a failed commit with `MLD1 ERR STORAGE`; without `--journal` it answers `MLD1 RECEIVED` and keeps no durability claim, exactly as the protocol requires. Still not implemented: the crossServer sender-side retry wiring to the ACK, the home final-sequence/tombstone authority and the Avatar runtime (the record store alone is the data persistence chain, not the runtime).

## apiServer

apiServer is implemented and keeps its shape; this section records its contract boundary.

| Aspect | Contract |
|---|---|
| Transport | HTTP/HTTPS by default; optional WS/WSS message listeners per the existing listener model |
| Peers | Platform/operator systems inbound. It is not on the player path and must not be a hidden game-message router |
| Lua surface | `http(request)` with `before_request`/`after_request`/`on_error` hooks; WS lifecycle hooks on upgraded listeners |
| Identity | Trusted-network today; `MLS1` does not apply to HTTP. The platform-callback authentication is the typed `MLA1` contract (canonical bytes + `HMAC-SHA256` + replay window, [protocol analysis](./protocol-analysis.md#external-api-surface-first-slice)) and rides the HTTP stack as a native `POST /mla1` intercept with env-only credential injection; rotation/revocation are decided and tested (a credential row's optional `valid_until`); the durable acceptance backend and the home-facts source are not. ||
| Idempotency | The typed acceptance semantics are fixed: `(audience, principal, realm, character, idempotency_key)` create-or-read distinguishing `Inserted`/`Existing`/`Conflict`, with a read-only ownership lookup. Live handlers are still request-scoped until wired; the durable acceptance backend remains unimplemented |
| Addressing | Reaching a player goes through the character's home services, never a pinned process address; apiServer has no registry membership and makes no placement decisions |

## What is deliberately not here

- Client-facing credential formats and account systems: application domain, deliberately outside the framework. The service-side ticket scheme itself is decided (HMAC-SHA256 short-lived ticket, per-realm pre-shared secret) and its C++ primitives exist as MLT1; what is not here is the wiring — host-side verification with secret injection, and the Lua sandbox still opens no crypto libraries.
- Character state schema and dirty semantics: the entity-contract chain.
- Settlement and reward rules: gameplay.

Legacy evidence and target boundaries: [players, scenes and cross-region execution](./player-scene-model.md); return protocol: [cross return protocol](./cross-return-protocol.md).
