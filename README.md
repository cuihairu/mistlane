# Mistlane

**English | [简体中文](README.zh-CN.md)**

A C++20 / Lua framework **extracted and reorganized from a legacy game backend**, published for **research and learning**. It demonstrates service separation, asynchronous networking, Lua embedding, and dependency modernization.

This is an educational framework candidate, not a production deployment recommendation. It removes the old game's gameplay, content tables, account/payment integration, channel SDKs, production configuration, and repository history. New interfaces and a new wire protocol mean it cannot directly replace the original cluster.

## Components

| Area | Implementation |
|---|---|
| Async networking | Boost.Asio |
| WS / WSS | Boost.Beast and OpenSSL |
| MySQL pool | Existing Boost.MySQL connection_pool |
| Scripting | Lua module lifecycle and replaceable VMs |
| C++ dependencies | CMake Presets + pinned vcpkg manifest |
| Documentation | VitePress, English and Chinese |

```text
client --> gateway --> router --> workers + Lua
application extension --> Boost.MySQL pool --> database
```

Routing is per session. Database integration is a separate C++ adapter, not yet a Lua database bridge. See [architecture and limits](docs/architecture.md).

## Build

Linux x64, C++20, CMake 3.25+, Ninja, Git, curl, zip/unzip, tar, pkg-config, Perl, Python 3.9+, and the OpenSSL CLI are required. Use a vcpkg checkout at the manifest baseline:

```bash
git clone https://github.com/microsoft/vcpkg.git "$HOME/vcpkg-mistlane"
git -C "$HOME/vcpkg-mistlane" checkout a5f736224e85b32e25a8ffc52562333180396642
"$HOME/vcpkg-mistlane/bootstrap-vcpkg.sh" -disableMetrics
export VCPKG_ROOT="$HOME/vcpkg-mistlane"
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug
```

Use linux-release for Release. The baseline supplies Boost 1.92.0, Lua 5.5.1, and OpenSSL 3.6.5#2. OpenSSL upstream has 4.0.3; this project uses the version packaged in vcpkg. [Dependency policy](docs/dependencies.md).

## Run

Start these in three terminals from the repository root:

```bash
./build/debug/mistlane --role worker --listen 127.0.0.1:7102 --script examples/echo.lua
./build/debug/mistlane --role router --listen 127.0.0.1:7101 --upstream 127.0.0.1:7102
./build/debug/mistlane --role gateway --listen 127.0.0.1:7100 --upstream 127.0.0.1:7101
```

```bash
python3 tools/client.py 'hello mistlane'
```

Use --transport ws for WS; WSS also requires --transport wss --cert FILE --key FILE. The [guide](docs/guide.md) covers TLS, Lua callbacks, script reload, MySQL pool usage, and the optional disposable database check.

## Bilingual documentation

With Node.js 20+ and the pnpm version pinned in package.json (enable Corepack if needed):

```bash
pnpm install --frozen-lockfile
pnpm docs:dev
pnpm docs:build
pnpm docs:preview
```

English pages live at docs/, Chinese pages at docs/zh/. The VitePress language menu switches between them. Default base: /mistlane/; override with DOCS_BASE for another deployment path. Building does not publish the site.

## Source release and licensing

```bash
python3 tools/export.py
```

Only reviewed files in release-files.txt enter the source archive, with SHA256 checksums. The archive excludes original game files, Git history, private configuration, dependency binaries, and build output.

New code retains the independent repository's MIT [LICENSE](LICENSE). Two derived utility files retain Apache-2.0; its full text is in [licenses/Apache-2.0.txt](licenses/Apache-2.0.txt). See [NOTICE](NOTICE) and [third-party information](THIRD_PARTY.md) for provenance. The original meaning of FOGS is unconfirmed.
