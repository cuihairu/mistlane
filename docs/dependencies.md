# Dependency management

CMake and vcpkg manifest mode manage all C++ dependencies. The project includes no historical third-party source tree or compiled library. CMake does not invoke a second Make build or download OpenSSL itself.

Pinned baseline: `a5f736224e85b32e25a8ffc52562333180396642`, checked on 2026-10-17.

| Dependency | Baseline version | Integration |
|---|---|---|
| boost-asio | 1.92.0 | Boost::asio, asynchronous I/O and coroutines |
| drogon | 1.9.14 (overlay) | Drogon::Drogon, HTTP/HTTPS/WS/WSS |
| trantor | 1.5.28 | Drogon network dependency; independent of Boost.Asio |
| curl | 8.23.0 | CURL::libcurl, HTTP client |
| boost-mysql | 1.92.0 | Boost::mysql, asynchronous connection_pool |
| lua | 5.5.1 | CMake FindLua include/library variables |
| openssl | 3.6.5#2 | OpenSSL::SSL and OpenSSL::Crypto |
| prometheus-cpp | 1.3.0 | prometheus-cpp::core plus the `pull` feature, backing the optional private `/metrics` listener |
| crashpad | 2026-07-02 | crashpad::crashpad, local minidump capture through the installed `crashpad_handler` |
| VitePress | 1.6.4 | pnpm exact direct version and pnpm-lock.yaml |
| mermaid | 12.2.0 | Markdown diagram renderer bundled by the docs theme |
| playwright | 1.64.1 | Headless browser check of rendered diagrams, responsive and dark themes |
| Vite | 6.4.4 | Explicit workspace override for patched development tooling |
| pnpm | 10.33.0 | packageManager pin |

Boost.Charconv and other transitive requirements are supplied by the Boost.MySQL port and imported targets. Asio explicitly requests TLS support; transitive ports determine the final resolved feature set.

“Latest” here means versions supplied by the selected vcpkg baseline at the checking date. Boost and Lua also match the latest stable upstream releases. OpenSSL upstream has 4.0.3, while this baseline provides 3.6.5#2; the project uses the packaged port and does not claim to integrate 4.0.3. No prerelease or moving latest tag is used. VitePress uses npm's stable dist-tag, not its 2.x alpha line.

The original project's log4cxx, Poco, Pion, jemalloc, custom MySQL/Redis/Lua extensions, and channel SDKs are excluded from the extracted server build. The test-only vendored sproto/LPeg sources below do not restore or upgrade the original game build.

### Vendored protocol test sources

The protocol conformance probe vendors two upstream source snapshots so a clean build and CTest run do not download parser or codec sources. `third_party/sproto/` contains cloudwu/sproto at commit `e93fd5a0653b2bcab38d3b830958bf2e3a901b5f` (MIT; upstream [repository](https://github.com/cloudwu/sproto)); `third_party/lpeg/` contains the `luvit/lpeg` mirror at commit `55bbd00e53890e50e05018907140f4f3d3e234f4` (LPeg upstream mirror; MIT; [mirror](https://github.com/luvit/lpeg), [upstream project](https://www.inf.puc-rio.br/~roberto/lpeg/)). The copied license texts are [licenses/sproto-MIT.txt](../licenses/sproto-MIT.txt) and [licenses/lpeg-MIT.txt](../licenses/lpeg-MIT.txt). CMake compiles these sources into the test-only `mistlane-sproto-probe`, linked against the already pinned Lua 5.5.1 dependency; they are not linked into the Mistlane server. LPeg is needed by cloudwu's text schema parser.

## Upgrade workflow

1. Select an immutable vcpkg commit and update builtin-baseline in vcpkg.json.
2. Review versions and upstream changes; update minimum version constraints in the manifest and CMake when needed.
3. Configure a clean build directory and inspect resolved direct and transitive versions.
4. Run network/Lua checks and disposable database checks when available, then update the validation record.
5. For docs, update the exact VitePress version and pnpm lockfile together, then run pnpm install --frozen-lockfile and the documentation build.

Use a vcpkg checkout containing the pinned commit and matching version database. An older working tree can fail to resolve versions even when the baseline commit is fetched. CI uses a full vcpkg clone and checks out the pinned commit. A shallow clone can lack historical trees referenced by versioned ports.

Sources: [vcpkg baseline](https://github.com/microsoft/vcpkg/tree/a5f736224e85b32e25a8ffc52562333180396642), [Boost stable release](https://www.boost.org/releases/latest/), [Lua downloads](https://www.lua.org/ftp/), [OpenSSL downloads](https://www.openssl-library.org/source/), [Boost.MySQL pool tutorial](https://www.boost.org/latest/libs/mysql/doc/html/mysql/tutorial_connection_pool.html), [VitePress](https://vitepress.dev/).

VitePress 1.6.4 defaults to the older Vite 5 line. The workspace overrides it with Vite 6.4.4 to address reported Vite/esbuild dependency advisories while keeping the stable VitePress release and plugin-vue 5 compatibility. Documentation builds and a dependency audit validate this override. The package manager version is pinned in package.json; CI reads that pin.

Drogon 1.9.14 is the stable release checked on 2026-10-18; the pinned baseline packages 1.9.13#2. vcpkg-configuration.json selects a ports overlay with a fixed release/SHA512, adapted vcpkg packaging patches and a static JsonCpp target fix. CMake does not download Drogon separately. Drogon ORM/MySQL features are disabled; Boost.MySQL remains the pool implementation. Trantor is installed transitively.

The same Boost line supplies Asio `thread_pool`, `steady_timer`, UDP sockets, Boost.Log and Boost.Redis. Mistlane enables Boost.Redis for optional Lua Streams and leaderboard APIs. Drogon's separate Redis client remains disabled to avoid maintaining two clients. See the [migration status ledger](./migration-status) for boundaries and upstream references.
