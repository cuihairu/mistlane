#include "mistlane/node.hpp"
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
            const Endpoint* upstream, std::function<void(Session*)> on_close)
      : client_(std::move(client)), service_(service),
        timer_(client_->executor()), on_close_(std::move(on_close)) {
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
            if (!self->upstream_) {
                try { self->reply(self->service_->message(payload)); }
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
    void forward(std::string payload) {
        upstream_->write(std::move(payload), [self=shared_from_this()](Error ec) {
            if (self->closed_) return;
            if (ec) return self->stop();
            self->upstream_->read([self](Error error, std::string response) {
                if (self->closed_) return;
                if (error) return self->stop();
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
    std::function<void(Session*)> on_close_;
    TokenBucket bucket_;
    std::uint64_t generation_ = 0;
    bool closed_ = false;
};
class Node {
public:
    explicit Node(Config config)
      : config_(std::move(config)), acceptor_(io_), signals_(io_, SIGINT, SIGTERM, SIGHUP), tick_(io_) {
        if (config_.transport == "wss") {
            tls_ = std::make_unique<asio::ssl::context>(asio::ssl::context::tls_server);
            if (!SSL_CTX_set_min_proto_version(tls_->native_handle(), TLS1_2_VERSION))
                throw std::runtime_error("cannot configure minimum TLS version");
            tls_->use_certificate_chain_file(config_.certificate);
            tls_->use_private_key_file(config_.private_key, asio::ssl::context::pem);
            if (!SSL_CTX_check_private_key(tls_->native_handle()))
                throw std::runtime_error("TLS private key does not match certificate");
        }
        if (config_.role == "worker") service_ = std::make_shared<LuaService>(config_.script);
        for (const auto& e : config_.upstreams) (void)asio::ip::make_address(e.address);
        auto endpoint = tcp::endpoint(asio::ip::make_address(config_.listen.address), config_.listen.port);
        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(asio::socket_base::reuse_address(true));
        acceptor_.bind(endpoint);
        acceptor_.listen();
    }
    void run() {
        accept();
        signal();
        tick();
        std::cout << "ready " << config_.role << ' ' << config_.listen.address << ':' << config_.listen.port << std::endl;
        io_.run();
    }
private:
    void accept() {
        acceptor_.async_accept([this](Error ec, tcp::socket socket) {
            if (stopping_) return;
            if (!ec && sessions_.size() < config_.max_connections) {
                const Endpoint* upstream = config_.upstreams.empty() ? nullptr
                    : &config_.upstreams[cursor_++ % config_.upstreams.size()];
                std::shared_ptr<Channel> client;
                if (config_.transport == "tcp") client = std::make_shared<TcpChannel>(std::move(socket));
                else client = make_websocket(std::move(socket), tls_.get());
                auto session = std::make_shared<Session>(std::move(client), service_, upstream,
                    [this](Session* ptr) {
                        for (auto i = sessions_.begin(); i != sessions_.end(); ++i)
                            if (i->get() == ptr) { sessions_.erase(i); break; }
                    });
                sessions_.insert(session);
                session->start();
            } else if (!ec) { Error ignored; socket.close(ignored); }
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
                        auto replacement = std::make_shared<LuaService>(config_.script);
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
            tick();
        });
    }
    void stop() {
        if (stopping_) return;
        stopping_ = true;
        Error ignored;
        acceptor_.close(ignored);
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
    std::set<std::shared_ptr<Session>> sessions_;
    std::size_t cursor_ = 0;
    bool stopping_ = false;
};
}
int run_node(Config config) { Node node(std::move(config)); node.run(); return 0; }
}
