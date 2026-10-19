// SPDX-License-Identifier: Apache-2.0
#include "test_support.hpp"
#include "test_fixtures.hpp"
#include "test_resources.hpp"
#include "mistlane/cross.hpp"
#include "mistlane/http_client.hpp"
#include "mistlane/token_bucket.hpp"
#include "mistlane/transport.hpp" // max_frame bounds the flood sizing
#include <curl/curl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <future>
#include <sstream>
#include <netinet/in.h>
#include <sys/ioctl.h>

namespace mistlane::test {
namespace {
constexpr std::string_view echo = "return {message=function(p) return p end}";
class RawSocket {
public:
    explicit RawSocket(unsigned port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        require(fd_ >= 0, "test socket failed");
        timeval timeout{3, 0};
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        sockaddr_in address{};
        address.sin_family = AF_INET; address.sin_port = htons(static_cast<std::uint16_t>(port));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            ::close(fd_); throw std::runtime_error("raw TCP connect failed");
        }
    }
    ~RawSocket() { ::close(fd_); }
    void send(std::string_view value) {
        while (!value.empty()) {
            auto n = ::send(fd_, value.data(), value.size(), MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            require(n > 0, "raw TCP send failed");
            value.remove_prefix(static_cast<std::size_t>(n));
        }
    }
    std::string receive(std::size_t size) {
        std::string result(size, '\0');
        std::size_t offset = 0;
        while (offset < size) {
            auto n = ::recv(fd_, result.data()+offset, size-offset, 0);
            if (n < 0 && errno == EINTR) continue;
            require(n > 0, "raw TCP closed or timed out");
            offset += static_cast<std::size_t>(n);
        }
        return result;
    }
    std::string receive_line() {
        std::string result;
        for (;;) {
            char ch;
            auto n = ::recv(fd_, &ch, 1, 0);
            if (n < 0 && errno == EINTR) continue;
            require(n > 0, "raw TCP closed or timed out");
            if (ch == '\n') break;
            result.push_back(ch);
        }
        return result;
    }
    bool closed() {
        char ch;
        auto n = ::recv(fd_, &ch, 1, 0);
        return n == 0 || (n < 0 && errno == ECONNRESET);
    }
    // True when at least one byte has arrived without blocking.
    bool pending() {
        int bytes = 0;
        return ::ioctl(fd_, FIONREAD, &bytes) == 0 && bytes > 0;
    }
private:
    int fd_;
};
struct Http {
    long status = 0;
    std::string body, headers;
};
Http http(const std::string& url, const std::string& method = "GET",
          const std::string& payload = {}, const std::string& ca = {}, bool metadata = false) {
    static const int runtime = [] { require(curl_global_init(CURL_GLOBAL_DEFAULT) == 0, "curl initialization failed"); return 1; }();
    (void)runtime;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), curl_easy_cleanup);
    require(static_cast<bool>(handle), "curl handle failed");
    Http result;
    curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    if (!ca.empty()) curl_easy_setopt(handle.get(), CURLOPT_CAINFO, ca.c_str());
    auto append = +[](char* bytes, std::size_t width, std::size_t count, void* context) -> std::size_t {
        auto& target = *static_cast<std::string*>(context);
        if (width && count > (1024*1024)/width) return 0;
        const auto n = width*count;
        if (target.size()+n > 1024*1024) return 0;
        target.append(bytes, n); return n;
    };
    curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, append);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(handle.get(), CURLOPT_HEADERFUNCTION, append);
    curl_easy_setopt(handle.get(), CURLOPT_HEADERDATA, &result.headers);
    if (method == "POST") {
        curl_easy_setopt(handle.get(), CURLOPT_POST, 1L);
        curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, payload.data());
        curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(payload.size()));
    }
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(
        metadata ? curl_slist_append(nullptr, "x-test: yes") : nullptr, curl_slist_free_all);
    if (headers) curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers.get());
    const auto error = curl_easy_perform(handle.get());
    if (error != CURLE_OK) throw std::runtime_error(curl_easy_strerror(error));
    curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &result.status);
    return result;
}
std::vector<std::string> tls(const Context& c) {
    return {"--cert", c.path("cert.pem").string(), "--key", c.path("key.pem").string()};
}
// Socket buffers can absorb megabytes before the server-side send bound sees
// anything. Size the flood past tcp_wmem + tcp_rmem plus the 2 MiB bound.
std::string flood_pushes() {
    const auto max_field = [](const char* path) -> std::size_t {
        std::ifstream in(path);
        if (!in) return 6ull*1024ull*1024ull; // linux default-ish fallback
        std::size_t value = 0, item = 0;
        while (in >> item) value = std::max(value, item);
        return value;
    };
    const auto bytes = max_field("/proc/sys/net/ipv4/tcp_wmem")
        + max_field("/proc/sys/net/ipv4/tcp_rmem") + 3*max_frame;
    const auto pushes = std::min<std::size_t>(64, (bytes + max_frame - 1)/max_frame);
    return "flood " + std::to_string(std::max<std::size_t>(pushes, 8));
}
// Minimal websocket client over a raw socket: enough for lifecycle checks that
// the request/response cli client cannot do (registry state, unread floods).
class WsRaw {
public:
    explicit WsRaw(unsigned port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        require(fd_ >= 0, "websocket socket failed");
        timeval timeout{5, 0};
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        sockaddr_in address{};
        address.sin_family = AF_INET; address.sin_port = htons(static_cast<std::uint16_t>(port));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            ::close(fd_); throw std::runtime_error("websocket connect failed");
        }
        send_all("GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
        std::string head;
        char ch;
        while (!head.ends_with("\r\n\r\n")) {
            const auto n = ::recv(fd_, &ch, 1, 0);
            require(n == 1, "handshake incomplete");
            head.push_back(ch);
            require(head.size() < 16384, "handshake too large");
        }
        require(head.starts_with("HTTP/1.1 101"), "upgrade rejected: "+head.substr(0, head.find('\r')));
    }
    ~WsRaw() { ::close(fd_); }
    WsRaw(const WsRaw&) = delete;
    WsRaw& operator=(const WsRaw&) = delete;
    void text(std::string_view payload) {
        require(payload.size() <= 125, "test frame too large");
        const char mask[4] = {'\x11', '\x22', '\x33', '\x44'};
        std::string frame{'\x81', static_cast<char>(0x80 | payload.size()), mask[0], mask[1], mask[2], mask[3]};
        for (std::size_t i = 0; i < payload.size(); ++i) frame.push_back(payload[i] ^ mask[i % 4]);
        send_all(frame);
    }
    void bin(std::string_view payload) {
        require(payload.size() <= 125, "test frame too large");
        const char mask[4] = {'\x11', '\x22', '\x33', '\x44'};
        std::string frame{'\x82', static_cast<char>(0x80 | payload.size()), mask[0], mask[1], mask[2], mask[3]};
        for (std::size_t i = 0; i < payload.size(); ++i) frame.push_back(payload[i] ^ mask[i % 4]);
        send_all(frame);
    }
    std::string read_text() {
        for (;;) {
            unsigned char head[2];
            recv_all(reinterpret_cast<char*>(head), 2);
            std::size_t length = head[1] & 0x7f;
            if (length == 126) { unsigned char ext[2]; recv_all(reinterpret_cast<char*>(ext), 2); length = (ext[0]<<8)|ext[1]; }
            else if (length == 127) {
                unsigned char ext[8]; recv_all(reinterpret_cast<char*>(ext), 8);
                length = 0;
                for (unsigned char byte : ext) length = (length<<8)|byte;
            }
            char mask[4] = {};
            const bool masked = head[1] & 0x80;
            if (masked) recv_all(mask, 4);
            std::string payload(length, '\0');
            if (length) recv_all(payload.data(), length);
            if (masked) for (std::size_t i = 0; i < length; ++i) payload[i] ^= mask[i % 4];
            const auto opcode = head[0] & 0x0f;
            if (opcode == 8) throw std::runtime_error("websocket closed by server");
            if (opcode == 1 || opcode == 2) return payload; // ping/pong/continuation skipped
        }
    }
    // Reads and discards until the server closes the session; returns the bytes seen.
    std::size_t drain_until_closed(std::chrono::seconds budget) {
        const auto deadline = std::chrono::steady_clock::now()+budget;
        std::size_t drained = 0;
        char buffer[65536];
        for (;;) {
            const auto n = ::recv(fd_, buffer, sizeof buffer, 0);
            if (n == 0 || (n < 0 && errno == ECONNRESET)) return drained;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("flood session was not closed");
                continue;
            }
            require(n > 0, "drain failed");
            drained += static_cast<std::size_t>(n);
        }
    }
private:
    void send_all(std::string_view data) {
        while (!data.empty()) {
            const auto n = ::send(fd_, data.data(), data.size(), MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            require(n > 0, "websocket send failed");
            data.remove_prefix(static_cast<std::size_t>(n));
        }
    }
    void recv_all(char* target, std::size_t size) {
        std::size_t offset = 0;
        while (offset < size) {
            const auto n = ::recv(fd_, target+offset, size-offset, 0);
            if (n < 0 && errno == EINTR) continue;
            require(n > 0, "websocket closed while reading a frame");
            offset += static_cast<std::size_t>(n);
        }
    }
    int fd_;
};
}
void core_contracts() {
    TokenBucket bucket(2, 0.000001);
    require(bucket.consume() && bucket.consume() && !bucket.consume(), "token budget is not enforced");
    must_fail([] { TokenBucket invalid(0, 1); });
    auto context = cross_context("MLC1 1 realm group room");
    equal(context.room, "room");
    must_fail([] { (void)cross_context("MLC1 0 realm group room"); });
    must_fail([] { (void)cross_context("MLC1 1 realm group room extra"); });
    CrossAdmission admission(1, 1, 1, 1);
    int a = 0, b = 0;
    require(admission.acquire(&a, context, 0).empty(), "first admission failed");
    equal(admission.acquire(&b, context, 0), "ROOM_FULL");
    admission.release(&a);
    require(admission.acquire(&b, context, 0).empty(), "admission did not recover on release");
    Context c;
    for (auto args : std::vector<std::vector<std::string>>{
        {"--role", "invalid"}, {"--listen", "127.0.0.1:65536"}, {"--role", "gameServer"},
        {"--role", "gateway"}, {"--max-connections", "0"}, {"--role", "gameServer", "--transport", "wss"}}) {
        args.insert(args.begin(), self().string());
        require(c.command(args) != 0, "invalid CLI options accepted");
    }
}
void role_names() {
    Context c;
    write(c.path("echo.lua"), echo);
    for (const auto& old : {"worker", "router", "cross", "shard"}) {
        auto& child = c.spawn({self().string(), "--role", old, "--script", c.path("echo.lua").string(),
                              "--upstream", endpoint(port())});
        require(child.wait() != 0, "removed role accepted");
        require(child.output().find("unknown role") != std::string::npos, "removed role was not rejected by name");
    }
    for (const auto& planned : {"login", "coordinator", "data"}) {
        auto& child = c.spawn({self().string(), "--role", planned});
        require(child.wait() != 0 && child.output().find("planned but not implemented") != std::string::npos,
                "planned service was presented as runnable");
    }
    write(c.path("api.lua"), http_lua);
    const auto api_port = port();
    auto& api = c.node("apiServer", api_port, {"--script", c.path("api.lua").string()});
    require(api.output().find("ready apiServer ") != std::string::npos, "apiServer role missing");
    // Default apiServer transport is HTTP. Missing HTTP callback has a controlled response.
    require(http("http://"+endpoint(api_port)+"/missing").status != 0, "apiServer HTTP default failed");
}
void transport_tcp() {
    Context c;
    const auto script = c.path("echo.lua");
    write(script, echo);
    const auto gameServer=port(), forwarder=port(), gateway=port(), metrics=port();
    c.node("gameServer", gameServer, {"--script", script.string(), "--metrics-listen", endpoint(metrics)});
    c.node("gateway", forwarder, {"--upstream", endpoint(gameServer)});
    c.node("gateway", gateway, {"--upstream", endpoint(forwarder)});
    auto client = tcp(gateway);
    for (const auto& payload : std::vector<std::string>{{}, "hello", std::string("\0\xff", 2), "中文", std::string(1024*1024, 'x')})
        equal(client->request(payload), payload);
    for (int i=0; i<100; ++i) equal(client->request("stack"), "stack");
    auto samples = http("http://"+endpoint(metrics)+"/metrics");
    require(samples.status == 200 && samples.body.find("mistlane_ready{role=\"gameServer\"} 1") != std::string::npos,
        "Prometheus readiness missing");
    require(samples.body.find("mistlane_lua_callback_seconds_count") != std::string::npos, "callback histogram missing");
    const auto metric_value = [](const std::string& body, const std::string& sample) {
        const auto at = body.find(sample);
        require(at != std::string::npos, "missing metric " + sample);
        return std::strtod(body.c_str() + at + sample.size(), nullptr);
    };
    require(metric_value(samples.body, "mistlane_messages_total{role=\"gameServer\",direction=\"rx\"}") > 0,
        "rx message counter missing");
    require(metric_value(samples.body, "mistlane_messages_total{role=\"gameServer\",direction=\"tx\"}") > 0,
        "tx message counter missing");
    require(metric_value(samples.body, "mistlane_payload_bytes_total{role=\"gameServer\",direction=\"tx\"}") > 1024*1024,
        "payload byte counter missing");
    require(metric_value(samples.body, "mistlane_request_seconds_count{role=\"gameServer\"}") > 0,
        "request latency histogram missing");
    require(metric_value(samples.body, "mistlane_request_seconds_sum{role=\"gameServer\"}") > 0,
        "request latency not observed");
    // The first scheduled tick fires at least 100ms after start; wait for it.
    Http ticks;
    eventually([&] {
        ticks = http("http://"+endpoint(metrics)+"/metrics");
        return metric_value(ticks.body, "mistlane_event_loop_lag_seconds_count{role=\"gameServer\"}") > 0;
    });
    require(metric_value(ticks.body, "mistlane_event_loop_lag_max_seconds{role=\"gameServer\"}") > 0,
        "tick lag maximum missing");
    require(metric_value(samples.body, "mistlane_upstream_errors_total{role=\"gameServer\"}") == 0,
        "unexpected upstream errors");
    require(metric_value(samples.body, "mistlane_upstream_timeouts_total{role=\"gameServer\"}") == 0,
        "unexpected upstream timeouts");
    const std::string frame("\0\0\0\5abcde", 9);
    {
        RawSocket raw(gateway);
        for (char ch : frame) raw.send(std::string_view(&ch, 1));
        equal(raw.receive(frame.size()), frame);
        raw.send(frame+frame);
        equal(raw.receive(2*frame.size()), frame+frame);
        raw.send(std::string("\0\x10\0\1", 4));
        require(raw.closed(), "oversize frame was accepted");
    }
    std::vector<std::future<void>> tasks;
    for (int i=0; i<8; ++i) tasks.push_back(std::async(std::launch::async, [=] {
        auto peer = tcp(gateway); equal(peer->request("parallel"), "parallel");
    }));
    for (auto& task : tasks) task.get();
    const auto limited = port();
    c.node("gameServer", limited, {"--script", script.string(), "--max-connections", "1"});
    auto first = tcp(limited); equal(first->request("one"), "one");
    RawSocket extra(limited);
    require(extra.closed(), "connection admission was not enforced");
    equal(first->request("still-alive"), "still-alive");
    const auto dead = port(), ingress = port(), gateway_metrics = port();
    c.node("gateway", ingress, {"--upstream", endpoint(dead), "--metrics-listen", endpoint(gateway_metrics)});
    must_fail([&] { (void)tcp(ingress)->request("unavailable"); });
    auto failure = http("http://"+endpoint(gateway_metrics)+"/metrics");
    require(metric_value(failure.body, "mistlane_upstream_errors_total{role=\"gateway\"}") >= 1,
        "upstream connect failure not counted");
}
void transport_websocket() {
    Context c; c.cert(); write(c.path("echo.lua"), echo);
    const auto gameServer = port(), forwarder = port();
    c.node("gameServer", gameServer, {"--script", c.path("echo.lua").string()});
    c.node("gateway", forwarder, {"--upstream", endpoint(gameServer)});
    for (const auto& transport : {"ws", "wss"}) {
        const bool secure = std::string_view(transport) == "wss";
        for (const auto& role : {"gateway", "gameServer"}) {
            const auto listen = port();
            std::vector<std::string> args{"--transport", transport};
            if (secure) { auto cert = tls(c); args.insert(args.end(), cert.begin(), cert.end()); }
            if (std::string_view(role) == "gateway") args.insert(args.end(), {"--upstream", endpoint(forwarder)});
            else args.insert(args.end(), {"--script", c.path("echo.lua").string()});
            c.node(role, listen, args);
            for (const auto& path : {"/", "/ws"}) {
                cli::ClientOptions options{std::string(transport)+"://localhost:"+std::to_string(listen)+path,
                    secure ? c.path("cert.pem").string() : ""};
                auto client = cli::make_client(options);
                for (const auto& data : std::vector<std::string>{{}, "中文", std::string("\0\xff", 2), std::string(65536, 'w')})
                    equal(client->request(data), data);
            }
            if (secure) {
                // Certificate trust and hostname verification must stay enabled.
                must_fail([&] { auto peer = cli::make_client({"wss://localhost:"+std::to_string(listen)+"/ws", {}, 500}); });
                must_fail([&] { auto peer = cli::make_client({"wss://"+endpoint(listen)+"/ws", c.path("cert.pem").string(), 500}); });
            }
        }
    }
}
void transport_http() {
    Context c; c.cert(); write(c.path("echo.lua"), echo);
    const auto gameServer = port();
    c.node("gameServer", gameServer, {"--script", c.path("echo.lua").string()});
    for (const auto& transport : {"http", "https"}) {
        const bool secure = std::string_view(transport) == "https";
        const auto listen = port();
        std::vector<std::string> args{"--transport", transport, "--upstream", endpoint(gameServer)};
        if (secure) { auto cert = tls(c); args.insert(args.end(), cert.begin(), cert.end()); }
        c.node("gateway", listen, args);
        const auto origin = std::string(transport)+"://localhost:"+std::to_string(listen);
        HttpRequest request{origin+"/message", "POST", std::string("\0\xffhello", 7),
            secure ? c.path("cert.pem").string() : ""};
        const auto result = http_request(request);
        require(result.status == 200, "HTTP adapter status"); equal(result.body, request.body);
        require(http(origin+"/missing", "GET", {}, request.ca_file).status == 404, "unknown HTTP route");
        request.max_response_bytes = 2;
        must_fail([&] { (void)http_request(request); });
        if (secure) {
            request.max_response_bytes = 1024; request.ca_file.clear();
            must_fail([&] { (void)http_request(request); });
        }
    }
    must_fail([] { (void)http_request({"file:///etc/hosts"}); });
}
void transport_listeners() {
    Context c; c.cert();
    write(c.path("echo.lua"), echo);
    write(c.path("ws.lua"), websocket_lua);
    // One process, three listeners: framed tcp plus ws and wss with a
    // per-listener certificate override. The ws listeners stay framed game
    // sessions (Lua message), exactly like single-transport runs.
    const auto plain = port(), ws_port = port(), wss_port = port();
    auto& mixed = c.spawn({self().string(), "--role", "gameServer", "--listen", endpoint(plain),
        "--script", c.path("echo.lua").string(),
        "--listener", "ws://127.0.0.1:"+std::to_string(ws_port),
        "--listener", "wss://127.0.0.1:"+std::to_string(wss_port)+"?cert="+c.path("cert.pem").string()+"&key="+c.path("key.pem").string()});
    mixed.ready("ready gameServer 127.0.0.1:"+std::to_string(wss_port));
    equal(tcp(plain)->request("plain"), "plain");
    equal(cli::make_client({"ws://localhost:"+std::to_string(ws_port)+"/ws"})->request("session"), "session");
    equal(cli::make_client({"wss://localhost:"+std::to_string(wss_port)+"/ws", c.path("cert.pem").string()})->request("secure"), "secure");

    // apiServer: one http listener serving the Lua HTTP service and websocket
    // lifecycle side by side. Plain requests stay with Lua; upgrades upgrade.
    const auto api = port();
    auto& apiServer = c.node("apiServer", api, {"--transport", "http", "--script", c.path("ws.lua").string()});
    const auto origin = "http://"+endpoint(api);
    require(http(origin+"/ws").status == 404, "plain HTTP on the ws path must reach the Lua service");
    require(http(origin+"/missing").status == 404, "Lua HTTP routing broken");
    std::string lifecycle_id;
    {
        WsRaw ws(api);
        ws.text("hello");
        equal(ws.read_text(), "echo:hello");
        ws.text("state");
        const auto state = ws.read_text();
        require(state.starts_with("open=1 closed=0 "), "websocket registry state wrong: "+state);
        lifecycle_id = state.substr(state.rfind('=')+1);
        // A lifecycle message that sends nothing must not wedge the bridge.
        ws.text("noreply");
        ws.text("state");
        require(ws.read_text().starts_with("open=1 closed=0 "), "reply-less message broke the bridge");
        ws.text("oversize");
        equal(ws.read_text(), "guarded");
        ws.text("unknown");
        equal(ws.read_text(), "guarded");
        const auto response = http(origin+"/echo", "POST", "via-lua");
        require(response.status == 201, "HTTP service must share the port with websocket upgrades");
        equal(response.body, "via-lua");
        equal(http(origin+"/state").body, "open=1 closed=0");
    }
    // Clean disconnect: close hook runs, registry bookkeeping updates, same id.
    eventually([&] { return apiServer.output().find("WSHOOK close "+lifecycle_id) != std::string::npos; },
        5s, "websocket close hook did not run");
    eventually([&] { return http(origin+"/state").body == "open=1 closed=1"; }, 5s, "websocket session leak");
    // A Lua failure inside the message callback closes that session only.
    {
        WsRaw ws(api);
        ws.text("boom");
        eventually([&] { return apiServer.output().find("websocket message failed") != std::string::npos; },
            5s, "lua failure in websocket message not logged");
    }
    require(http(origin+"/state").status == 200, "HTTP service must survive a failed websocket session");
    equal(cli::make_client({"ws://localhost:"+std::to_string(api)+"/ws"})->request("still"), "echo:still");
    // Exit strategy: SIGTERM with a live websocket session still exits cleanly.
    {
        WsRaw ws(api);
        ws.text("hello");
        equal(ws.read_text(), "echo:hello");
        apiServer.signal(SIGTERM);
        require(apiServer.wait() == 0, "shutdown with live websocket sessions failed");
    }
    // Slow consumer: pushes past the send bound must close the runaway session
    // while the same process keeps serving fresh ones.
    const auto slow = port();
    auto& lagging = c.node("apiServer", slow, {"--transport", "http", "--script", c.path("ws.lua").string()});
    {
        WsRaw ws(slow);
        ws.text(flood_pushes());
        std::this_thread::sleep_for(500ms); // pushes queue while nobody reads
        const auto drained = ws.drain_until_closed(20s);
        require(drained > 0, "server closed before any flood data reached the client");
        equal(cli::make_client({"ws://localhost:"+std::to_string(slow)+"/ws"})->request("alive"), "echo:alive");
    }
    // HTTPS and WSS on one process: the https primary uses the global cert,
    // the wss listener carries its own via the query override.
    const auto tls_port = port(), wss_extra = port();
    {
        auto cert = tls(c);
        std::vector<std::string> args{"--transport", "https", "--script", c.path("ws.lua").string(),
            "--listener", "wss://127.0.0.1:"+std::to_string(wss_extra)+"?cert="+c.path("cert.pem").string()+"&key="+c.path("key.pem").string()};
        args.insert(args.end(), cert.begin(), cert.end());
        c.node("apiServer", tls_port, args);
    }
    require(http("https://localhost:"+std::to_string(tls_port)+"/state", "GET", {}, c.path("cert.pem").string()).status == 200,
        "https listener not serving the Lua service");
    // The wss listener stays a framed game session: message echo, not lifecycle.
    equal(cli::make_client({"wss://localhost:"+std::to_string(wss_extra)+"/ws", c.path("cert.pem").string()})->request("tls"), "tls");
    // Gateway: framed tcp listener beside an http listener, both forwarding.
    const auto gate = port(), gate_http = port();
    c.node("gateway", gate, {"--upstream", endpoint(plain), "--listener", "http://127.0.0.1:"+std::to_string(gate_http)});
    equal(tcp(gate)->request("tcp-via-gateway"), "tcp-via-gateway");
    const auto forwarded = http("http://"+endpoint(gate_http)+"/message", "POST", std::string("\0\xff", 2));
    require(forwarded.status == 200, "gateway /message forwarding");
    equal(forwarded.body, std::string("\0\xff", 2));
    equal(cli::make_client({"ws://localhost:"+std::to_string(gate_http)+"/ws"})->request("ws-via-gateway"), "ws-via-gateway");
    // Config files carry LISTENERS like any other key.
    const auto cfg_plain = port(), cfg_ws = port();
    write(c.path("node.conf"), "# mistlane-config 1\nROLE=gameServer\nLISTEN=127.0.0.1:"+std::to_string(cfg_plain)+
        "\nSCRIPT="+c.path("echo.lua").string()+"\nLISTENERS=ws://127.0.0.1:"+std::to_string(cfg_ws)+"\n");
    auto& configured = c.spawn({self().string(), "--config", c.path("node.conf").string()});
    configured.ready("ready gameServer 127.0.0.1:"+std::to_string(cfg_ws));
    equal(tcp(cfg_plain)->request("config"), "config");
    equal(cli::make_client({"ws://localhost:"+std::to_string(cfg_ws)+"/ws"})->request("config-ws"), "config-ws");
    // Conflicting listener sets must fail at startup, never half-serve.
    must_fail([&] { c.node("apiServer", port(), {"--transport", "http", "--script", c.path("ws.lua").string(),
        "--listener", "tcp://127.0.0.1:"+std::to_string(port())}); });
    must_fail([&] { const auto reused = port();
        c.node("gameServer", port(), {"--script", c.path("echo.lua").string(),
            "--listener", "tcp://127.0.0.1:"+std::to_string(reused),
            "--listener", "ws://127.0.0.1:"+std::to_string(reused)}); });
}
// Framed game sessions carry a session identity and accept Lua pushes:
// mistlane.session.send/broadcast deliver frames without a matching request,
// a handler may answer nothing, and outbound pushes stay bounded per session.
// Optional correlation mode: the first frame "MLC2" switches the session to
// u32 BE request ids carried inside the frame payload; pushes carry id 0.
void transport_correlate() {
    Context c;
    write(c.path("correlate.lua"), correlate_lua);
    const auto gameServer = port(), gateway = port(), ws_port = port();
    auto& node = c.spawn({self().string(), "--role", "gameServer", "--listen", endpoint(gameServer),
        "--script", c.path("correlate.lua").string(),
        "--listener", "ws://127.0.0.1:"+std::to_string(ws_port)});
    c.node("gateway", gateway, {"--upstream", endpoint(gameServer)});
    const auto send_frame = [](RawSocket& raw, std::string_view payload) {
        std::string frame(4, '\0');
        auto length = static_cast<std::uint32_t>(payload.size());
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(length & 255); length >>= 8; }
        raw.send(frame+std::string(payload));
    };
    const auto read_frame = [](RawSocket& raw) {
        const auto header = raw.receive(4);
        std::size_t length = 0;
        for (unsigned char ch : header) length = (length << 8) | ch;
        return raw.receive(length);
    };
    const auto tagged = [](std::uint32_t id, std::string_view payload) {
        std::string frame(4, '\0');
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(id & 255); id >>= 8; }
        return frame+std::string(payload);
    };
    std::uint32_t id = 0;
    std::string body;
    const auto split = [&id, &body](const std::string& frame) {
        require(frame.size() >= 4, "correlated frame too short");
        id = 0;
        for (int i = 0; i < 4; ++i) id = (id<<8) | static_cast<unsigned char>(frame[i]);
        body = frame.substr(4);
    };
    // Handshake, then replies echo the request id byte for byte.
    RawSocket first(gameServer);
    send_frame(first, "MLC2");
    equal(read_frame(first), "MLC2 OK");
    send_frame(first, tagged(0x01020304, "hello"));
    split(read_frame(first));
    require(id == 0x01020304 && body == "hello", "correlated echo lost id or payload");
    send_frame(first, tagged(9, "self"));
    split(read_frame(first));
    require(id == 9 && body.starts_with("session=") && body.ends_with(":req=9"),
        "lua request id missing: "+body);
    // A nil reply still means no frame; pushes carry id 0 and precede the reply.
    send_frame(first, tagged(5, "silent"));
    std::this_thread::sleep_for(300ms);
    require(!first.pending(), "nil reply produced a frame");
    send_frame(first, tagged(6, "push"));
    split(read_frame(first));
    require(id == 0 && body == "pushed", "push must stay uncorrelated");
    split(read_frame(first));
    require(id == 6 && body == "done", "reply after push lost its id");
    // An oversized correlated push reports instead of failing the handler.
    send_frame(first, tagged(7, "big"));
    split(read_frame(first));
    require(id == 7 && body == "big=false:payload exceeds 1 MiB", "oversize push not reported: "+body);
    // A plain client on the same server stays uncorrelated; broadcast shapes differ.
    RawSocket second(gameServer);
    send_frame(second, "hello");
    equal(read_frame(second), "hello");
    send_frame(second, "bcast");
    split(read_frame(first));
    require(id == 0 && body == "bc", "broadcast to a correlated session must carry id 0");
    equal(read_frame(second), "bc");
    equal(read_frame(second), "bcast=2");
    // The handshake is honored as the first frame only.
    send_frame(second, "MLC2");
    equal(read_frame(second), "MLC2");
    // The same contract through a gateway: ids survive the relay.
    RawSocket via(gateway);
    send_frame(via, "MLC2");
    equal(read_frame(via), "MLC2 OK");
    send_frame(via, tagged(0x0A0B0C0D, "ping"));
    split(read_frame(via));
    require(id == 0x0A0B0C0D && body == "ping", "gateway lost correlation");
    send_frame(via, tagged(3, "self"));
    split(read_frame(via));
    require(id == 3 && body.ends_with(":req=3"), "gateway lua request id missing: "+body);
    // The ws game-session leg speaks the same mode in binary frames.
    {
        WsRaw over(ws_port);
        over.text("MLC2");
        equal(over.read_text(), "MLC2 OK");
        over.bin(tagged(11, "w"));
        split(over.read_text());
        require(id == 11 && body == "w", "ws correlated echo failed");
    }
    // Cross routing keeps its own handshake and rejects correlation.
    const auto shard = port(), cross_gate = port();
    c.node("crossServer", shard, {"--script", c.path("correlate.lua").string()});
    c.cross_gateway(cross_gate, {"--upstream", endpoint(shard)});
    {
        RawSocket crossed(cross_gate);
        send_frame(crossed, "MLC2");
        equal(read_frame(crossed), "MLC1 ERR INVALID");
    }
    require(node.output().find("callback failed") == std::string::npos, "correlation path logged Lua failures");
}
// MLS1 identifies internal legs: a node with --service-token only serves
// peers that present "MLS1 <role> <instance> <token>", a gateway with
// --upstream-token presents itself before any business frame, and every
// refusal closes the connection with a reason instead of leaking frames into
// the script.
void transport_identity() {
    Context c;
    write(c.path("correlate.lua"), correlate_lua);
    const auto gameServer = port(), open_server = port(), gateway = port(),
              no_token = port(), wrong_token = port(), mismatched = port();
    auto& node = c.spawn({self().string(), "--role", "gameServer", "--listen", endpoint(gameServer),
        "--script", c.path("correlate.lua").string(), "--service-token", "s3cret",
        "--service-name", "game-1"});
    auto& open_node = c.spawn({self().string(), "--role", "gameServer", "--listen", endpoint(open_server),
        "--script", c.path("correlate.lua").string()});
    c.node("gateway", gateway, {"--upstream", endpoint(gameServer),
        "--upstream-token", "s3cret", "--service-name", "gwy-1"});
    c.node("gateway", no_token, {"--upstream", endpoint(gameServer)});
    c.node("gateway", wrong_token, {"--upstream", endpoint(gameServer), "--upstream-token", "nope"});
    c.node("gateway", mismatched, {"--upstream", endpoint(open_server), "--upstream-token", "s3cret"});
    const auto send_frame = [](RawSocket& raw, std::string_view payload) {
        std::string frame(4, '\0');
        auto length = static_cast<std::uint32_t>(payload.size());
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(length & 255); length >>= 8; }
        raw.send(frame+std::string(payload));
    };
    const auto read_frame = [](RawSocket& raw) {
        const auto header = raw.receive(4);
        std::size_t length = 0;
        for (unsigned char ch : header) length = (length << 8) | ch;
        return raw.receive(length);
    };
    const auto tagged = [](std::uint32_t id, std::string_view payload) {
        std::string frame(4, '\0');
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(id & 255); id >>= 8; }
        return frame+std::string(payload);
    };
    std::uint32_t id = 0;
    std::string body;
    const auto split = [&id, &body](const std::string& frame) {
        require(frame.size() >= 4, "correlated frame too short");
        id = 0;
        for (int i = 0; i < 4; ++i) id = (id<<8) | static_cast<unsigned char>(frame[i]);
        body = frame.substr(4);
    };
    const auto expect_close = [](RawSocket& raw) {
        for (int i = 0; i < 40 && !raw.closed(); ++i) std::this_thread::sleep_for(50ms);
        require(raw.closed(), "connection stayed open after a refusal");
    };
    // An unauthenticated first frame never reaches the script.
    RawSocket anonymous(gameServer);
    send_frame(anonymous, "self");
    equal(read_frame(anonymous), "MLS1 ERR TOKEN");
    expect_close(anonymous);
    // Malformed handshakes report the shape, not the secret.
    for (const auto& hello : {"MLS1 onlyrole", "MLS1 a b c d"}) {
        RawSocket malformed(gameServer);
        send_frame(malformed, hello);
        equal(read_frame(malformed), "MLS1 ERR INVALID");
        expect_close(malformed);
    }
    RawSocket refused(gameServer);
    send_frame(refused, "MLS1 gateway inst-9 nope");
    equal(read_frame(refused), "MLS1 ERR TOKEN");
    expect_close(refused);
    // Correct credentials admit the session; the handshake stays invisible after.
    RawSocket joined(gameServer);
    send_frame(joined, "MLS1 gateway inst-9 s3cret");
    equal(read_frame(joined), "MLS1 OK");
    send_frame(joined, "self");
    body = read_frame(joined);
    require(body.starts_with("session=") && body.ends_with(":req=nil"), "handshaken session broken: "+body);
    // MLC2 stacks on the accepted handshake: both close at the first payload.
    RawSocket correlated(gameServer);
    send_frame(correlated, "MLS1 gateway inst-9 s3cret");
    equal(read_frame(correlated), "MLS1 OK");
    send_frame(correlated, "MLC2");
    equal(read_frame(correlated), "MLC2 OK");
    send_frame(correlated, tagged(9, "self"));
    split(read_frame(correlated));
    require(id == 9 && body.ends_with(":req=9"), "correlation lost after the handshake: "+body);
    // A token-bearing gateway shows no handshake at either end.
    RawSocket via(gateway);
    send_frame(via, "MLC2");
    equal(read_frame(via), "MLC2 OK");
    send_frame(via, tagged(3, "ping"));
    split(read_frame(via));
    require(id == 3 && body == "ping", "gateway handshake leaked into the relay: "+body);
    RawSocket plain_via(gateway);
    send_frame(plain_via, "self");
    body = read_frame(plain_via);
    require(body.starts_with("session=") && body.ends_with(":req=nil"), "gateway session broke: "+body);
    // An upstream refusal ends the session without relaying the verdict.
    RawSocket silent(wrong_token);
    send_frame(silent, "ping");
    std::this_thread::sleep_for(300ms);
    require(!silent.pending(), "upstream verdict leaked to the client");
    expect_close(silent);
    // A gateway without a token cannot hide the mismatch, so it relays the reason.
    RawSocket relayed(no_token);
    send_frame(relayed, "ping");
    equal(read_frame(relayed), "MLS1 ERR TOKEN");
    expect_close(relayed);
    // A server that needs no handshake refuses the stray one.
    RawSocket to_plain(open_server);
    send_frame(to_plain, "MLS1 gateway inst-9 s3cret");
    equal(read_frame(to_plain), "MLS1 ERR UNEXPECTED");
    expect_close(to_plain);
    RawSocket via_open(mismatched);
    send_frame(via_open, "ping");
    std::this_thread::sleep_for(300ms);
    require(!via_open.pending(), "server refusal leaked to the client");
    expect_close(via_open);
    // ...and keeps serving anonymous clients.
    RawSocket open_client(open_server);
    send_frame(open_client, "hello");
    equal(read_frame(open_client), "hello");
    // Cross routing keeps its own identity, so the token flags refuse to start.
    const auto cross_arg = std::vector<std::string>{"--routing", "cross"};
    for (const auto& extra : {std::vector<std::string>{"--service-token", "s3cret"},
                              std::vector<std::string>{"--upstream-token", "s3cret"}}) {
        auto args = std::vector<std::string>{self().string(), "--role", "gateway",
            "--listen", endpoint(port()), "--upstream", endpoint(gameServer)};
        args.insert(args.end(), cross_arg.begin(), cross_arg.end());
        args.insert(args.end(), extra.begin(), extra.end());
        auto& child = c.spawn(args);
        require(child.wait() != 0 && child.output().find("does not apply to cross routing") != std::string::npos,
                "token flags accepted on cross routing");
    }
    for (const auto& node_output : {node.output(), open_node.output()})
        require(node_output.find("callback failed") == std::string::npos, "handshake path logged Lua failures");
}
// The gateway --upstream-multiplex mode (MLM1) shares one upstream connection
// across client sessions: logical sessions stay isolated, pushes and broadcast
// address a single session, a Lua error or disconnect closes only its own
// logical session, and plain clients are told off instead of upgrading.
void transport_multiplex() {
    Context c;
    write(c.path("correlate.lua"), correlate_lua);
    const auto gameServer = port(), metrics = port(), gateway = port();
    auto& node = c.spawn({self().string(), "--role", "gameServer", "--listen", endpoint(gameServer),
        "--script", c.path("correlate.lua").string(), "--service-token", "s3cret",
        "--metrics-listen", endpoint(metrics)});
    c.node("gateway", gateway, {"--upstream", endpoint(gameServer),
        "--upstream-token", "s3cret", "--upstream-multiplex", "1"});
    const auto send_frame = [](RawSocket& raw, std::string_view payload) {
        std::string frame(4, '\0');
        auto length = static_cast<std::uint32_t>(payload.size());
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(length & 255); length >>= 8; }
        raw.send(frame+std::string(payload));
    };
    const auto read_frame = [](RawSocket& raw) {
        const auto header = raw.receive(4);
        std::size_t length = 0;
        for (unsigned char ch : header) length = (length << 8) | ch;
        return raw.receive(length);
    };
    const auto tagged = [](std::uint32_t id, std::string_view payload) {
        std::string frame(4, '\0');
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(id & 255); id >>= 8; }
        return frame+std::string(payload);
    };
    std::uint32_t id = 0;
    std::string body;
    const auto split = [&id, &body](const std::string& frame) {
        require(frame.size() >= 4, "correlated frame too short");
        id = 0;
        for (int i = 0; i < 4; ++i) id = (id<<8) | static_cast<unsigned char>(frame[i]);
        body = frame.substr(4);
    };
    const auto expect_close = [](RawSocket& raw) {
        for (int i = 0; i < 40 && !raw.closed(); ++i) std::this_thread::sleep_for(50ms);
        require(raw.closed(), "connection stayed open after a refusal");
    };
    const auto established = [](unsigned port) {
        char hex[8];
        std::snprintf(hex, sizeof hex, "%04X", port);
        std::ifstream net("/proc/net/tcp");
        std::string sl, local, remote, state, rest;
        int count = 0;
        while (net >> sl >> local >> remote >> state) {
            if (state == "01" && local.ends_with(":"+std::string(hex))) ++count;
            std::getline(net, rest);
        }
        return count;
    };
    // Two clients ride one upstream connection; the server still counts both.
    RawSocket a(gateway), b(gateway);
    send_frame(a, "hello"); equal(read_frame(a), "hello");
    send_frame(b, "world"); equal(read_frame(b), "world");
    std::this_thread::sleep_for(300ms);
    require(established(gameServer) == 1, "upstream connection not shared by logical sessions");
    const auto live = http("http://"+endpoint(metrics)+"/metrics").body;
    require(live.find("mistlane_active_sessions{role=\"gameServer\"} 2") != std::string::npos,
            "logical sessions missing from server admission");
    // Correlation is a first-frame handshake, so it lives on a third client and
    // must survive the envelope of a session it never shared.
    RawSocket corr(gateway);
    send_frame(corr, "MLC2"); equal(read_frame(corr), "MLC2 OK");
    send_frame(corr, tagged(5, "self"));
    split(read_frame(corr));
    require(id == 5 && body.ends_with(":req=5"), "correlation lost through the shared leg: "+body);
    send_frame(b, "self");
    body = read_frame(b);
    require(body.starts_with("session=") && body.ends_with(":req=nil"), "plain session broken: "+body);
    // A push and a broadcast stay inside their logical session.
    send_frame(corr, tagged(6, "push"));
    split(read_frame(corr)); require(id == 0 && body == "pushed", "push lost on the shared leg");
    split(read_frame(corr)); require(id == 6 && body == "done", "reply lost after a push");
    require(!b.pending(), "push leaked into another logical session");
    // A directly authenticated session shares the registry too, so the count
    // reaches four: a, b, corr and the direct peer.
    RawSocket direct(gameServer);
    send_frame(direct, "MLS1 gateway inst-9 s3cret");
    equal(read_frame(direct), "MLS1 OK");
    send_frame(direct, "hi"); equal(read_frame(direct), "hi");
    send_frame(b, "bcast");
    split(read_frame(corr)); require(id == 0 && body == "bc", "broadcast shape lost on the correlated session");
    equal(read_frame(a), "bc");
    equal(read_frame(b), "bc"); equal(read_frame(b), "bcast=4");
    equal(read_frame(direct), "bc");
    // A Lua error closes only that logical session.
    {
        RawSocket err(gateway);
        send_frame(err, "oops");
        expect_close(err);
    }
    require(node.output().find("mux oops") != std::string::npos,
            "logical session error not reported");
    send_frame(b, "again"); equal(read_frame(b), "again");
    // A disconnect retires its logical session; the rest of the hub survives.
    {
        RawSocket gone(gateway);
        send_frame(gone, "bye"); equal(read_frame(gone), "bye");
        send_frame(b, "bcast");
        equal(read_frame(b), "bc"); equal(read_frame(b), "bcast=5");
        equal(read_frame(a), "bc"); // the survivors read the broadcast twice
    }
    std::this_thread::sleep_for(300ms);
    send_frame(b, "bcast");
    equal(read_frame(b), "bc"); equal(read_frame(b), "bcast=4");
    equal(read_frame(a), "bc");
    // Nobody upgrades mid-session, whether through the gateway or directly.
    send_frame(a, "MLM1"); equal(read_frame(a), "MLM1");
    {
        RawSocket mid(gateway);
        send_frame(mid, "hi"); equal(read_frame(mid), "hi");
        send_frame(mid, "MLM1"); equal(read_frame(mid), "MLM1");
    }
    RawSocket plain(gameServer);
    send_frame(plain, "MLM1");
    equal(read_frame(plain), "MLM1 ERR UNEXPECTED");
    expect_close(plain);
    {
        RawSocket through(gateway);
        send_frame(through, "MLM1");
        equal(read_frame(through), "MLM1 ERR UNEXPECTED");
        expect_close(through);
    }
    // Multiplexing is an internal mode, so it needs the identity handshake.
    {
        auto& bad = c.spawn({self().string(), "--role", "gateway", "--listen", endpoint(port()),
                             "--upstream", endpoint(gameServer), "--upstream-multiplex", "1"});
        require(bad.wait() != 0 && bad.output().find("requires --upstream-token") != std::string::npos,
                "multiplex accepted without a service identity");
    }
    require(node.output().find("unexpected frame") == std::string::npos, "hub parsed a malformed frame");
}
void transport_push() {
    Context c;
    write(c.path("push.lua"), push_lua);
    const auto gameServer = port(), gateway = port(), ws_port = port();
    auto& node = c.spawn({self().string(), "--role", "gameServer", "--listen", endpoint(gameServer),
        "--script", c.path("push.lua").string(),
        "--listener", "ws://127.0.0.1:"+std::to_string(ws_port)});
    c.node("gateway", gateway, {"--upstream", endpoint(gameServer)});
    const auto send_frame = [](RawSocket& raw, std::string_view payload) {
        std::string frame(4, '\0');
        auto length = static_cast<std::uint32_t>(payload.size());
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(length & 255); length >>= 8; }
        raw.send(frame+std::string(payload));
    };
    const auto read_frame = [](RawSocket& raw) {
        const auto header = raw.receive(4);
        std::size_t length = 0;
        for (unsigned char ch : header) length = (length << 8) | ch;
        return raw.receive(length);
    };
    RawSocket first(gameServer);
    send_frame(first, "self");
    equal(read_frame(first), "session=1");
    // A handler may answer nothing; the session must stay usable afterwards.
    send_frame(first, "silent");
    std::this_thread::sleep_for(300ms);
    require(!first.pending(), "nil handler reply produced a frame");
    send_frame(first, "self");
    equal(read_frame(first), "session=1");
    // Lua push reaches the client without a request; the reply follows the pushes.
    send_frame(first, "push");
    equal(read_frame(first), "pushed");
    equal(read_frame(first), "done");
    // Broadcast reaches every session; each client sees the initiator's id.
    RawSocket second(gameServer);
    send_frame(second, "self");
    equal(read_frame(second), "session=2");
    send_frame(first, "bcast");
    equal(read_frame(second), "bc:1");
    equal(read_frame(first), "bc:1");
    equal(read_frame(first), "bcast=2");
    // Push bounds: per-frame cap, unknown ids and the backlog report, not close.
    send_frame(first, "burst 64");
    std::string burst;
    std::size_t pushed = 0;
    for (;;) {
        burst = read_frame(first);
        if (burst.starts_with("burst=")) break;
        require(burst.size() == 64*1024 && burst.find_first_not_of('B') == std::string::npos,
            "pushed frame corrupted");
        ++pushed;
        require(pushed <= 64, "more push frames queued than attempts");
    }
    const auto colon = burst.find(':');
    const auto queued = std::stoul(burst.substr(6, colon-6));
    const auto refused = std::stoul(burst.substr(colon+1));
    require(queued == pushed && refused > 0 && queued+refused == 64, "push bounds not enforced: "+burst);
    // The same contract through a gateway: pushes relay unsolicited.
    RawSocket via(gateway);
    send_frame(via, "self");
    equal(read_frame(via), "session=3");
    send_frame(via, "push");
    equal(read_frame(via), "pushed");
    equal(read_frame(via), "done");
    // Pushes and broadcast also work on the ws game-session leg.
    {
        WsRaw over(ws_port);
        over.text("self");
        equal(over.read_text(), "session=4");
        over.text("push");
        equal(over.read_text(), "pushed");
        equal(over.read_text(), "done");
        over.text("bcast");
        equal(over.read_text(), "bc:4");
        equal(over.read_text(), "bcast=4");
    }
    require(node.output().find("callback failed") == std::string::npos, "push path logged Lua failures");
}
void lua_lifecycle() {
    Context c; const auto script = c.path("service.lua"); write(script, echo);
    const auto listen = port();
    auto& gameServer = c.node("gameServer", listen, {"--script", script.string()});
    auto client = tcp(listen); equal(client->request("v1"), "v1");
    write(script, "return {message=function(p) return 'v2:'..p end}");
    gameServer.signal(SIGHUP); gameServer.ready("script reloaded");
    equal(client->request("x"), "v2:x");
    write(script, "invalid lua !!!");
    gameServer.signal(SIGHUP); gameServer.ready("reload rejected");
    equal(client->request("x"), "v2:x");
    write(script, "return {}");
    gameServer.signal(SIGHUP); gameServer.ready("service.message is required");
    equal(client->request("x"), "v2:x");
    write(c.path("bad.lua"), "return {message=function(p) if p=='error' then error('bad') end "
        "if p=='loop' then while true do end end return p end}");
    const auto bad = port();
    c.node("gameServer", bad, {"--script", c.path("bad.lua").string()});
    for (const auto& payload : {"error", "loop"}) must_fail([&] { (void)tcp(bad)->request(payload); });
    equal(tcp(bad)->request("healthy"), "healthy");
    // The actual exported Lua source is embedded at configure time, so installed
    // binaries can run this regression without depending on a source checkout.
    write(c.path("heap.lua"), "local H=assert(load([==["+std::string(heap_lua)+"]==]))()\n"
        "assert(MinHeap==nil); local h=H:new(3,function(a,b) return a<b end)\n"
        "h:push(3);h:push(1);h:push(2);assert(h:pop()==1 and h:pop()==2 and h:pop()==3)\n"
        "return {message=function(p)return p end}");
    const auto heap = port(); c.node("gameServer", heap, {"--script", c.path("heap.lua").string()});
    equal(tcp(heap)->request("heap"), "heap");
    gameServer.signal(SIGTERM); require(gameServer.wait() == 0, "gameServer graceful shutdown failed");
}
void lua_http() {
    Context c; const auto script = c.path("http.lua"); write(script, http_lua);
    const auto listen = port();
    auto& server = c.node("apiServer", listen, {"--transport", "http", "--script", script.string()});
    const auto origin = "http://"+endpoint(listen);
    auto response = http(origin+"/echo?x=1", "POST", std::string("\0\xff", 2), {}, true);
    require(response.status == 201 && response.headers.find("x-hook: after") != std::string::npos, "HTTP metadata/after hook");
    equal(response.body, std::string("\0\xff", 2));
    for (const auto& [path, status, body] : std::vector<std::tuple<std::string, long, std::string>>{
        {"/private",401,"denied"}, {"/replace",202,"replaced"}, {"/error",502,"handled"},
        {"/badheader",502,"handled"}, {"/fatal",500,"Internal Server Error"}, {"/missing",404,"missing"}}) {
        response = http(origin+path); require(response.status == status, "HTTP hook status"); equal(response.body, body);
    }
    write(script, "return {http=function(r)return{body='bad'}end,start=function()error('start rejected')end}");
    server.signal(SIGHUP); server.ready("reload rejected"); equal(http(origin+"/missing").body, "missing");
    write(script, "return {http=function(r)return{body='v2'}end,"
        "init=function()print('HOOK init v2')end,start=function()print('HOOK start v2')end,"
        "stop=function()print('HOOK stop v2')end,shutdown=function()print('HOOK shutdown v2')end}");
    server.signal(SIGHUP); server.ready("script reloaded"); equal(http(origin+"/").body, "v2");
    server.signal(SIGTERM); require(server.wait() == 0, "HTTP shutdown failed");
    std::istringstream log(server.output()); std::string line; std::vector<std::string> hooks;
    while (std::getline(log, line)) if (line.starts_with("HOOK ")) hooks.push_back(line);
    require(hooks == std::vector<std::string>{"HOOK init v1","HOOK start v1","HOOK init v2","HOOK start v2",
        "HOOK stop v1","HOOK shutdown v1","HOOK stop v2","HOOK shutdown v2"}, "Lua lifecycle order changed");
}
void cross_admission() {
    Context c; write(c.path("cross.lua"), "return {message=function(p,c)return c.realm..':'..c.group..'/'..c.room..':'..p end}");
    const auto s0=port(), s1=port(), ingress=port(), ingress2=port();
    auto& shard = c.node("crossServer", s0, {"--script",c.path("cross.lua").string(),"--max-room-sessions","2"});
    c.node("crossServer", s1, {"--script",c.path("cross.lua").string(),"--max-room-sessions","2"});
    std::vector<std::string> args{"--upstream",endpoint(s0),"--upstream",endpoint(s1),"--max-room-sessions","2"};
    c.cross_gateway( ingress, args); c.cross_gateway( ingress2, args);
    std::array<std::string, 2> rooms;
    for (unsigned i=0; rooms[0].empty() || rooms[1].empty(); ++i) {
        CrossContext route{"a","g","r"+std::to_string(i),1};
        rooms[cross_shard(route,2)] = route.room;
    }
    auto join = [](cli::Client& peer, const std::string& realm, const std::string& room) {
        return peer.request("MLC1 1 "+realm+" g "+room);
    };
    {
        auto a=tcp(ingress), b=tcp(ingress);
        equal(join(*a,"a",rooms[0]),"MLC1 OK 1"); equal(join(*b,"b",rooms[0]),"MLC1 OK 1");
        equal(a->request("x"),"a:g/"+rooms[0]+":x"); equal(b->request("x"),"b:g/"+rooms[0]+":x");
        for (auto entry : {ingress,ingress2}) {
            auto third=tcp(entry); equal(join(*third,"c",rooms[0]),"MLC1 ERR ROOM_FULL");
        }
        auto other=tcp(ingress); equal(join(*other,"c",rooms[1]),"MLC1 OK 1");
    }
    eventually([&] { auto released=tcp(ingress); return join(*released,"d",rooms[0])=="MLC1 OK 1"; });
    equal(tcp(ingress)->request("MLC1 2 a g r"),"MLC1 ERR EPOCH");
    equal(tcp(ingress)->request("invalid"),"MLC1 ERR INVALID");
    shard.signal(SIGTERM); require(shard.wait()==0,"shard shutdown failed");
    must_fail([&] { auto failed=tcp(ingress); (void)join(*failed,"a",rooms[0]); });
    auto healthy=tcp(ingress); equal(join(*healthy,"a",rooms[1]),"MLC1 OK 1");
    equal(healthy->request("ok"),"a:g/"+rooms[1]+":ok");
}
void probe_control() {
    Context c;
    write(c.path("echo.lua"), echo);
    // Capacity 2 with a 0.5 readiness gate saturates at one session; two ticks flip state.
    const auto work=port(), probe=port(), work_metrics=port(), guarded=port(), guard_probe=port();
    c.node("gameServer", work, {"--script", c.path("echo.lua").string(), "--max-connections", "2",
        "--overload-ratio", "0.5", "--ready-hysteresis", "2", "--probe-listen", endpoint(probe),
        "--metrics-listen", endpoint(work_metrics)});
    const auto ask = [](unsigned listen, const std::string& line) {
        RawSocket raw(listen);
        raw.send(line + "\n");
        return raw.receive_line();
    };
    const auto metric_value = [](const std::string& body, const std::string& sample) {
        const auto at = body.find(sample);
        require(at != std::string::npos, "missing metric " + sample);
        return std::strtod(body.c_str() + at + sample.size(), nullptr);
    };
    equal(ask(probe, "PROBE1 n1 liveness"), "PROBE1 n1 ok alive");
    equal(ask(probe, "PROBE9 n2 liveness"), "PROBE1 n2 err version");
    equal(ask(probe, "PROBE1 n3 status"), "PROBE1 n3 err command");
    equal(ask(probe, "PROBE1 n4"), "PROBE1 n4 err malformed");
    equal(ask(probe, "PROBE1 bad!n liveness"), "PROBE1 - err malformed");
    equal(ask(probe, "hello world"), "PROBE1 world err version");
    // Every ask is a fresh connection, so reply buffers and teardown must hold
    // up across repeated probe lifetimes.
    for (int i = 0; i < 50; ++i) {
        const auto nonce = "loop" + std::to_string(i);
        equal(ask(probe, "PROBE1 " + nonce + " liveness"), "PROBE1 " + nonce + " ok alive");
    }
    const auto readiness = ask(probe, "PROBE1 n5 readiness");
    require(readiness.starts_with("PROBE1 n5 ok ready active=0 capacity=2 "),
        "unexpected readiness reply: " + readiness);
    require(readiness.find(" lag_ms=") != std::string::npos, "readiness lag summary missing");
    // A probe connection that never asks must be closed by the server read deadline.
    {
        RawSocket silent(probe);
        eventually([&] { return silent.closed(); });
    }
    // Saturating flips to notready, releasing the session flips back.
    auto session = tcp(work);
    equal(session->request("busy"), "busy");
    eventually([&] {
        return ask(probe, "PROBE1 n6 readiness").find(" ok notready active=1 capacity=2 ") != std::string::npos;
    });
    // The readiness gauge must follow the admission gate, not just start/stop.
    {
        const auto busy = http("http://"+endpoint(work_metrics)+"/metrics");
        require(metric_value(busy.body, "mistlane_ready{role=\"gameServer\"} ") == 0,
            "readiness gauge must drop with the admission gate: " + busy.body);
    }
    session.reset();
    eventually([&] {
        return ask(probe, "PROBE1 n7 readiness").find(" ok ready active=0 capacity=2 ") != std::string::npos;
    });
    {
        const auto recovered = http("http://"+endpoint(work_metrics)+"/metrics");
        require(metric_value(recovered.body, "mistlane_ready{role=\"gameServer\"} ") == 1,
            "readiness gauge must recover with the admission gate: " + recovered.body);
    }
    {
        // Minimal identity check: a token env makes unauthenticated probes fail.
        ScopedEnv token("MISTLANE_PROBE_TOKEN", "s3cret");
        c.node("gameServer", guarded, {"--script", c.path("echo.lua").string(), "--max-connections", "5",
            "--probe-listen", endpoint(guard_probe)});
    }
    equal(ask(guard_probe, "PROBE1 n8 readiness"), "PROBE1 n8 err auth");
    equal(ask(guard_probe, "PROBE1 n9 wrong readiness"), "PROBE1 n9 err auth");
    const auto authenticated = ask(guard_probe, "PROBE1 n10 s3cret readiness");
    require(authenticated.starts_with("PROBE1 n10 ok ready active=0 capacity=5 "),
        "unexpected authenticated readiness: " + authenticated);

    // Cross consults shard readiness for new rooms; established rooms keep flowing.
    const auto shard=port(), shard_probe=port(), ingress=port(), ingress_metrics=port(),
        ingress2=port(), plain=port(), broken=port();
    c.node("crossServer", shard, {"--script", c.path("echo.lua").string(), "--max-connections", "4",
        "--overload-ratio", "0.5", "--ready-hysteresis", "2", "--probe-listen", endpoint(shard_probe)});
    const auto upstream = endpoint(shard) + ":" + std::to_string(shard_probe);
    c.cross_gateway( ingress, {"--upstream", upstream, "--metrics-listen", endpoint(ingress_metrics)});
    c.cross_gateway( ingress2, {"--upstream", upstream});
    const auto join = [](cli::Client& peer, const std::string& room) {
        return peer.request("MLC1 1 realm g " + room);
    };
    auto room1 = tcp(ingress), room2 = tcp(ingress2);
    equal(join(*room1, "r1"), "MLC1 OK 1");
    equal(join(*room2, "r2"), "MLC1 OK 1");
    eventually([&] {
        return ask(shard_probe, "PROBE1 nr readiness").find(" ok notready active=2 ") != std::string::npos;
    }, 5s, "shard to reach notready at active=2");
    equal(join(*tcp(ingress), "r3"), "MLC1 ERR NOT_READY");
    equal(join(*tcp(ingress2), "r4"), "MLC1 ERR NOT_READY");
    // Admission rejections include cross-gated joins, not only local limits.
    {
        const auto rejects = http("http://"+endpoint(ingress_metrics)+"/metrics");
        require(metric_value(rejects.body, "mistlane_admission_rejections_total{role=\"gateway\"} ") >= 1,
            "cross admission rejects are not counted: " + rejects.body);
    }
    room1.reset();
    eventually([&] {
        return ask(shard_probe, "PROBE1 nr2 readiness").find(" ok ready active=1 ") != std::string::npos;
    }, 5s, "shard to recover ready at active=1");
    equal(join(*tcp(ingress), "r5"), "MLC1 OK 1");
    // Without a probe port the cross node cannot gate, so it keeps forwarding.
    c.cross_gateway( plain, {"--upstream", endpoint(shard)});
    equal(join(*tcp(plain), "r7"), "MLC1 OK 1");
    // A dead probe listener is an upstream failure, not a silent accept.
    c.cross_gateway( broken, {"--upstream", endpoint(shard) + ":" + std::to_string(port())});
    equal(join(*tcp(broken), "r8"), "MLC1 ERR UPSTREAM");

    // Bracketed IPv6 listen and upstream addresses, gated and ungated.
    const auto v6_shard=port(), v6_probe=port(), v6_gated=port(), v6_plain=port();
    auto& v6_node = c.spawn({self().string(), "--role", "crossServer",
        "--listen", "[::1]:" + std::to_string(v6_shard),
        "--script", c.path("echo.lua").string(), "--max-connections", "4",
        "--overload-ratio", "0.5", "--ready-hysteresis", "2",
        "--probe-listen", "[::1]:" + std::to_string(v6_probe)});
    v6_node.ready("ready crossServer");
    c.cross_gateway( v6_gated, {"--upstream",
        "[::1]:" + std::to_string(v6_shard) + ":" + std::to_string(v6_probe)});
    equal(join(*tcp(v6_gated), "v1"), "MLC1 OK 1");
    c.cross_gateway( v6_plain, {"--upstream", "[::1]:" + std::to_string(v6_shard)});
    equal(join(*tcp(v6_plain), "v2"), "MLC1 OK 1");

    // An oversized probe request line is closed without any reply bytes.
    {
        RawSocket long_line(probe);
        long_line.send(std::string(300, 'A') + "\n");
        must_fail([&] { (void)long_line.receive_line(); });
    }
    // Concurrent probe connections are capped; rejected sockets close at once.
    {
        std::vector<std::unique_ptr<RawSocket>> held;
        for (int i = 0; i < 64; ++i) held.push_back(std::make_unique<RawSocket>(probe));
        const auto deadline = std::chrono::steady_clock::now() + 1200ms;
        bool rejected = false;
        RawSocket over(probe);
        while (std::chrono::steady_clock::now() < deadline) {
            if (over.closed()) { rejected = true; break; }
            std::this_thread::sleep_for(20ms);
        }
        require(rejected, "peer cap did not close the 65th probe connection");
        held.clear();
        eventually([&] {
            return ask(probe, "PROBE1 freed liveness") == "PROBE1 freed ok alive";
        }, 5s, "probe slots did not free after held peers closed");
    }
}
void config_file() {
    Context c;
    write(c.path("echo.lua"), echo);
    const auto config = c.path("gameServer.env");
    const auto listen = port();
    write(config, "# mistlane-config 1\nROLE=gameServer\nLISTEN=" + endpoint(listen) +
        "\nSCRIPT=echo.lua\nTRANSPORT=tcp\nMAX_CONNECTIONS=5\n");
    {
        auto& node = c.spawn({self().string(), "--config", config.string()});
        node.ready("ready gameServer " + endpoint(listen));
        require(node.output().find("session admission limit 5") != std::string::npos,
            "config max_connections missing: " + node.output());
        equal(tcp(listen)->request("hello"), "hello");
        node.signal(SIGTERM);
        require(node.wait() == 0, "config node should stop cleanly");
    }
    {
        // A version header is required so a random shell env file is not consumed silently.
        write(config, "ROLE=gameServer\nLISTEN=" + endpoint(listen) + "\n");
        auto& bad = c.spawn({self().string(), "--config", config.string()});
        require(bad.wait() == 1, "missing version line must fail");
        require(bad.output().find("mistlane-config 1") != std::string::npos,
            "version error missing: " + bad.output());
    }
    {
        write(config, "# mistlane-config 1\nNOPE=1\n");
        auto& bad = c.spawn({self().string(), "--config", config.string()});
        require(bad.wait() == 1, "unknown key must fail");
        require(bad.output().find("unknown key NOPE") != std::string::npos,
            "unknown key error missing: " + bad.output());
    }
    {
        write(config, "# mistlane-config 1\nMAX_CONNECTIONS=5\nMAX_CONNECTIONS=6\n");
        auto& bad = c.spawn({self().string(), "--config", config.string()});
        require(bad.wait() == 1, "duplicate key must fail");
        require(bad.output().find("duplicate key MAX_CONNECTIONS") != std::string::npos,
            "duplicate key error missing: " + bad.output());
    }
    {
        // Environment beats the file; an explicit flag beats the environment.
        write(config, "# mistlane-config 1\nROLE=gameServer\nLISTEN=" + endpoint(listen) +
            "\nSCRIPT=echo.lua\nTRANSPORT=tcp\nMAX_CONNECTIONS=5\n");
        ScopedEnv around("MISTLANE_MAX_CONNECTIONS", "7");
        auto& node = c.spawn({self().string(), "--config", config.string()});
        node.ready("ready gameServer " + endpoint(listen));
        require(node.output().find("session admission limit 7") != std::string::npos,
            "environment override missing: " + node.output());
        node.signal(SIGTERM);
        require(node.wait() == 0, "overridden node should stop cleanly");
    }
    {
        auto& node = c.spawn({self().string(), "--config", config.string(), "--max-connections", "3"});
        node.ready("ready gameServer " + endpoint(listen));
        require(node.output().find("session admission limit 3") != std::string::npos,
            "flag override missing: " + node.output());
        node.signal(SIGTERM);
        require(node.wait() == 0, "flag node should stop cleanly");
    }
    {
        // ROOT keeps scaffold-style project-root-relative entries readable.
        const auto root = c.path("app");
        fs::create_directory(root);
        write(root / "echo.lua", echo);
        const auto rooted_port = port();
        write(c.path("rooted.env"), "# mistlane-config 1\nROOT=app\nROLE=gameServer\nLISTEN=" +
            endpoint(rooted_port) + "\nSCRIPT=echo.lua\n");
        auto& node = c.spawn({self().string(), "--config", c.path("rooted.env").string()});
        node.ready("ready gameServer " + endpoint(rooted_port));
        equal(tcp(rooted_port)->request("ping"), "ping");
        node.signal(SIGTERM);
        require(node.wait() == 0, "rooted node should stop cleanly");
    }
    {
        const auto metrics = port(), probe = port();
        write(c.path("diag.env"), "# mistlane-config 1\nROLE=gameServer\nLISTEN=" + endpoint(port()) +
            "\nSCRIPT=echo.lua\nMETRICS_LISTEN=" + endpoint(metrics) + "\nPROBE_LISTEN=" + endpoint(probe) + "\n");
        auto& node = c.spawn({self().string(), "--config", c.path("diag.env").string()});
        node.ready("ready gameServer");
        { RawSocket raw(probe); raw.send("PROBE1 cf liveness\n"); equal(raw.receive_line(), "PROBE1 cf ok alive"); }
        const auto scrape = http("http://"+endpoint(metrics)+"/metrics");
        require(scrape.status == 200 && scrape.body.find("mistlane_messages_total") != std::string::npos,
            "config metrics listener missing");
        node.signal(SIGTERM);
        require(node.wait() == 0, "diagnostics node should stop cleanly");
    }
}
void scaffold_init() {
    Context c; const auto root=c.path("sample service");
    require(c.command({self().string(),"init",root.string()})==0,"initializer failed");
    for (const auto& name : {"scripts/echo.lua","scripts/http.lua","scripts/cross.lua",
        "config/instances/gateway.env","config/instances/gateway-ws.env","config/instances/gateway-wss.env",
        "config/instances/gameServer-mixed.env","ops/systemd/mistlane@.service"})
        require(fs::is_regular_file(root/name),"missing template: "+std::string(name));
    for (const auto& name : {"ops/run-instance.sh","ops/cluster.sh","run-local.sh","run-cross-local.sh"}) {
        require(::access((root/name).c_str(),X_OK)==0,"launcher is not executable");
        require(c.command({"sh","-n",(root/name).string()})==0,"invalid shell syntax");
    }
    const auto capture=c.path("args"), recorder=c.path("recorder.sh");
    write(recorder,"#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$MISTLANE_CAPTURE\"\n");
    fs::permissions(recorder,fs::perms::owner_all);
    ScopedEnv bin("MISTLANE_BIN",recorder.string()), output("MISTLANE_CAPTURE",capture.string());
    require(c.command({(root/"ops/run-instance.sh").string(),"gateway-ws"})==0,"WS template launch failed");
    require(read(capture).find("--transport\nws\n")!=std::string::npos,"WS transport not forwarded");
    require(c.command({(root/"ops/run-instance.sh").string(),"gateway-wss"})!=0,"missing certificate accepted");
    const auto cert=root/"certs/server chain.pem", key=c.path("server key.pem");
    write(cert,"fixture"); write(key,"fixture");
    auto config=read(root/"config/instances/gateway-wss.env");
    config.replace(config.find("CERT=certs/fullchain.pem"),std::string("CERT=certs/fullchain.pem").size(),"CERT='certs/server chain.pem'");
    config.replace(config.find("KEY=certs/private-key.pem"),std::string("KEY=certs/private-key.pem").size(),"KEY='"+key.string()+"'");
    write(root/"config/instances/gateway-wss.env",config);
    require(c.command({(root/"ops/run-instance.sh").string(),"gateway-wss"})==0,"WSS template launch failed");
    require(read(capture).find("--cert\n"+cert.string())!=std::string::npos,"relative certificate path not resolved");
    require(read(capture).find("--key\n"+key.string())!=std::string::npos,"absolute key path not resolved");
    require(c.command({(root/"ops/run-instance.sh").string(),"gameServer-mixed"})==0,"mixed template launch failed");
    require(read(capture).find("--listener\nws://127.0.0.1:7082\n")!=std::string::npos,"LISTENERS not forwarded");
    {
        ScopedEnv real_binary("MISTLANE_BIN",self().string());
        const auto listen=port();
        write(root/"config/instances/gameServer.env","ROLE=gameServer\nLISTEN="+endpoint(listen)+"\nSCRIPT=scripts/echo.lua\nTRANSPORT=tcp\nUPSTREAMS=\nROUTE_EPOCH=\n");
        auto& node=c.spawn({(root/"ops/run-instance.sh").string(),"gameServer"});
        node.ready(); equal(tcp(listen)->request("template-path-check"),"template-path-check");
        node.signal(SIGTERM); require(node.wait()==0,"generated gameServer shutdown failed");
    }
    {
        // cluster.sh gates each start on the instance reporting readiness and
        // rolls back instances started by the same command on failure.
        ScopedEnv real_binary("MISTLANE_BIN",self().string());
        const auto cluster=(root/"ops/cluster.sh").string();
        const auto listen=port();
        write(root/"config/instances/gameServer.env","ROLE=gameServer\nLISTEN="+endpoint(listen)+"\nSCRIPT=scripts/echo.lua\nTRANSPORT=tcp\nUPSTREAMS=\nROUTE_EPOCH=\n");
        const auto forwarder=root/"config/instances/gateway.env";
        const auto original_gateway=read(forwarder);
        write(forwarder,"ROLE=gateway\nLISTEN="+endpoint(port())+"\nSCRIPT=scripts/missing.lua\nTRANSPORT=tcp\nUPSTREAMS="+endpoint(listen)+"\nROUTE_EPOCH=\n");
        auto& started=c.spawn({cluster,"start","gameServer"});
        started.reap_descendants(false);  // the daemon must outlive its launcher
        require(started.wait()==0,"cluster start failed");
        equal(tcp(listen)->request("ready-gate"),"ready-gate");
        auto& preserved=c.spawn({cluster,"start","gameServer","gateway"});
        require(preserved.wait()!=0,"broken forwarder instance was accepted");
        require(preserved.output().find("stopping instances started by this command")!=std::string::npos,
            "rollback banner missing");
        auto& preserved_status=c.spawn({cluster,"status","gameServer","gateway"});
        preserved_status.wait();
        require(preserved_status.output().find("gameServer running")!=std::string::npos,
            "pre-existing instance was rolled back");
        require(preserved_status.output().find("gateway stopped")!=std::string::npos,
            "broken instance reported running");
        auto& stopped=c.spawn({cluster,"stop","gameServer"});
        require(stopped.wait()==0,"cluster stop failed");
        auto& rolled_back=c.spawn({cluster,"start","gameServer","gateway"});
        require(rolled_back.wait()!=0,"broken sequence was accepted");
        auto& final_status=c.spawn({cluster,"status","gameServer","gateway"});
        final_status.wait();
        require(final_status.output().find("gameServer stopped")!=std::string::npos,
            "freshly started instance survived rollback");
        require(final_status.output().find("gateway stopped")!=std::string::npos,
            "forwarder reported running after failure");
        write(forwarder,original_gateway);
    }
    write(root/"keep.txt","preserve");
    require(c.command({self().string(),"init",root.string()})!=0,"initializer overwrote nonempty project");
    equal(read(root/"keep.txt"),"preserve");
}
void cli_bot() {
    Context c; write(c.path("echo.lua"),echo);
    const auto listen=port(); c.node("gameServer",listen,{"--script",c.path("echo.lua").string()});
    auto& bot=c.spawn({self().string(),"bot","--url","tcp://"+endpoint(listen),
        "--clients","4","--count","3","--payload","native-bot","--expect-echo"});
    require(bot.wait()==0 && bot.output().find("completed=12")!=std::string::npos,"bot request accounting failed");
    require(c.command({self().string(),"bot","--url","tcp://"+endpoint(listen),"--expect","mismatch"})!=0,
        "bot ignored response mismatch");
    require(c.command({self().string(),"bot","--count","0"})!=0,"bot accepted invalid count");
    require(c.command({self().string(),"test","--case","does-not-exist"})!=0,"unknown test returned success");
    // A listening socket that never replies validates a bounded receive timeout.
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor stalled(io,{boost::asio::ip::address_v4::loopback(),0});
    const auto stalled_port=stalled.local_endpoint().port();
    auto& timeout=c.spawn({self().string(),"bot","--url","tcp://"+endpoint(stalled_port),"--timeout-ms","100"});
    require(timeout.wait(3s)!=0,"bot receive deadline was not enforced");
}
void crashpad_startup(const fs::path& handler) {
    require(fs::is_regular_file(handler),"provide --crashpad-handler FILE for the crashpad suite");
    Context c; write(c.path("echo.lua"),echo); const auto listen=port();
    auto& node=c.node("gameServer",listen,{"--script",c.path("echo.lua").string(),"--crashpad-handler",handler.string(),
        "--crashpad-database",c.path("reports").string()});
    require(fs::is_directory(c.path("reports")) && !fs::is_empty(c.path("reports")),"Crashpad database missing");
    equal(tcp(listen)->request("crashpad"),"crashpad");
    node.signal(SIGTERM); require(node.wait()==0,"Crashpad-enabled gameServer shutdown failed");
}
void crashpad_minidump(const fs::path& handler) {
    require(fs::is_regular_file(handler),"provide --crashpad-handler FILE for the crashpad suite");
    const auto sample=self().parent_path()/"mistlane-crash-sample";
    require(fs::is_regular_file(sample),"build mistlane-crash-sample for the crashpad suite");
    Context c; const auto database=c.path("reports");
    auto& victim=c.spawn({sample.string(),"--crashpad-handler",handler.string(),
        "--crashpad-database",database.string()});
    // The handler is a descendant that must stay alive long enough to finish
    // writing the dump, so opt out of the automatic group kill.
    victim.reap_descendants(false);
    const pid_t group=victim.group();
    bool found=false;
    eventually([&] {
        if (!fs::is_directory(database)) return false;
        for (auto entry : fs::recursive_directory_iterator(database)) {
            if (!entry.is_regular_file()) continue;
            std::ifstream stream(entry.path(),std::ios::binary); char magic[4]{};
            stream.read(magic,sizeof(magic));
            // Minidumps start with the MDMP magic and are far larger than a header.
            if (std::string(magic,magic+sizeof(magic))=="MDMP"
                && fs::file_size(entry.path())>4096) return found=true;
        }
        return false;
    },10s);
    // Give the handler a moment to finish the file before tearing it down.
    std::this_thread::sleep_for(100ms);
    require(victim.wait()==128+SIGSEGV,"crash sample did not die from SIGSEGV");
    require(found,"crashpad database never received a minidump");
    // The handler outlives the sample on purpose; do not leak it.
    if (group>0) ::kill(-group,SIGKILL);
}
}
