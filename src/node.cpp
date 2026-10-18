#include "mistlane/node.hpp"
#include "mistlane/cross.hpp"
#include "mistlane/diagnostics.hpp"
#include "mistlane/lua_service.hpp"
#include "mistlane/token_bucket.hpp"
#include "mistlane/transport.hpp"
#include <utility>
#include <boost/asio.hpp>
#include <array>
#include <csignal>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <thread>

namespace mistlane {
namespace {
namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using Error = boost::system::error_code;
// Replaces the old mixed TCP/WebSocket framing. Wire format: u32 BE + payload.
class TcpChannel : public Channel, public std::enable_shared_from_this<TcpChannel> {
public:
    explicit TcpChannel(tcp::socket socket) : socket_(std::move(socket)) {}
    tcp::socket& socket() { return socket_; }
    asio::any_io_executor executor() override { return socket_.get_executor(); }
    void start(Completion callback) override { callback({}); }
    using Reader = std::function<void(Error, std::string)>;
    void read(Reader callback) {
        auto self = shared_from_this();
        asio::async_read(socket_, asio::buffer(header_),
          [self, callback=std::move(callback)](Error ec, std::size_t) mutable {
            if (ec) return callback(ec, {});
            std::size_t length = 0;
            for (unsigned char ch : self->header_) length = (length << 8) | ch;
            if (length > max_frame) return callback(asio::error::message_size, {});
            auto payload = std::make_shared<std::string>(length, '\0');
            asio::async_read(self->socket_, asio::buffer(*payload),
              [self, payload, callback=std::move(callback)](Error error, std::size_t) mutable {
                callback(error, std::move(*payload));
              });
          });
    }
    void write(std::string payload, std::function<void(Error)> callback) {
        if (payload.size() > max_frame) return callback(asio::error::message_size);
        auto frame = std::make_shared<std::string>(4, '\0');
        auto n = static_cast<std::uint32_t>(payload.size());
        for (int i = 3; i >= 0; --i) { (*frame)[i] = static_cast<char>(n & 255); n >>= 8; }
        frame->append(payload);
        auto self = shared_from_this();
        asio::async_write(socket_, asio::buffer(*frame),
          [self, frame, callback=std::move(callback)](Error ec, std::size_t) { callback(ec); });
    }
    void close() {
        Error ignored;
        socket_.cancel(ignored);
        socket_.shutdown(tcp::socket::shutdown_both, ignored);
        socket_.close(ignored);
    }
private:
    tcp::socket socket_;
    std::array<unsigned char, 4> header_{};
};
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(std::shared_ptr<Channel> client, std::shared_ptr<LuaService>& service,
            const Endpoint* upstream, const Config& config, CrossAdmission& admission,
            Diagnostics& diagnostics,
            std::function<void(Session*)> on_close)
      : client_(std::move(client)), service_(service),
        timer_(client_->executor()), config_(config), admission_(admission), diagnostics_(diagnostics),
        on_close_(std::move(on_close)) {
        if (upstream) {
            endpoint_ = tcp::endpoint(asio::ip::make_address(upstream->address), upstream->port);
            upstream_ = std::make_shared<TcpChannel>(tcp::socket(client_->executor()));
        }
    }
    void start() {
        deadline(10);
        client_->start([self=shared_from_this()](Error ec) {
            if (self->closed_) return;
            if (ec) self->stop(); else self->next();
        });
    }
    void stop() {
        if (closed_) return;
        closed_ = true;
        timer_.cancel();
        client_->close();
        if (upstream_) upstream_->close();
        on_close_(this);
    }
private:
    void deadline(int seconds) {
        auto generation = ++generation_;
        timer_.expires_after(std::chrono::seconds(seconds));
        timer_.async_wait([self=shared_from_this(), generation](Error ec) {
            if (!ec && self->generation_ == generation) self->stop();
        });
    }
    void next() {
        if (closed_) return;
        deadline(60);
        client_->read([self=shared_from_this()](Error ec, std::string payload) {
            if (self->closed_) return;
            if (ec || !self->bucket_.consume()) return self->stop();
            self->deadline(10);
            if ((self->config_.role == "cross" || self->config_.role == "shard") && !self->joined_) {
                try {
                    self->context_ = cross_context(payload);
                    if (self->context_.epoch != self->config_.route_epoch) return self->reject("EPOCH");
                    auto index = self->config_.role == "cross"
                        ? cross_shard(self->context_, self->config_.upstreams.size()) : 0;
                    auto error = self->admission_.acquire(self.get(), self->context_, index);
                    if (!error.empty()) return self->reject(error);
                    self->joined_ = true;
                    self->awaiting_join_ = self->config_.role == "cross";
                    if (self->config_.role == "shard")
                        return self->reply("MLC1 OK " + std::to_string(self->config_.route_epoch));
                    const auto& endpoint = self->config_.upstreams[index];
                    self->endpoint_ = tcp::endpoint(asio::ip::make_address(endpoint.address), endpoint.port);
                    self->upstream_ = std::make_shared<TcpChannel>(tcp::socket(self->client_->executor()));
                } catch (const std::exception&) { return self->reject("INVALID"); }
            }
            if (!self->upstream_) {
                try {
                    const auto started = std::chrono::steady_clock::now();
                    auto response = self->service_->message(payload, self->joined_ ? &self->context_ : nullptr);
                    self->diagnostics_.observe_lua_callback("message", std::chrono::steady_clock::now() - started);
                    self->reply(std::move(response));
                }
                catch (const std::exception& e) {
                    std::cerr << "callback failed: " << e.what() << '\n';
                    self->stop();
                }
                return;
            }
            if (self->upstream_->socket().is_open()) self->forward(std::move(payload));
            else {
                self->upstream_->socket().async_connect(self->endpoint_,
                  [self, payload=std::move(payload)](Error error) mutable {
                    if (self->closed_) return;
                    if (error) return self->stop();
                    self->forward(std::move(payload));
                  });
            }
        });
    }
    void reject(const std::string& reason) {
        std::cerr << "cross admission rejected: " << reason << '\n';
        client_->write("MLC1 ERR " + reason, [self=shared_from_this()](Error) { self->stop(); });
    }
    void forward(std::string payload) {
        upstream_->write(std::move(payload), [self=shared_from_this()](Error ec) {
            if (self->closed_) return;
            if (ec) return self->stop();
            self->upstream_->read([self](Error error, std::string response) {
                if (self->closed_) return;
                if (error) return self->stop();
                if (self->awaiting_join_) {
                    self->awaiting_join_ = false;
                    if (response != "MLC1 OK " + std::to_string(self->config_.route_epoch)) {
                        if (!response.starts_with("MLC1 ERR ")) return self->reject("UPSTREAM");
                        return self->client_->write(std::move(response), [self](Error) { self->stop(); });
                    }
                }
                self->reply(std::move(response));
            });
        });
    }
    void reply(std::string payload) {
        client_->write(std::move(payload), [self=shared_from_this()](Error ec) {
            if (self->closed_) return;
            if (ec) self->stop();
            else self->next();
        });
    }
    std::shared_ptr<Channel> client_;
    std::shared_ptr<TcpChannel> upstream_;
    std::shared_ptr<LuaService>& service_;
    asio::steady_timer timer_;
    tcp::endpoint endpoint_;
    const Config& config_;
    CrossAdmission& admission_;
    Diagnostics& diagnostics_;
    CrossContext context_;
    bool joined_ = false, awaiting_join_ = false;
    std::function<void(Session*)> on_close_;
    TokenBucket bucket_;
    std::uint64_t generation_ = 0;
    bool closed_ = false;
};
class Node {
public:
    explicit Node(Config config)
      : config_(std::move(config)), acceptor_(io_), signals_(io_, SIGINT, SIGTERM, SIGHUP), tick_(io_),
        admission_(std::max<std::size_t>(1, config_.upstreams.size()), config_.max_room_sessions,
                   config_.max_group_sessions, config_.max_shard_sessions) {
        diagnostics_ = std::make_unique<Diagnostics>(config_);
        // Reserve an admission envelope before allocating protocol buffers. Not a hard RSS quota.
        constexpr std::size_t session_envelope = 6*max_frame + 64*1024;
        config_.max_connections = std::min(config_.max_connections,
            config_.memory_budget_mib*1024*1024/session_envelope);
        if (!config_.max_connections) throw std::invalid_argument("memory budget too small for one session");
        std::cout << "session admission limit " << config_.max_connections << std::endl;
        if (config_.transport == "wss" || config_.transport == "https") {
            tls_ = std::make_unique<asio::ssl::context>(asio::ssl::context::tls_server);
            if (!SSL_CTX_set_min_proto_version(tls_->native_handle(), TLS1_2_VERSION))
                throw std::runtime_error("cannot configure minimum TLS version");
            tls_->use_certificate_chain_file(config_.certificate);
            tls_->use_private_key_file(config_.private_key, asio::ssl::context::pem);
            if (!SSL_CTX_check_private_key(tls_->native_handle()))
                throw std::runtime_error("TLS private key does not match certificate");
        }
        if (config_.redis_enabled) {
            redis_ = std::make_shared<RedisRuntime>(io_, config_.redis.address, config_.redis.port);
            redis_->start();
        }
        if (config_.role == "worker" || config_.role == "shard") service_ = std::make_shared<LuaService>(config_.script, ServiceContext{config_.role, config_.transport, config_.route_epoch}, redis_);
        for (const auto& e : config_.upstreams) (void)asio::ip::make_address(e.address);
        if (config_.transport == "tcp") {
            auto endpoint = tcp::endpoint(asio::ip::make_address(config_.listen.address), config_.listen.port);
            acceptor_.open(endpoint.protocol());
            acceptor_.set_option(asio::socket_base::reuse_address(true));
            acceptor_.bind(endpoint);
            acceptor_.listen();
        }
    }
    void run() {
        signal();
        tick();
        if (config_.transport == "tcp") {
            if (service_) service_->start();
            accept();
            diagnostics_->set_ready(true);
            std::cout << "ready " << config_.role << ' ' << config_.listen.address << ':' << config_.listen.port << std::endl;
            io_.run();
        } else {
            std::thread worker([this] { io_.run(); });
            try {
                run_web(config_, io_.get_executor(), [this](std::shared_ptr<Channel> c) { attach(std::move(c)); },
                    [this](WebRequest request, HttpCompletion complete) {
                        asio::post(io_, [this, request=std::move(request), complete=std::move(complete)]() mutable {
                            if (stopping_ || !service_) return complete({503, {}, "Service Unavailable"});
                            const auto started = std::chrono::steady_clock::now();
                            auto response = service_->http(request);
                            diagnostics_->observe_lua_callback("http", std::chrono::steady_clock::now() - started);
                            complete(std::move(response));
                        });
                    }, [this] {
                        try {
                            if (service_) service_->start();
                            diagnostics_->set_ready(true);
                            std::cout << "ready " << config_.role << ' ' << config_.listen.address << ':' << config_.listen.port << std::endl;
                        } catch (...) { startup_error_ = std::current_exception(); stop(); }
                    });
            }
            catch (...) { asio::post(io_, [this] { stop(); }); worker.join(); throw; }
            asio::post(io_, [this] { stop(); });
            worker.join();
            if (startup_error_) std::rethrow_exception(startup_error_);
        }
    }
private:
    void attach(std::shared_ptr<Channel> client) {
        if (stopping_ || sessions_.size() >= config_.max_connections) return client->close();
        const Endpoint* upstream = config_.upstreams.empty() || config_.role == "cross" ? nullptr
            : &config_.upstreams[cursor_++ % config_.upstreams.size()];
        auto session = std::make_shared<Session>(std::move(client), service_, upstream, config_, admission_, *diagnostics_,
            [this](Session* ptr) {
                admission_.release(ptr);
                for (auto i = sessions_.begin(); i != sessions_.end(); ++i)
                    if (i->get() == ptr) { sessions_.erase(i); break; }
                diagnostics_->set_active_sessions(sessions_.size());
            });
        sessions_.insert(session);
        diagnostics_->set_active_sessions(sessions_.size());
        session->start();
    }
    void accept() {
        acceptor_.async_accept([this](Error ec, tcp::socket socket) {
            if (stopping_) return;
            if (!ec && sessions_.size() < config_.max_connections) {
                attach(std::make_shared<TcpChannel>(std::move(socket)));
            } else if (!ec) { diagnostics_->reject("connection_limit"); Error ignored; socket.close(ignored); }
            // Resource exhaustion should fail visibly rather than spin the event loop.
            if (ec && ec != asio::error::operation_aborted) {
                std::cerr << "accept failed: " << ec.message() << '\n';
                stop();
                return;
            }
            if (!ec) accept();
        });
    }
    void signal() {
        signals_.async_wait([this](Error ec, int value) {
            if (ec || stopping_) return;
            if (value == SIGHUP) {
                if (service_) {
                    try {
                        auto replacement = std::make_shared<LuaService>(config_.script, ServiceContext{config_.role, config_.transport, config_.route_epoch}, redis_);
                        replacement->start();
                        service_.swap(replacement);
                        std::cout << "script reloaded; module state reset" << std::endl;
                    } catch (const std::exception& e) {
                        std::cerr << "reload rejected, previous service retained: " << e.what() << '\n';
                    }
                }
                signal();
            } else stop();
        });
    }
    void tick() {
        tick_.expires_after(std::chrono::milliseconds(100));
        tick_.async_wait([this](Error ec) {
            if (ec || stopping_) return;
            if (service_) {
                try { service_->tick(); }
                catch (const std::exception& e) { std::cerr << "tick failed: " << e.what() << '\n'; }
            }
            diagnostics_->refresh_process_memory();
            tick();
        });
    }
    void stop() {
        if (stopping_) return;
        stopping_ = true;
        diagnostics_->set_ready(false);
        Error ignored;
        acceptor_.close(ignored);
        if (service_) {
            try { service_->stop(); }
            catch (const std::exception& e) { std::cerr << "stop failed: " << e.what() << '\n'; }
        }
        if (redis_) redis_->stop();
        if (config_.transport != "tcp") stop_web();
        signals_.cancel();
        tick_.cancel();
        while (!sessions_.empty()) {
            auto session = *sessions_.begin();
            session->stop();
        }
        if (service_) {
            try { service_->shutdown(); }
            catch (const std::exception& e) { std::cerr << "shutdown failed: " << e.what() << '\n'; }
        }
        // Drain cancellation handlers before destroying Lua/Node state.
    }
    Config config_;
    asio::io_context io_;
    tcp::acceptor acceptor_;
    asio::signal_set signals_;
    asio::steady_timer tick_;
    std::unique_ptr<asio::ssl::context> tls_;
    std::shared_ptr<LuaService> service_;
    std::shared_ptr<RedisRuntime> redis_;
    std::set<std::shared_ptr<Session>> sessions_;
    std::size_t cursor_ = 0;
    CrossAdmission admission_;
    std::unique_ptr<Diagnostics> diagnostics_;
    std::exception_ptr startup_error_;
    bool stopping_ = false;
};
}
int run_node(Config config) { Node node(std::move(config)); node.run(); return 0; }
}
