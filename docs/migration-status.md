# Migration status

This ledger distinguishes architectural ideas carried forward from code that has actually been extracted. It describes the Mistlane 0.1 candidate; it does not claim that the full legacy framework or game server has been migrated.

| Legacy capability | Mistlane status | Current boundary / next decision |
|---|---|---|
| Process startup, listeners, signal handling and shutdown | Reimplemented | `Node` owns the new lifecycle. The old process API and binary compatibility are not retained. |
| Asynchronous TCP transport | Reimplemented | New four-byte length-prefixed payload protocol, per-session limits and deadlines. Old framing and client protocol are unsupported. |
| WebSocket / secure WebSocket | Replaced | Drogon/Trantor handles WS/WSS; no handwritten handshake, masking or control-frame parser is carried over. |
| HTTP server | Replaced | Drogon handles HTTP/HTTPS and Lua request handlers. The old HTTP server API is not retained. |
| HTTP client | Replaced | A bounded synchronous libcurl adapter is provided for C++ callers; it must not block an event-loop callback. |
| Embedded Lua and callbacks | Reimplemented | A module table and explicit init/start/message/http/tick/stop/shutdown hooks replace fixed legacy global callbacks. The VM remains single-threaded. |
| Lua infinite-loop detection | Partially replaced | An instruction-count hook catches common runaway scripts. It is not a hard CPU or memory sandbox. |
| Gateway / router / worker topology | Reimplemented | Generic TCP forwarding and worker execution exist. Account identity, player affinity and old service registration are absent. |
| Cross-server room routing | New minimal implementation | `cross`/`shard` provides deterministic group/room routing, epoch checks and local hard admission. It is not global admission, service discovery or player-state migration. |
| Peer connection registration, reconnect and queued delivery | Not migrated | Current sessions use direct upstream connections. No legacy registration packet, shared multiplexed connection, reconnect policy or durable queue exists. Decide these only against an application requirement. |
| Legacy thread pool | Reuse available | Boost.Asio provides a fixed-size `thread_pool`. Mistlane should use it for blocking work if needed; add admission/backpressure around submissions because a worker pool alone does not define the application's queue limit or shutdown policy. |
| Lua timer queue | Reuse available | Boost.Asio `steady_timer` supplies monotonic asynchronous timers. A Lua timeout/interval adapter may be needed for handles, cancellation, callback errors and VM reload cleanup; do not port the old timer wheel by default. |
| Lua dead-loop monitor | Partially replaced | An instruction-count hook is present; the old monitoring thread and its stop/race behavior are not copied. |
| CPU/memory profiler | Not migrated | Neither the old framework nor Mistlane has CPU sampling, allocation profiling or Lua function-level profiling. The old Lua timeout hook is not a profiler. |
| MySQL access | Replaced at the connection-pool layer | Boost.MySQL `connection_pool` and a probe are present. No game schema, dirty-write cache, SQL helper, or general Lua async database bridge is migrated. |
| Redis client and Lua binding | Replaced/partial | Boost.Redis 1.92 is enabled from vcpkg. Optional `--redis` enables bounded asynchronous Lua Streams APIs and sorted-set leaderboard operations. TLS, generic command access and automated Redis integration coverage remain future work. |
| UDP transport | Reuse available | Boost.Asio already provides asynchronous UDP sockets. Only Mistlane's datagram/session contract and any Lua adapter would be new; confirm a use case before adding that public API. |
| Logging service and Lua logger APIs | Reuse available | Boost.Log provides filters, attributes, formatters and sinks; Drogon has its own logging facility. Select one common C++ logging facade before binding a small structured logger to Lua. Remote aggregation remains an external deployment concern. |
| Metrics, process monitor and system information | Partial | Optional private `/metrics` listener exports readiness, active sessions, admission rejections, Lua callback duration, and RSS. Internal health endpoints, complete traffic/tick metrics, production dashboards and load validation remain. |
| Crash reporting | Partial | Optional Crashpad handler writes to a local database with uploads disabled. Minidump generation, retention policy, symbol packaging and production deployment are not yet validated. |
| Crypto, compression, URL, serialization helpers | Not migrated wholesale | Game-specific helpers and custom protocol utilities are excluded. Use maintained libraries and explicit formats for concrete needs. |
| Generic Lua utility files | Selective extraction | Only MinHeap was reviewed and extracted; other utilities need dependency, behavior, test and provenance review one file at a time. |
| Gameplay, accounts, payments, navigation, cache tables, operations and SDKs | Excluded | These are application/game systems, private deployment concerns or third-party integrations, not framework migration targets. |

## Acceptance meaning

“Reimplemented” means a new implementation exists and has a defined contract; it does not mean wire/API compatibility or production validation. “Replaced” means a maintained dependency owns the protocol/pool implementation, with Mistlane providing only an adapter. “Partially replaced” and “not migrated” are explicit gaps. The historical cross-server overload crash has not been reproduced or diagnosed; local admission controls do not prove a capacity fix.

## Reusable upstream facilities

The current dependency baseline includes Boost 1.92.0, so Asio timers, fixed-size thread pools, UDP sockets, Boost.Log and Boost.Redis are available upstream. Boost.Redis is an Asio-native RESP3 client with reconnect and Sentinel support; Mistlane enables it for optional asynchronous Lua Streams access. Drogon 1.9.14 provides an HTTP worker pool and an optional asynchronous Redis client, which remains disabled to avoid maintaining two clients. Lua-facing lifecycle, request caps and shutdown integration remain adapter responsibilities. Drogon's ORM/MySQL feature stays disabled because the project already uses Boost.MySQL's connection pool.

References: [Boost.Asio thread pool](https://www.boost.org/doc/libs/1_92_0/doc/html/boost_asio/reference/thread_pool.html), [Asio steady timer](https://www.boost.org/doc/libs/1_92_0/doc/html/boost_asio/reference/steady_timer.html), [Asio UDP socket](https://www.boost.org/doc/libs/1_92_0/doc/html/boost_asio/basic_datagram_socket.html), [Boost.Redis](https://www.boost.org/doc/libs/1_92_0/libs/redis/doc/html/redis/index.html), [Boost.Redis Sentinel](https://www.boost.org/doc/libs/1_92_0/libs/redis/doc/html/redis/sentinel.html), [Boost.Log](https://www.boost.org/doc/libs/1_92_0/libs/log/doc/html/log/design.html), and [Drogon Redis support](https://github.com/drogonframework/drogon/wiki/ENG-18-Redis).

## Extraction naming and provenance

Public package paths and filenames use the Mistlane namespace and do not inherit legacy product prefixes. Renaming a path does not change source provenance: reused material and adapted third-party files remain identified in `NOTICE` and `THIRD_PARTY.md`. Do not include legacy source files in the export package merely to document a design reference.
