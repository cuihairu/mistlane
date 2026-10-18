// SPDX-License-Identifier: Apache-2.0
#include "mistlane/node.hpp"
#include "mistlane/transport.hpp"
#include <drogon/drogon.h>
#include <drogon/WebSocketController.h>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <map>
#include <optional>

namespace mistlane {
namespace {
namespace asio = boost::asio;
// Only payload delivery crosses from Trantor to Asio. Lua never runs on Trantor.
class WebChannel final : public Channel, public std::enable_shared_from_this<WebChannel> {
public:
    using Sender = std::function<void(std::string)>;
    WebChannel(asio::any_io_executor ex, Sender send, std::function<void()> close, bool once)
      : executor_(ex), sender_(std::move(send)), closer_(std::move(close)), once_(once) {}
    asio::any_io_executor executor() override { return executor_; }
    void start(Completion cb) override { cb({}); }
    void read(Reader cb) override {
        if (closed_ || finished_) return cb(asio::error::eof, {});
        reader_ = std::move(cb);
        if (incoming_) { auto data = std::move(*incoming_); incoming_.reset(); deliver(std::move(data)); }
    }
    void write(std::string data, Completion cb) override {
        if (closed_) return cb(asio::error::operation_aborted);
        if (data.size() > max_frame) return cb(asio::error::message_size);
        sender_(std::move(data));
        finished_ = once_;
        cb({});
    }
    void close() override {
        if (closed_.exchange(true)) return;
        closer_();
        if (reader_) { auto cb = std::move(reader_); cb(asio::error::operation_aborted, {}); }
    }
    // Called by Drogon. At most one posted payload per channel; no unbounded bridge queue.
    void incoming(std::string data) {
        if (closed_ || closing_) return;
        if (data.size() > max_frame || !credit_.exchange(false)) return peer_closed();
        asio::post(executor_, [self=shared_from_this(), data=std::move(data)]() mutable {
            if (!self->closed_) self->deliver(std::move(data));
        });
    }
    void peer_closed() {
        if (closing_.exchange(true)) return;
        asio::post(executor_, [self=shared_from_this()] { self->close(); });
    }
private:
    void deliver(std::string data) {
        // The bridge slot frees once the payload is dispatched, so a message
        // that produces no reply (nil handler result, lifecycle message
        // without websocket.send) still admits the next one.
        credit_ = true;
        if (!reader_) { incoming_ = std::move(data); return; }
        auto cb = std::move(reader_); cb({}, std::move(data));
    }
    asio::any_io_executor executor_;
    Sender sender_;
    std::function<void()> closer_;
    Reader reader_;
    std::optional<std::string> incoming_;
    std::atomic<bool> credit_{true}, closed_{false}, closing_{false};
    bool once_, finished_ = false;
};
class SocketController : public drogon::WebSocketController<SocketController, false> {
public:
    // Which listener an upgrade lands on decides its contract: ws/wss listeners
    // (and gateway http listeners) carry framed game sessions; an http/https
    // listener of a script role joins the Lua websocket lifecycle instead.
    enum class Kind { game, lifecycle };
    using Dispatch = std::function<Kind(std::uint16_t port)>;
    SocketController(asio::any_io_executor ex, Attach attach, WsAttach ws_attach, Dispatch dispatch)
      : executor_(ex), attach_(std::move(attach)), ws_attach_(std::move(ws_attach)), dispatch_(std::move(dispatch)) {}
    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/");
    WS_PATH_ADD("/ws");
    WS_PATH_LIST_END
    void handleNewConnection(const drogon::HttpRequestPtr& req, const drogon::WebSocketConnectionPtr& conn) override {
        const auto kind = dispatch_(req->getLocalAddr().toPort());
        std::weak_ptr<drogon::WebSocketConnection> weak = conn;
        auto channel = std::make_shared<WebChannel>(executor_, [weak](std::string data) {
            if (auto c = weak.lock()) c->send(data, drogon::WebSocketMessageType::Binary);
        }, [weak] { if (auto c = weak.lock()) c->forceClose(); }, false);
        conn->setContext(channel);
        if (kind == Kind::lifecycle) {
            const std::string path = req->path();
            asio::post(executor_, [ws_attach=ws_attach_, channel, path] { ws_attach(channel, path); });
        } else {
            asio::post(executor_, [attach=attach_, channel] { attach(channel); });
        }
    }
    void handleNewMessage(const drogon::WebSocketConnectionPtr& conn, std::string&& data,
                          const drogon::WebSocketMessageType& type) override {
        if (type == drogon::WebSocketMessageType::Text || type == drogon::WebSocketMessageType::Binary)
            conn->getContext<WebChannel>()->incoming(std::move(data));
    }
    void handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) override {
        if (conn->hasContext()) conn->getContext<WebChannel>()->peer_closed();
        conn->clearContext();
    }
private:
    asio::any_io_executor executor_;
    Attach attach_;
    WsAttach ws_attach_;
    Dispatch dispatch_;
};
}
void stop_web() { drogon::app().quit(); }
void run_web(const Config& config, asio::any_io_executor executor, Attach attach, WsAttach ws_attach,
             HttpHandler http_handler, std::function<void()> ready) {
    auto& app = drogon::app();
    const bool script_role = config.role == "gameServer" || config.role == "apiServer";
    app.disableSigtermHandling().setThreadNum(1).setLogLevel(trantor::Logger::kWarn)
       .setMaxConnectionNum(config.max_connections).setIdleConnectionTimeout(60)
       .setClientMaxBodySize(max_frame).setClientMaxMemoryBodySize(max_frame)
       // Drogon rejects when outstanding + parsed >= threshold: 2 permits one request.
       .setClientMaxWebSocketMessageSize(max_frame).setPipeliningRequestsNumber(2);
    // One Drogon listener per config entry; TLS entries carry their effective
    // certificate pair (listener override, else the global --cert/--key).
    std::map<std::uint16_t, SocketController::Kind> dispatch;
    for (const auto& item : config.listeners) {
        if (item.protocol == "tcp") continue; // framed listeners stay with the node's own acceptors
        const bool tls = item.protocol == "wss" || item.protocol == "https";
        app.addListener(item.endpoint.address, item.endpoint.port, tls,
            tls ? (item.certificate.empty() ? config.certificate : item.certificate) : std::string(),
            tls ? (item.private_key.empty() ? config.private_key : item.private_key) : std::string());
        if (item.protocol == "ws" || item.protocol == "wss")
            dispatch.emplace(item.endpoint.port, SocketController::Kind::game);
        else if (item.protocol == "http" || item.protocol == "https")
            // First writer wins; the CLI rejects duplicate ports across listeners.
            dispatch.emplace(item.endpoint.port, script_role && ws_attach
                ? SocketController::Kind::lifecycle : SocketController::Kind::game);
    }
    app.setConnectionCallback([](const trantor::TcpConnectionPtr& conn) {
        if (conn->connected()) conn->setHighWaterMarkCallback(
            [](const trantor::TcpConnectionPtr& c, size_t) { c->forceClose(); }, 2*max_frame);
    });
    if (!dispatch.empty()) {
        auto game_attach = attach;
        app.registerController(std::make_shared<SocketController>(executor, std::move(game_attach), ws_attach,
            [dispatch](std::uint16_t port) {
                const auto found = dispatch.find(port);
                return found == dispatch.end() ? SocketController::Kind::game : found->second;
            }));
    }
    const bool any_http = std::any_of(config.listeners.begin(), config.listeners.end(), [](const Listener& item) {
        return item.protocol == "http" || item.protocol == "https"; });
    if (any_http && script_role) {
        auto pending = std::make_shared<std::atomic<std::size_t>>(0);
        app.registerHandlerViaRegex("/.*", [http_handler, pending, limit=config.max_connections](const drogon::HttpRequestPtr& req,
                            std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            if (pending->fetch_add(1) >= limit) {
                --*pending;
                auto response = drogon::HttpResponse::newHttpResponse();
                response->setStatusCode(drogon::k503ServiceUnavailable);
                return callback(response);
            }
            WebRequest request{req->methodString(), req->path(), req->query(), std::string(req->body()), {}};
            for (const auto& [name, value] : req->headers()) request.headers.emplace(name, value);
            http_handler(std::move(request), [pending, callback=std::move(callback)](WebResponse value) {
                --*pending;
                auto response = drogon::HttpResponse::newHttpResponse();
                response->setStatusCode(static_cast<drogon::HttpStatusCode>(value.status));
                response->setContentTypeCode(drogon::CT_TEXT_PLAIN);
                for (const auto& [name, header] : value.headers) response->addHeader(name, header);
                response->setBody(std::move(value.body));
                callback(response);
            });
        }, {drogon::Get, drogon::Post, drogon::Put, drogon::Delete, drogon::Patch, drogon::Options, drogon::Head});
    } else if (any_http && config.role == "gateway") {
        app.registerHandler("/message", [executor, attach](const drogon::HttpRequestPtr& req,
                            std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            // Each HTTP request is a separate service session. No implicit user affinity.
            auto complete = std::make_shared<bool>(false); // only touched by the Asio loop
            auto respond = [callback=std::move(callback), complete](drogon::HttpStatusCode status, std::string body) {
                if (std::exchange(*complete, true)) return;
                auto response = drogon::HttpResponse::newHttpResponse();
                response->setStatusCode(status);
                response->setContentTypeCode(drogon::CT_APPLICATION_OCTET_STREAM);
                response->setBody(std::move(body));
                callback(response);
            };
            auto channel = std::make_shared<WebChannel>(executor,
                [respond](std::string data) { respond(drogon::k200OK, std::move(data)); },
                [respond] { respond(drogon::k503ServiceUnavailable, "service unavailable"); }, true);
            asio::post(executor, [attach, channel] { attach(channel); });
            channel->incoming(std::string(req->body()));
        }, {drogon::Post});
    }
    app.registerBeginningAdvice([executor, ready=std::move(ready)] { asio::post(executor, ready); });
    app.run();
}
}
