# Dependency management

CMake and vcpkg manifest mode manage all C++ dependencies. The project includes no historical third-party source tree or compiled library. CMake does not invoke a second Make build or download OpenSSL itself.

Pinned baseline: `a5f736224e85b32e25a8ffc52562333180396642`, checked on 2026-10-17.

| Dependency | Baseline version | Integration |
|---|---|---|
| boost-asio | 1.92.0 | Boost::asio, asynchronous I/O and coroutines |
| boost-beast | 1.92.0 | Boost::beast, WS/WSS |
| boost-mysql | 1.92.0 | Boost::mysql, asynchronous connection_pool |
| lua | 5.5.1 | CMake FindLua include/library variables |
| openssl | 3.6.5#2 | OpenSSL::SSL and OpenSSL::Crypto |
| VitePress | 1.6.4 | npm exact direct version and package-lock.json |

Boost.Charconv and other transitive requirements are supplied by the Boost.MySQL port and imported targets. Asio explicitly requests TLS support; transitive ports determine the final resolved feature set.

“Latest” here means versions supplied by the selected vcpkg baseline at the checking date. Boost and Lua also match the latest stable upstream releases. OpenSSL upstream has 4.0.3, while this baseline provides 3.6.5#2; the project uses the packaged port and does not claim to integrate 4.0.3. No prerelease or moving latest tag is used. VitePress uses npm's stable dist-tag, not its 2.x alpha line.

The original project's log4cxx, Poco, Pion, jemalloc, custom MySQL/Redis/Lua extensions, LPeg, sproto, and channel SDKs are excluded from the extracted build. This work does not upgrade the original game build.

## Upgrade workflow

1. Select an immutable vcpkg commit and update builtin-baseline in vcpkg.json.
2. Review versions and upstream changes; update minimum version constraints in the manifest and CMake when needed.
3. Configure a clean build directory and inspect resolved direct and transitive versions.
4. Run network/Lua checks and disposable database checks when available, then update the validation record.
5. For docs, update the exact VitePress version and npm lockfile together, then run npm ci and the documentation build.

Use a vcpkg checkout containing the pinned commit and matching version database. An older working tree can fail to resolve versions even when the baseline commit is fetched. CI checks out a separate vcpkg tree at the pinned commit.

Sources: [vcpkg baseline](https://github.com/microsoft/vcpkg/tree/a5f736224e85b32e25a8ffc52562333180396642), [Boost stable release](https://www.boost.org/releases/latest/), [Lua downloads](https://www.lua.org/ftp/), [OpenSSL downloads](https://www.openssl-library.org/source/), [Boost.MySQL pool tutorial](https://www.boost.org/latest/libs/mysql/doc/html/mysql/tutorial_connection_pool.html), [VitePress](https://vitepress.dev/).
