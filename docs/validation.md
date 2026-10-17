# Validation record

Validated on 2026-10-17, using Linux x64, GCC 15.2.0, CMake 4.2.3, and the pinned vcpkg baseline `a5f736224e85b32e25a8ffc52562333180396642`.

| Item | Actual result |
|---|---|
| CMake configure and complete Debug build | Passed with vcpkg-provided Boost 1.92.0, Lua 5.5.1, OpenSSL 3.6.5#2 |
| Three-process TCP chain | Passed gateway → router → worker roundtrips |
| TCP framing | Passed empty, binary, UTF-8, maximum-size, fragmented, coalesced, and oversized request cases |
| Concurrent clients and session routing | Passed concurrent requests and per-session upstream affinity |
| WS / WSS | Passed Beast handshake, masking, fragmented messages, ping/pong, and CA-trusted TLS echo |
| Lua | Passed successful VM reload, invalid/schematic reload rollback, callback errors, typical infinite-loop interruption, and repeated callbacks |
| Extracted heap | Passed module isolation and ordered pops using the actual linked Lua runtime |
| Failure and resource paths | Passed upstream connection failure, connection limit, message-rate limit, invalid CLI arguments, SIGTERM exit |
| Database pool | Passed three acquire/query/return cycles with a pool of size one |
| Query values and TLS | Passed quote/backslash binding and CA/hostname-verified TLS |
| Database authentication error | Passed failure within the configured acquisition deadline |
| Documentation | VitePress 1.6.4 with patched Vite 6.4.4 built both locales using pnpm 10.33.0; all 10 pages have the expected language/navigation/repository links |
| Documentation dependency audit | pnpm audit reported zero advisories after the Vite override |
| Source package | Allowlist and SHA256 checksum verification passed; no original game directories or Git history |

The database check used a disposable **MariaDB 11.8.6** instance. This verifies the adapter against MariaDB; it is not an Oracle MySQL server compatibility matrix. The test created its own temporary data directory, random credentials, port, and certificate, and cleaned them up after completion.

Commands:

```bash
cmake --preset linux-debug -DMISTLANE_TEST_LOCAL_DATABASE=ON
cmake --build --preset linux-debug
ctest --preset linux-debug -R '^mistlane_integration$'
python3 tools/mysql_integration.py ./build/debug/mistlane-mysql-probe
pnpm install --frozen-lockfile
pnpm docs:build
python3 tools/export.py
```

The optional database check is also registered as mistlane_mysql_integration when enabled, so a complete CTest run can execute both checks.

During dependency setup, the environment's temporary filesystem refused a large OpenSSL archive. Rebuilding the vcpkg working tree on the regular disk resolved it. No port source patches or dependency downgrades were introduced for this environment issue.

Not validated: the original game cluster and business behavior, production load capacity, soak stability, other platforms, full game migration, Oracle MySQL versions. Hosted CI status is available in [GitHub Actions](https://github.com/cuihairu/mistlane/actions). These results support research and learning; they do not establish production readiness.
