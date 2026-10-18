# Validation record

Validated on 2026-10-17; Redis additions and the HTTP/HTTPS, Lua HTTP worker and native cross/shard checks were verified on 2026-10-18. Environment: Linux x64, GCC 15.2.0, CMake 4.2.3, pinned vcpkg baseline `a5f736224e85b32e25a8ffc52562333180396642`.

| Item | Actual result |
|---|---|
| CMake configure and complete Debug build | Passed with vcpkg-provided Boost 1.92.0, Lua 5.5.1, OpenSSL 3.6.5#2 |
| Three-process TCP chain | Passed gateway → router → worker roundtrips |
| TCP framing | Passed empty, binary, UTF-8, maximum-size, fragmented, coalesced, and oversized request cases |
| Concurrent clients and session routing | Passed concurrent requests and per-session upstream affinity |
| WS / WSS | Passed Drogon handshake, masking, fragmented messages, ping/pong, and CA-trusted TLS echo |
| HTTP / HTTPS | Passed Drogon requests over both transports, binary bodies, and the curl adapter's certificate/hostname verification, HTTP/HTTPS-only protocol restriction and response-size limit |
| Lua HTTP worker | Passed request metadata, before_request short-circuit, after_request mutation, on_error fallback, response validation and reload rollback |
| Native cross/shard | Passed MLC1 handshake, fixed group/room affinity to one shard, final ROOM_FULL admission, lease release on disconnect, EPOCH mismatch and upstream-failure isolation |
| Lua | Passed successful VM reload, invalid/schematic reload rollback, callback errors, typical infinite-loop interruption, and repeated callbacks |
| Extracted heap | Passed module isolation and ordered pops using the actual linked Lua runtime |
| Failure and resource paths | Passed upstream connection failure, connection limit, message-rate limit, invalid CLI arguments, SIGTERM exit |
| Database pool | Passed three acquire/query/return cycles with a pool of size one |
| Query values and TLS | Passed quote/backslash binding and CA/hostname-verified TLS |
| Database authentication error | Passed failure within the configured acquisition deadline |
| Redis Streams and leaderboard | Passed local Redis end-to-end Lua checks for idempotent consumer groups, publish/read, pending claim + ACK, score increment, rank/get, top page and removal |
| Crash drill (minidump) | `mistlane-crash-sample` performs a controlled null dereference through the same local-only pipeline: the crashpad database receives a real minidump (over 4 KiB starting with the `MDMP` magic), the sample exits from `SIGSEGV`, and no handler process is left behind (`mistlane test --case crashpad.minidump`, four consecutive passes) |
| Dump-to-binary correlation | `tools/crash_symbols.py build-id` returned the sample's `NT_GNU_BUILD_ID` (matching `readelf -n`), `modules` listed all seven dump modules with their build ids, `match` paired the drill dump with the exact sample binary (exit 0) and rejected a different build (exit 1); a full `minidump_stackwalk` run is documented but not executed locally because the vcpkg crashpad port ships only `crashpad_handler` |
| Traffic and tick metrics | `transport.tcp` scrapes the live `/metrics` exposition and asserts nonzero rx/tx message counters, tx payload bytes above 1 MiB, request-latency histogram observations, event-loop tick-lag histogram plus running maximum (after waiting for the first scheduled tick), zero upstream errors/timeouts on a healthy worker, and at least one upstream error on a gateway pointing at a dead port |
| Internal probe (PROBE1) | `probe.control` drives the protocol over raw TCP: liveness/version/command/malformed/nonce error paths, readiness lines with `active`/`capacity`/`lag_ms`, idle-connection deadline, saturation-driven `ready -> notready -> ready` with hysteresis (`--overload-ratio 0.5 --ready-hysteresis 2`) mirrored by the `mistlane_ready` gauge, `err auth` under `MISTLANE_PROBE_TOKEN`, cross gating via `IP:PORT:PROBE` (`MLC1 ERR NOT_READY` on explicit notready, `MLC1 ERR UPSTREAM` on a dead probe port, ungated upstreams without a probe port, established rooms unaffected) with rejected joins counted in `mistlane_admission_rejections_total`, 50 sequential probe lifetimes, a 300-byte request line closed without a reply and the 65th of 64 held probe connections rejected, and bracketed IPv6 (`[::1]`) listen/probe/upstream addresses both gated and ungated |
| Native config file | `config.file` boots a worker from a `# mistlane-config 1` file (role, listen, script resolved relative to the file, transport, admission limit), rejects a missing version line, unknown keys and duplicate keys with file:line errors, applies `MISTLANE_MAX_CONNECTIONS` over the file and an explicit flag over both, honors `ROOT=` for project-root-relative entries, and enables `PROBE_LISTEN`/`METRICS_LISTEN` from the file |
| Documentation | VitePress 1.6.4 with patched Vite 6.4.4 built both locales using pnpm 10.33.0; all 10 pages have the expected language/navigation/repository links |
| Documentation dependency audit | pnpm audit reported zero advisories after the Vite override |
| Source package | Allowlist and SHA256 checksum verification passed; no original game directories or Git history |

Decision record (self-decided 2026-10-18): the VitePress build's "large chunk" warnings caused by Mermaid's lazy-loaded bundles were evaluated. The single lazy-loaded chunk layout is kept without splitting; the warnings do not fail the build. Revisit only if a future bundle audit shows a real load-time cost.

The database check used a disposable **MariaDB 11.8.6** instance. This verifies the adapter against MariaDB; it is not an Oracle MySQL server compatibility matrix. The test created its own temporary data directory, random credentials, port, and certificate, and cleaned them up after completion.

Commands:

```bash
cmake --preset linux-debug -DMISTLANE_TEST_LOCAL_DATABASE=ON -DMISTLANE_TEST_LOCAL_REDIS=ON
cmake --build --preset linux-debug
ctest --preset linux-debug -R '^mistlane_integration$'
python3 tests/mysql_integration.py ./build/debug/mistlane-mysql-probe
pnpm install --frozen-lockfile
pnpm docs:build
python3 tools/export.py
```

The optional database and Redis checks are registered as `mistlane_mysql_integration` and `mistlane_redis_integration` when enabled. The Redis integration test starts a disposable local Redis server and covers duplicate consumer-group creation, reconnect after the server is killed and restarted on the same port, a stream callback left pending across a Lua hot reload (the callback owned by the replaced VM must be dropped), and a TLS-only Redis server (a matching CA and name are accepted; a wrong CA and a missing IP name are rejected); a full CTest run then executes all enabled checks.

During dependency setup, the environment's temporary filesystem refused a large OpenSSL archive. Rebuilding the vcpkg working tree on the regular disk resolved it. No port source patches or dependency downgrades were introduced for this environment issue.

The install rules were smoke-checked on 2026-10-18: `cmake --install` into a temporary prefix placed `mistlane` and `crashpad_handler` in `bin/`, and the installed handler passed the crashpad startup suite against the freshly built server. Both binaries depend only on the system C/C++ runtime.

Not validated: the original game cluster and business behavior, production load capacity, soak stability, other platforms, full game migration, Oracle MySQL versions. Hosted CI status is available in [GitHub Actions](https://github.com/cuihairu/mistlane/actions). These results support research and learning; they do not establish production readiness.
