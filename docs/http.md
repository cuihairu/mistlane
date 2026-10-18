# HTTP server and client

This page covers protocols, not a process named http. Legacy World embeds HTTP and separate payment services exist. Optional independent external ingress is named apiServer; see [responsibilities](./process-roles.md).


See [process responsibilities](./process-roles.md) for target names. Commands use current CLI roles; character, coordination and persistence integration remain unimplemented.


The legacy World HTTP server used Poco. Mistlane uses Drogon for HTTP/HTTPS and WS/WSS, while internal TCP remains on Boost.Asio. Drogon uses the independent Trantor network library, not Boost. The choice provides a complete web framework; it is not an unmeasured performance ranking.

Game clients connect to `/ws` using `gateway --transport ws/wss --upstream IP:PORT`, with payloads handled by a TCP gameServer's Lua `message(payload)`. Alternatively, `gameServer --transport ws/wss --script FILE` handles them directly. Game messages after the WebSocket upgrade do not invoke Lua HTTP handlers. See the [game WS/WSS guide](./guide.md) for launch commands, client code and instance configuration. The HTTP examples below use a separate listener instance.

## Server

```sh
build/debug/mistlane --role gameServer --listen 127.0.0.1:7102 --script examples/echo.lua
build/debug/mistlane --role gateway --listen 127.0.0.1:8080 --transport http --upstream 127.0.0.1:7102
curl --data-binary 'hello' http://127.0.0.1:8080/message
```

HTTP/HTTPS gateway mode exposes POST /message. The body becomes the Lua payload; success returns 200 with an application/octet-stream body, session/upstream failure returns 503, unknown paths return 404, and unsupported methods are rejected by Drogon. For TLS use --transport https --cert FILE --key FILE. WS/WSS exposes / and /ws. HTTP and WebSocket listeners can also share one process; see [Multiple listeners](#multiple-listeners-and-websocket-lifecycle).

Each HTTP request owns a separate internal session without user affinity. A WS connection keeps one upstream and must wait for a response before sending its next request; pipelining closes the connection. Drogon parses traffic and submits at most one outstanding payload per channel to Asio, where Lua executes.

Body and WS messages are capped at 1 MiB, HTTP pipelining is limited to one, and Trantor closes connections when output backlog crosses a 2 MiB high-water mark. That callback is not a hard allocator quota. Connection admission and internal request deadlines also apply. Drogon manages pre-session TLS/HTTP stages: the internal 10-second processing deadline is not an end-to-end deadline for every network stage. Exercise slow headers/bodies/readers and configure deployment proxy deadlines.

This generic adapter does not restore legacy operations APIs, management authorization or CORS policy. Define application-specific controllers, filters, authentication and permissions explicitly.

## libcurl client

Link mistlane::http and include mistlane/http_client.hpp.

```cpp
mistlane::HttpRequest request;
request.url = "https://example.org/api";
request.method = "POST";
request.body = "hello";
request.connect_timeout_ms = 2000;
request.timeout_ms = 10000;
request.max_response_bytes = 1024 * 1024;
auto response = mistlane::http_request(request);
```

The wrapper supports GET/POST with RAII easy handles. Certificate-chain and hostname verification remain enabled; ca_file optionally selects a trust store. Only HTTP/HTTPS is permitted, redirects are disabled, and response bodies have a byte cap. Transport/TLS/timeout/size failures throw; HTTP 4xx/5xx return their status/body. Applications must authorize destinations: a protocol restriction alone does not prevent access to private addresses.

This is a **blocking** API. Use a bounded application executor and asynchronous completion, never a Lua callback or an Asio/Trantor event loop. No background pool, Lua binding or curl multi adapter is included.

## Dependencies and measurement

vcpkg installs Drogon 1.9.14, Trantor 1.5.28, curl 8.23.0 and OpenSSL. The project overlay pins the Drogon release and source checksum.

Benchmark representative TCP/WS/WSS payload sizes, connection lifetimes, TLS, Lua workload and thread counts. Record P50/P95/P99, CPU/RSS, event-loop delay and rejection behavior, including slow clients and failed upstreams. Whole-framework benchmarks cannot isolate network-library performance or substitute for hot-room tests. No performance comparison or supported player-count claim is published.

References: [Drogon](https://github.com/drogonframework/drogon), [1.9.14 release](https://github.com/drogonframework/drogon/releases/tag/v1.9.14), [curl timeout](https://curl.se/libcurl/c/CURLOPT_TIMEOUT_MS.html), [write callback](https://curl.se/libcurl/c/CURLOPT_WRITEFUNCTION.html).

## HTTP request sequence

```mermaid
sequenceDiagram
  participant Client
  participant Web as Drogon / Trantor
  participant Owner as Asio owner thread
  participant Lua as Lua service
  Client->>Web: HTTP request
  Web->>Web: Parse / body limit / admission
  Web->>Owner: Bounded request submission
  Owner->>Lua: before_request(req)
  alt no early response
    Owner->>Lua: http(req)
  end
  Owner->>Lua: after_request(req, response)
  alt callback or response validation fails
    Owner->>Lua: on_error(req, error)
  end
  Owner-->>Web: status / headers / body
  Web-->>Client: HTTP response
```

## Lua HTTP handlers

Use gameServer --transport http/https --script examples/http.lua for full Lua HTTP request/response handling, middleware and error hooks, plus init/start/tick/stop/shutdown. See [Lua lifecycle and HTTP contracts](./lua).


## Multiple listeners and WebSocket lifecycle

One process runs any mix of independently configured TCP, HTTP, HTTPS, WS and WSS listeners, including simultaneous cleartext and TLS. `--listen`/`--transport` configure the primary listener; each `--listener PROTO://IP:PORT` adds another. A `wss`/`https` listener uses the global `--cert`/`--key` unless it appends its own `?cert=FILE&key=FILE` query override. Listener ports must be unique across the process, so upgrade routing (which reads the local port of the accepted connection) is never ambiguous; conflicting sets fail at startup. Config files carry the same entries under `LISTENERS=` (comma separated).

Routing follows the listener, not the path:

- WS/WSS listeners and gateway HTTP listeners carry framed game sessions → Lua `message`. Plain requests on them do not reach the Lua HTTP service.
- An HTTP/HTTPS listener of a script role (gameServer/apiServer) serves the Lua HTTP service, and a WebSocket upgrade on `/` or `/ws` joins the Lua WebSocket lifecycle below instead of a game session.

The Lua WebSocket lifecycle adds three optional callbacks and one push API:

```lua
local M = {}
function M.websocket_open(id, path) end      -- session joined, upgrade path included
function M.websocket_message(id, payload) end -- one call per client frame (≤ 1 MiB)
function M.websocket_close(id) end           -- disconnect or node shutdown
function M.http(request) end                 -- unchanged, same port as the upgrades
return M
```

`mistlane.websocket.send(id, payload)` pushes to one session and returns `true`, or `false` plus a reason (`unknown websocket id`, `payload exceeds 1 MiB`). Lua errors inside `websocket_open`/`websocket_message` close that session without running `websocket_close`; the node keeps serving. Each push is capped at 1 MiB, and per-connection output backlog past the 2 MiB high-water mark force-closes the slow consumer, which then reaches `websocket_close`. Sessions count against `--max-connections` together with game sessions. On SIGTERM the node closes websocket sessions while the VM can still run their close callbacks. A script that only serves upgrades may omit `message`; a listener mix containing any tcp/ws/wss entry still requires it.

```mermaid
flowchart TB
  config["One process · independent listeners"] --> game["Game ingress: TCP / WS / WSS"]
  config --> web["Web service: HTTP / HTTPS + WS / WSS"]
  game --> message["Game session → Lua message"]
  web --> request["HTTP request → Lua http"]
  web --> socket["Upgrade → websocket_open/message/close + push"]
```
