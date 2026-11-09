// SPDX-License-Identifier: Apache-2.0
// Probe and dashboard listeners, extracted from the node implementation.
// Both are read-only bounded surfaces: one request line per connection, an
// inline answer on the event loop, and hard caps on peers, bytes and time.
#pragma once
#include "node_internal.hpp"
#include "mistlane/dashboard.hpp"
#include <chrono>
#include <cctype>
#include <functional>
#include <istream>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mistlane {

// Internal probe protocol: one request line per connection, answered inline on
// the event loop, so a timely reply doubles as liveness evidence. It never
// shares a connection or a Lua handler with player sessions.
//   request:  PROBE1 <nonce> liveness|readiness [token]
//   response: PROBE1 <nonce> ok alive
//             PROBE1 <nonce> ok ready|notready active=N capacity=N lag_ms=F
//             PROBE1 <nonce> err version|auth|command|malformed
class ProbeServer : public std::enable_shared_from_this<ProbeServer> {
public:
    using Answer = std::function<std::string(std::string_view command)>;
    // One request line per connection: enough for the longest nonce plus a
    // probe secret, small enough that a stalled or hostile client cannot grow
    // unbounded buffers or exhaust the node's peer slots.
    static constexpr std::size_t max_peers = 64;
    static constexpr std::size_t probe_line_cap = 256;
    ProbeServer(asio::io_context& io, const Endpoint& listen, Answer answer)
      : acceptor_(io, tcp::endpoint(parse_address(listen.address), listen.port)),
        answer_(std::move(answer)) {}
    // Separate from the constructor: accept() needs shared_from_this() ownership.
    void start() { accept(); }
    void stop() {
        Error ignored;
        acceptor_.close(ignored);
        for (auto& peer : peers_) peer->close();
        peers_.clear();
    }
private:
    class Peer : public std::enable_shared_from_this<Peer> {
    public:
        Peer(tcp::socket socket, Answer answer)
          : socket_(std::move(socket)), timer_(socket_.get_executor()), answer_(std::move(answer)) {}
        void start(const std::shared_ptr<ProbeServer>& server) {
            server_ = server;
            read();
            expire();
        }
        void close() {
            Error ignored;
            timer_.cancel();
            socket_.close(ignored);
        }
    private:
        void read() {
            auto self = shared_from_this();
            asio::async_read_until(socket_, buffer_, '\n',
              [self](Error ec, std::size_t) {
                if (ec) return self->finish();
                std::istream line(&self->buffer_);
                std::string request;
                std::getline(line, request);
                self->reply(self->handle(request));
            });
        }
        void expire() {
            auto self = shared_from_this();
            timer_.expires_after(std::chrono::seconds(2));
            timer_.async_wait([self](Error ec) {
                if (ec) return;
                Error ignored;
                self->socket_.close(ignored);
            });
        }
        static bool nonce(std::string_view value) {
            if (value.empty() || value.size() > 64) return false;
            for (char ch : value)
                if (!(std::isalnum(static_cast<unsigned char>(ch)) ||
                      ch == '_' || ch == '-' || ch == '.' || ch == ':')) return false;
            return true;
        }
        std::string handle(std::string_view request) {
            std::vector<std::string_view> fields;
            for (auto item = request.data(); item != request.data()+request.size();) {
                while (item != request.data()+request.size() && (*item==' '||*item=='\t')) ++item;
                auto end = item;
                while (end != request.data()+request.size() && *end!=' ' && *end!='\t') ++end;
                if (end != item) fields.emplace_back(item, end-item);
                item = end;
            }
            const std::string_view nonce_text = fields.size() > 1 ? fields[1] : std::string_view("-");
            const auto error = [&](std::string_view reason) {
                return "PROBE1 " + std::string(nonce_text) + " err " + std::string(reason);
            };
            if (fields.empty()) return error("malformed");
            if (!nonce(nonce_text)) return "PROBE1 - err malformed";
            if (fields.front() != "PROBE1") return error("version");
            const char* expected = ::getenv("MISTLANE_PROBE_TOKEN");
            std::string_view command;
            if (expected && *expected) {
                if (fields.size() != 4 || fields[2] != expected) return error("auth");
                command = fields[3];
            } else {
                if (fields.size() != 3) return error("malformed");
                command = fields[2];
            }
            if (command != "liveness" && command != "readiness") return error("command");
            return "PROBE1 " + std::string(nonce_text) + " " + answer_(command);
        }
        void reply(std::string text) {
            // asio::buffer never copies: the reply line must outlive the write.
            auto line = std::make_shared<std::string>(std::move(text));
            line->push_back('\n');
            auto self = shared_from_this();
            asio::async_write(socket_, asio::buffer(*line),
              [self, line](Error, std::size_t) { self->finish(); });
        }
        // Releases the peer slot only after the last handler frame: callers hold
        // their own `self`, so erase() cannot destroy us mid-handler.
        void finish() {
            if (finished_) return;
            finished_ = true;
            Error ignored;
            timer_.cancel();
            socket_.close(ignored);
            if (auto server = server_) server->retire(shared_from_this());
        }
        tcp::socket socket_;
        asio::steady_timer timer_;
        // One short request line per connection; oversized input closes the peer.
        asio::streambuf buffer_{probe_line_cap};
        Answer answer_;
        std::shared_ptr<ProbeServer> server_;
        bool finished_ = false;
    };
    void retire(const std::shared_ptr<Peer>& peer) { peers_.erase(peer); }
    void accept() {
        auto self = shared_from_this();
        acceptor_.async_accept([self](Error ec, tcp::socket socket) {
            if (ec) return;
            if (self->peers_.size() >= max_peers) {
                Error ignored;
                socket.close(ignored);
            } else {
                auto peer = std::make_shared<Peer>(std::move(socket), self->answer_);
                self->peers_.insert(peer);
                peer->start(self);
            }
            self->accept();
        });
    }
    tcp::acceptor acceptor_;
    Answer answer_;
    std::set<std::shared_ptr<Peer>> peers_;
};

// The embedded operations dashboard: a minimal HTTP listener serving the
// self-contained page and the same Prometheus text the --metrics-listen
// exposer serves. Read-only by construction — there is no write route and no
// script in the path — so the page can observe the node without being able to
// perturb it. Requests parse only as far as the first line; anything else is a
// 404, and every connection is bounded in count, bytes and time like a probe.
class DashboardServer : public std::enable_shared_from_this<DashboardServer> {
public:
    using Metrics = std::function<std::string()>;
    static constexpr std::size_t max_peers = 64;
    static constexpr std::size_t request_line_cap = 2048;
    DashboardServer(asio::io_context& io, const Endpoint& listen, Metrics metrics)
      : acceptor_(io, tcp::endpoint(parse_address(listen.address), listen.port)),
        metrics_(std::move(metrics)) {}
    void start() { accept(); }
    void stop() {
        Error ignored;
        acceptor_.close(ignored);
        for (auto& peer : peers_) peer->close();
        peers_.clear();
    }
private:
    class Peer : public std::enable_shared_from_this<Peer> {
    public:
        Peer(tcp::socket socket, Metrics metrics)
          : socket_(std::move(socket)), timer_(socket_.get_executor()), metrics_(std::move(metrics)) {}
        void start(const std::shared_ptr<DashboardServer>& server) {
            server_ = server;
            read();
            expire();
        }
        void close() {
            Error ignored;
            timer_.cancel();
            socket_.close(ignored);
        }
    private:
        void read() {
            auto self = shared_from_this();
            // One request line decides the route; headers and bodies are
            // irrelevant to a read-only surface and are never parsed.
            asio::async_read_until(socket_, buffer_, '\n',
              [self](Error ec, std::size_t) {
                if (ec) return self->finish();
                std::istream line(&self->buffer_);
                std::string request;
                std::getline(line, request);
                self->reply(self->handle(request));
            });
        }
        void expire() {
            auto self = shared_from_this();
            timer_.expires_after(std::chrono::seconds(5));
            timer_.async_wait([self](Error ec) {
                if (ec) return;
                Error ignored;
                self->socket_.close(ignored);
            });
        }
        std::pair<int, std::string> handle(std::string_view request) {
            // "GET /path HTTP/1.1" — method and path only. A slow-loris body
            // or pipelined garbage hits the 5s expiry instead of logic.
            const auto space = request.find(' ');
            const auto trailing = space == std::string_view::npos
                ? std::string_view{} : request.substr(space + 1);
            const auto path = trailing.substr(0, trailing.find(' '));
            if (request.substr(0, space) != "GET") return {405, "method not allowed\n"};
            if (path == "/" || path == "/dashboard")
                return {200, std::string(kDashboardHtml)};
            if (path == "/metrics") return {200, metrics_()};
            return {404, "not found\n"};
        }
        void reply(std::pair<int, std::string> answer) {
            const auto* reason = answer.first == 200 ? "OK" : answer.first == 404 ? "Not Found"
                : answer.first == 405 ? "Method Not Allowed" : "Error";
            const auto* type = answer.second.starts_with("<!doctype html>") ? "text/html; charset=utf-8"
                : "text/plain; version=0.0.4; charset=utf-8";
            // asio::buffer never copies: the response must outlive the write.
            auto body = std::make_shared<std::string>("HTTP/1.1 " + std::to_string(answer.first) + " " + reason
                + "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(answer.second.size())
                + "\r\nConnection: close\r\n\r\n" + answer.second);
            auto self = shared_from_this();
            asio::async_write(socket_, asio::buffer(*body),
              [self, body](Error, std::size_t) { self->finish(); });
        }
        void finish() {
            if (finished_) return;
            finished_ = true;
            Error ignored;
            timer_.cancel();
            socket_.close(ignored);
            if (auto server = server_) server->retire(shared_from_this());
        }
        tcp::socket socket_;
        asio::steady_timer timer_;
        asio::streambuf buffer_{request_line_cap};
        Metrics metrics_;
        std::shared_ptr<DashboardServer> server_;
        bool finished_ = false;
    };
    void retire(const std::shared_ptr<Peer>& peer) { peers_.erase(peer); }
    void accept() {
        auto self = shared_from_this();
        acceptor_.async_accept([self](Error ec, tcp::socket socket) {
            if (ec) return;
            if (self->peers_.size() >= max_peers) {
                Error ignored;
                socket.close(ignored);
            } else {
                auto peer = std::make_shared<Peer>(std::move(socket), self->metrics_);
                self->peers_.insert(peer);
                peer->start(self);
            }
            self->accept();
        });
    }
    tcp::acceptor acceptor_;
    Metrics metrics_;
    std::set<std::shared_ptr<Peer>> peers_;
};

}
