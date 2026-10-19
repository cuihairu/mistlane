# Release notes (draft)

Status: **draft only.** This file holds the content required for the first public source candidate (ledger TODO.md items 130/131). The version number and the actual GitHub release are decided and triggered by the maintainer only — nothing in this repository tags, publishes or releases on its own.

## Purpose

Mistlane is published for **research and learning**. It is a C++20 / Lua multi-process game-server framework extracted and reorganized from a legacy game backend to study service separation, asynchronous networking, Lua embedding and dependency modernization. It is not a product, not a hosted service and not production software.

## Background and business stripping

The source material is a legacy game cluster. Everything business-specific was deliberately left behind:

- Original gameplay rules, content, configurations, assets and art are not included and are not reproduced.
- Business protocol payloads, database schemas and account/charging flows were stripped; only generic transport, session, placement, identity, record and persistence contracts remain.
- Names of roles and processes describe responsibilities (gateway, login, coordinator, gameServer, crossServer, data, apiServer), not the legacy deployment.
- Legacy evidence quoted in the documentation cites the original tree for analysis only; no legacy code is carried into this repository.

## What works today

See [validation records](./docs/validation.md) for the tested list: framed TCP/WS/WSS transport with bounded pushes, request correlation, service identity and upstream multiplexing, coordinator registry and placement lookups, character route records (`MLR1 ROUTE`), the MLD1 record endpoint on the data role (receipts only), the entity-contract definition-time toolchain (`mistlane contract check`/`build` emitting XSD, release lock, digest, deterministic sproto schema and C++ binding descriptors) validated against the vendored upstream cloudwu/sproto codec with standard wire vectors and Lua round-trips, an in-process entity dispatch/router and RPC lifecycle harness (no networked entity runtime), Lua module lifecycle with hot reload, HTTP/HTTPS apiServer with WebSocket lifecycle hooks, Redis Streams/leaderboard integration and crashpad startup.

## What is explicitly not here

- **No durable character persistence chain**: the MLD1 endpoint answers receipts, never `ACK durable`; records are process-local and a restart answers empty.
- **No networked Avatar/scene/entity runtime**: an in-process entity dispatcher, router and RPC lifecycle harness exist as contract test fixtures only — nothing is wired to transport, sessions or persistence; no Avatar, scene or map types exist, and the lifecycle, transfer and big-map designs remain documents.
- **No crossServer sender**: the bounded retry queue, read-only transition and return completion wait on the persistence chain.
- **No authentication**: login credential formats, ticket signing and platform-callback authentication are undesigned; identity today is trusted-network plus the optional `MLS1` shared token (plaintext comparison, not mTLS).
- **No spatial AOI, chunk streaming or seamless partitioning**: requirements only, see the [big map](./docs/big-map.md) page.

## Unvalidated boundaries

Per the validation records, the following are **not** validated: the original game cluster and business behavior, production load capacity, soak stability, platforms other than the tested Linux environment, full game migration, and Oracle MySQL versions. These results support research and learning; they do not establish production readiness.

## License

Apache-2.0. See [LICENSE](./LICENSE) and [NOTICE](./NOTICE).
