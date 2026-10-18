// SPDX-License-Identifier: Apache-2.0
#include "cli_client.hpp"
#include "mistlane/http_client.hpp"
#include "mistlane/transport.hpp"
#include <curl/curl.h>
#include <drogon/WebSocketClient.h>
#include <trantor/net/EventLoopThread.h>
#include <array>
#include <atomic>
#include <future>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace mistlane::cli {
namespace {
namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using Error = boost::system::error_code;
struct Url {
    std::string scheme, host, port, origin, path;
};
Url parse(const std::string& value) {
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> handle(curl_url(), curl_url_cleanup);
    if (!handle || value.find('\0') != std::string::npos ||
        curl_url_set(handle.get(), CURLUPART_URL, value.c_str(), CURLU_NON_SUPPORT_SCHEME) != CURLUE_OK)
        throw std::invalid_argument("expected tcp://host:port or ws/wss/http/https URL");
    auto get = [&](CURLUPart part) {
        char* raw = nullptr;
        if (curl_url_get(handle.get(), part, &raw, 0) != CURLUE_OK) return std::string{};
        std::string result(raw);
        curl_free(raw);
        return result;
    };
    if (!get(CURLUPART_USER).empty() || !get(CURLUPART_PASSWORD).empty() ||
        !get(CURLUPART_FRAGMENT).empty()) throw std::invalid_argument("URL credentials/fragments are unsupported");
    Url url{get(CURLUPART_SCHEME), get(CURLUPART_HOST), get(CURLUPART_PORT), {}, get(CURLUPART_PATH)};
    if (url.scheme != "tcp" && url.scheme != "ws" && url.scheme != "wss" &&
        url.scheme != "http" && url.scheme != "https") throw std::invalid_argument("unsupported URL scheme");
    if (url.port.empty()) {
        if (url.scheme == "tcp") throw std::invalid_argument("TCP URL requires a port");
        url.port = url.scheme == "wss" || url.scheme == "https" ? "443" : "80";
    }
    url.origin = url.scheme + "://" + url.host + ":" + url.port;
    if (url.path.empty()) url.path = "/";
    const auto query = get(CURLUPART_QUERY);
    if (!query.empty()) url.path += "?" + query;
    if (url.host.starts_with('[')) url.host = url.host.substr(1, url.host.size()-2);
    if (url.scheme == "tcp" && (url.path != "/" || !query.empty()))
        throw std::invalid_argument("TCP URLs have no request path");
    return url;
}
class TcpClient final : public Client {
public:
    TcpClient(const Url& url, unsigned timeout)
        : socket_(io_), resolver_(io_), timer_(io_), timeout_(timeout) {
        perform([&](auto complete) {
            resolver_.async_resolve(url.host, url.port,
                [&, complete](Error ec, tcp::resolver::results_type addresses) {
                    if (ec) return complete(ec);
                    asio::async_connect(socket_, addresses, [complete](Error error, const tcp::endpoint&) { complete(error); });
                });
        });
    }
    std::string request(std::string_view payload, unsigned timeout_ms) override {
        if (timeout_ms) timeout_=std::min(timeout_,timeout_ms);
        if (payload.size() > max_frame) throw std::invalid_argument("payload exceeds 1 MiB");
        std::string frame(4, '\0'), response;
        const auto length = static_cast<std::uint32_t>(payload.size());
        for (unsigned i = 0; i < 4; ++i) frame[i] = static_cast<char>(length >> (24-i*8));
        frame.append(payload);
        std::array<unsigned char, 4> header{};
        perform([&](auto complete) {
            asio::async_write(socket_, asio::buffer(frame), [&, complete](Error ec, std::size_t) {
                if (ec) return complete(ec);
                asio::async_read(socket_, asio::buffer(header), [&, complete](Error error, std::size_t) {
                    if (error) return complete(error);
                    std::size_t n = 0;
                    for (auto ch : header) n = (n << 8) | ch;
                    if (n > max_frame) return complete(asio::error::message_size);
                    response.resize(n);
                    asio::async_read(socket_, asio::buffer(response), [complete](Error e, std::size_t) { complete(e); });
                });
            });
        });
        return response;
    }
private:
    template<class Start> void perform(Start start) {
        Error result;
        bool timed_out = false;
        io_.restart();
        timer_.expires_after(std::chrono::milliseconds(timeout_));
        timer_.async_wait([&](Error ec) {
            if (!ec) {
                timed_out = true;
                resolver_.cancel();
                Error ignored;
                socket_.close(ignored);
            }
        });
        start([&](Error ec) { result = ec; timer_.cancel(); });
        io_.run();
        if (timed_out) throw std::runtime_error("TCP operation timed out");
        if (result) throw std::runtime_error("TCP: " + result.message());
    }
    asio::io_context io_;
    tcp::socket socket_;
    tcp::resolver resolver_;
    asio::steady_timer timer_;
    unsigned timeout_;
};
struct Pending {
    std::promise<std::string> promise;
    std::atomic<bool> done{false};
    void success(std::string value) { if (!done.exchange(true)) promise.set_value(std::move(value)); }
    void fail(const std::string& error) {
        if (!done.exchange(true)) promise.set_exception(std::make_exception_ptr(std::runtime_error(error)));
    }
};
struct SocketState {
    std::mutex mutex;
    std::shared_ptr<Pending> pending;
};
class WsClient final : public Client {
public:
    WsClient(const Url& url, const ClientOptions& options)
        : timeout_(options.timeout_ms), state_(std::make_shared<SocketState>()) {
        loop_.run();
        client_ = drogon::WebSocketClient::newWebSocketClient(url.origin, loop_.getLoop(), false, true);
        if (!client_) throw std::runtime_error("cannot create Drogon WebSocket client");
        if (!options.ca_file.empty()) client_->addSSLConfigs({{"VerifyCAFile", options.ca_file}});
        client_->setMessageHandler([state=state_](std::string&& data, const auto&, const auto& type) {
            if (type != drogon::WebSocketMessageType::Binary && type != drogon::WebSocketMessageType::Text) return;
            std::shared_ptr<Pending> pending;
            { std::lock_guard lock(state->mutex); pending = state->pending; }
            if (pending) {
                if (data.size() > max_frame) pending->fail("WS response exceeds 1 MiB");
                else pending->success(std::move(data));
            }
        });
        client_->setConnectionClosedHandler([state=state_](const auto&) {
            std::lock_guard lock(state->mutex);
            if (state->pending) state->pending->fail("WebSocket peer closed");
        });
        auto ready = std::make_shared<Pending>();
        auto future = ready->promise.get_future();
        auto request = drogon::HttpRequest::newHttpRequest();
        request->setPath(url.path);
        client_->connectToServer(request, [ready](auto result, const auto&, const auto&) {
            if (result == drogon::ReqResult::Ok) ready->success({});
            else ready->fail("WebSocket handshake/TLS failed");
        });
        try { (void)await(future); }
        catch (...) { client_->stop(); throw; }
    }
    ~WsClient() override { client_->stop(); }
    std::string request(std::string_view payload, unsigned timeout_ms) override {
        if (payload.size() > max_frame) throw std::invalid_argument("payload exceeds 1 MiB");
        auto pending = std::make_shared<Pending>();
        auto future = pending->promise.get_future();
        { std::lock_guard lock(state_->mutex); state_->pending = pending; }
        loop_.getLoop()->queueInLoop([client=client_, pending, data=std::string(payload)] {
            auto connection = client->getConnection();
            if (!connection || !connection->connected()) return pending->fail("WebSocket is disconnected");
            connection->send(data, drogon::WebSocketMessageType::Binary);
        });
        return await(future,timeout_ms);
    }
private:
    std::string await(std::future<std::string>& future, unsigned timeout_ms = 0) {
        if (future.wait_for(std::chrono::milliseconds(timeout_ms?std::min(timeout_,timeout_ms):timeout_)) != std::future_status::ready)
            throw std::runtime_error("WebSocket operation timed out");
        return future.get();
    }
    trantor::EventLoopThread loop_;
    drogon::WebSocketClientPtr client_;
    unsigned timeout_;
    std::shared_ptr<SocketState> state_;
};
class HttpClient final : public Client {
public:
    explicit HttpClient(ClientOptions options) : options_(std::move(options)) {}
    std::string request(std::string_view payload, unsigned timeout_ms) override {
        const auto timeout=static_cast<long>(timeout_ms?std::min(options_.timeout_ms,timeout_ms):options_.timeout_ms);
        auto response = http_request({options_.url, options_.method, std::string(payload), options_.ca_file,
                                      timeout, timeout, max_frame});
        if (response.status < 200 || response.status >= 300)
            throw std::runtime_error("HTTP status " + std::to_string(response.status));
        return response.body;
    }
private:
    ClientOptions options_;
};
}
std::unique_ptr<Client> make_client(const ClientOptions& options) {
    if (options.timeout_ms == 0 || options.timeout_ms > 60000)
        throw std::invalid_argument("timeout must be between 1 and 60000 milliseconds");
    const auto url = parse(options.url);
    if (url.scheme == "tcp") return std::make_unique<TcpClient>(url, options.timeout_ms);
    if (url.scheme == "ws" || url.scheme == "wss") return std::make_unique<WsClient>(url, options);
    return std::make_unique<HttpClient>(options);
}
}
