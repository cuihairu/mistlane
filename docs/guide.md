# Getting started

This guide runs the extracted framework for research and learning. It does not deploy the original game.

## Requirements

Linux x64, a C++20 compiler, CMake 3.25+, Ninja, Git, curl, zip/unzip, tar, pkg-config, Perl, Python 3.9+, and the OpenSSL command-line tool. Documentation also requires Node.js 20+ and npm.

## Prepare vcpkg

```bash
git clone https://github.com/microsoft/vcpkg.git "$HOME/vcpkg-mistlane"
git -C "$HOME/vcpkg-mistlane" checkout a5f736224e85b32e25a8ffc52562333180396642
"$HOME/vcpkg-mistlane/bootstrap-vcpkg.sh" -disableMetrics
export VCPKG_ROOT="$HOME/vcpkg-mistlane"
```

An existing vcpkg checkout can be reused if it includes the pinned baseline and the matching version database. CMake uses the toolchain at `VCPKG_ROOT`; machine-specific paths belong in an ignored `CMakeUserPresets.json`.

## Build and check

From the repository root:

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug
```

Use `linux-release` for a Release build. Initial configuration installs the manifest dependencies; subsequent builds reuse vcpkg's binary cache. See [dependencies](./dependencies.md) for the exact versions.

## Run three processes

Open three terminals in the repository root:

```bash
./build/debug/mistlane-node --role worker --listen 127.0.0.1:7102 --script examples/echo.lua
./build/debug/mistlane-node --role router --listen 127.0.0.1:7101 --upstream 127.0.0.1:7102
./build/debug/mistlane-node --role gateway --listen 127.0.0.1:7100 --upstream 127.0.0.1:7101
```

Then send a request:

```bash
python3 tools/client.py 'hello mistlane'
```

Repeat `--upstream IP:PORT` to distribute new sessions across several workers. A session stays with its selected upstream. This is not routing by account or player ID.

## WebSocket and TLS

Add `--transport ws` to a gateway to accept WebSocket messages. Each WS message is a payload; do not prepend the internal TCP length header.

```bash
./build/debug/mistlane-node --role gateway --listen 127.0.0.1:7443 \
  --transport wss --cert /path/to/fullchain.pem --key /path/to/private-key.pem \
  --upstream 127.0.0.1:7101
```

Use your own certificate and private key, kept outside source control. Beast handles protocol details. WSS requires TLS 1.2 or newer. Responses are binary WS messages. Internal TCP frames use a four-byte big-endian length and a payload, up to 1 MiB.

## Lua services

A script returns a table with `message(payload) -> string`. Optional callbacks are `init()`, `tick()`, and `shutdown()`. Tick runs every 100ms. All callbacks execute on the node's single event-loop thread and should return quickly.

SIGHUP constructs and initializes a replacement VM first. A failed load retains the previous VM; a successful load resets module state. No state migration is performed. SIGTERM and SIGINT stop listeners, cancel sessions, and invoke the current module's shutdown callback.

Scripts are trusted application extensions, not a security sandbox for untrusted code. The instruction hook can interrupt typical runaway callbacks but provides no complete CPU or memory isolation.

## MySQL pool

`include/mistlane/mysql_pool.hpp` configures the existing Boost.MySQL pool. Connections are acquired asynchronously and returned by the lifetime of `pooled_connection`. Query values use `with_params`, not string concatenation.

```bash
export MISTLANE_MYSQL_HOST=localhost
export MISTLANE_MYSQL_PORT=3306
export MISTLANE_MYSQL_USER=your_user
# Inject MISTLANE_MYSQL_PASSWORD from your environment.
./build/debug/mistlane-mysql-probe
```

TLS with CA and hostname verification is required by default. Set `SSL_CERT_FILE` for a private CA. `MISTLANE_MYSQL_TLS=disable` is an explicit option for disposable local tests. The example uses a pool of size one and three parameterized queries to verify acquisition and return.

With MariaDB tools installed, run the optional disposable-server integration check:

```bash
python3 tools/mysql_integration.py ./build/debug/mistlane-mysql-probe
```

It creates a temporary database directory, random credentials, a temporary port, and a temporary certificate, then stops the server and cleans up. To register it in CTest, configure with `-DMISTLANE_TEST_LOCAL_DATABASE=ON`.

The pool adapter is not yet wired to Lua message callbacks. Do not execute blocking SQL in the Lua event loop.

## Documentation

```bash
npm ci
npm run docs:dev
npm run docs:build
npm run docs:preview
```

The site contains English and Chinese navigation, local search, and matching pages. Its default base is `/mistlane/`; set `DOCS_BASE=/` when building for a domain root. The build produces `docs/.vitepress/dist` and checks internal links. No hosting deployment is performed by these commands.

## Export source

```bash
python3 tools/export.py
```

The archive uses the explicit `release-files.txt` allowlist and includes SHA256 checksums. It contains no parent repository history, private configuration, original gameplay code, or prebuilt binaries.
