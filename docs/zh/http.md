# HTTP 服务端与客户端

旧 World HTTP 服务端用了 Poco。Mistlane 使用 Drogon 处理 HTTP/HTTPS、WS/WSS 及协议解析；TCP 核心仍用 Boost.Asio。Drogon 基于独立的 Trantor 网络库，并非基于 Boost。选型理由是直接使用完整 Web 框架能力，不是未经验证的性能排名。

游戏客户端通过 `gateway --transport ws/wss --upstream IP:PORT` 连接 `/ws`，消息由 TCP worker 的 Lua `message(payload)` 处理；也可通过 `worker --transport ws/wss --script FILE` 直接处理。WebSocket 升级后的游戏消息不会调用 Lua HTTP handler。完整启动命令、客户端示例与模板配置见[游戏 WS/WSS 接入指南](./guide.md)。本页下面的 HTTP 示例使用独立的 HTTP 监听实例。

## 服务端

```sh
build/debug/mistlane --role worker --listen 127.0.0.1:7102 --script examples/echo.lua
build/debug/mistlane --role gateway --listen 127.0.0.1:8080 --transport http --upstream 127.0.0.1:7102
curl --data-binary 'hello' http://127.0.0.1:8080/message
```

gateway/router 的 HTTP/HTTPS 模式暴露 POST /message，body 作为 Lua payload，成功返回 200 与 application/octet-stream 正文；会话或上游失败返回 503，未知路径为 404，非允许方法由 Drogon 拒绝。HTTPS 使用 --transport https --cert FILE --key FILE。WS/WSS 支持 / 和 /ws，与 HTTP 使用独立监听进程；本版本未在同一监听器混合启用二者。

HTTP 每个请求创建独立内部会话，不提供按用户固定上游的保证。WS 连接保持同一上游，必须等上一响应再发下一请求；超前发送会关闭连接。Drogon 解析请求，在有界通道中递交至 Asio，Asio 执行业务并返回结果。没有把 curl 或 Lua 放在 Trantor 线程直接执行。

正文与 WS 消息最大 1 MiB，HTTP pipeline 配置上限为 1；Trantor 发送积压越过 2 MiB 高水位时关闭连接。高水位是越界后回调，不是严格内存分配器配额。另有连接准入和请求期限；TLS/HTTP 握手的前置阶段由 Drogon 管理，不能把内部会话的 10 秒处理超时当成所有网络阶段的总期限。上线前需对慢头部、慢正文与慢读进行压测，并按部署配置反向代理期限。

这是通用消息适配器；没有迁入旧运营 HTTP API，也不提供管理认证或跨域策略。需要在应用自己的 Drogon controller/filter 中显式定义路由、认证和权限。

## libcurl 客户端

CMake target 为 mistlane::http，头文件 include/mistlane/http_client.hpp。

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

当前支持 GET/POST，RAII 释放 easy handle。默认验证证书链和主机名，可通过 ca_file 指定 CA；只允许 HTTP/HTTPS，默认不跟随重定向，响应正文有字节上限。传输、TLS、超时和超限通过异常报告；HTTP 4xx/5xx 保留 status 与 body，不混同网络失败。目标地址必须由应用授权；只限制协议并不等于阻止内网地址访问。

接口是**同步阻塞**调用，应放入有界应用工作线程，再将结果异步回投；当前不提供后台线程池、Lua 绑定或异步 curl multi 封装。禁止在 Lua message/tick 或 Asio/Trantor 事件循环上直接调用。

## 依赖与性能验收

vcpkg 管理 Drogon 1.9.14、Trantor 1.5.28、curl 8.23.0 及 OpenSSL。Drogon 使用项目 overlay，版本和源码校验值固定。

吞吐需结合 TCP/WS/WSS、消息尺寸、连接寿命、TLS、Lua 工作量和线程数测量。观察 P50/P95/P99、CPU/RSS、事件循环延迟与过载拒绝，包含慢客户端及下游故障。框架跑分不能证明单一网络库优劣，也不能代替跨服热点压测。当前未发布性能对比或承载人数结论。

参考：[Drogon 官方仓库](https://github.com/drogonframework/drogon)、[Drogon 1.9.14](https://github.com/drogonframework/drogon/releases/tag/v1.9.14)、[libcurl 超时](https://curl.se/libcurl/c/CURLOPT_TIMEOUT_MS.html)、[响应回调](https://curl.se/libcurl/c/CURLOPT_WRITEFUNCTION.html)。

## HTTP 请求时序

```mermaid
sequenceDiagram
  participant Client
  participant Web as Drogon / Trantor
  participant Loop as Asio owner thread
  participant Lua as Lua service
  Client->>Web: HTTP request
  Web->>Web: Parse / body limit / admission
  Web->>Loop: Bounded request submission
  Loop->>Lua: before_request(req)
  alt no early response
    Loop->>Lua: http(req)
  end
  Loop->>Lua: after_request(req, response)
  alt callback or response validation fails
    Loop->>Lua: on_error(req, error)
  end
  Loop-->>Web: status / headers / body
  Web-->>Client: HTTP response
```

## 使用 Lua 编写 HTTP handler

worker --transport http/https --script examples/http.lua 可直接用 Lua 处理完整请求和响应，支持 before_request/http/after_request/on_error，以及 init/start/tick/stop/shutdown。详见 [Lua 生命周期与 HTTP 契约](./lua)。
