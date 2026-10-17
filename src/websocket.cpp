#include "mistlane/transport.hpp"
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

namespace mistlane {
namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ws = beast::websocket;
using tcp = asio::ip::tcp;

template<bool Secure>
class WebSocketChannel final : public Channel,
    public std::enable_shared_from_this<WebSocketChannel<Secure>> {
    using Stream = std::conditional_t<Secure,
        beast::ssl_stream<beast::tcp_stream>, beast::tcp_stream>;
public:
    template<class... Args>
    explicit WebSocketChannel(Args&&... args) : stream_(std::forward<Args>(args)...) {
        stream_.read_message_max(max_frame);
        stream_.binary(true);
        stream_.set_option(ws::stream_base::timeout::suggested(beast::role_type::server));
    }
    asio::any_io_executor executor() override { return stream_.get_executor(); }
    void start(Completion callback) override {
        auto self = this->shared_from_this();
        if constexpr (Secure) {
            beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(10));
            stream_.next_layer().async_handshake(asio::ssl::stream_base::server,
                [self, callback=std::move(callback)](Error ec) mutable {
                    if (ec) return callback(ec);
                    beast::get_lowest_layer(self->stream_).expires_never();
                    self->accept(std::move(callback));
                });
        } else accept(std::move(callback));
    }
    void read(Reader callback) override {
        auto self = this->shared_from_this();
        stream_.async_read(buffer_, [self, callback=std::move(callback)](Error ec, std::size_t) mutable {
            std::string payload;
            if (!ec) payload = beast::buffers_to_string(self->buffer_.data());
            self->buffer_.consume(self->buffer_.size());
            callback(ec, std::move(payload));
        });
    }
    void write(std::string payload, Completion callback) override {
        if (payload.size() > max_frame) return callback(asio::error::message_size);
        auto data = std::make_shared<std::string>(std::move(payload));
        auto self = this->shared_from_this();
        stream_.async_write(asio::buffer(*data),
            [self, data, callback=std::move(callback)](Error ec, std::size_t) { callback(ec); });
    }
    void close() override {
        Error ignored;
        auto& socket = beast::get_lowest_layer(stream_).socket();
        socket.cancel(ignored);
        socket.shutdown(tcp::socket::shutdown_both, ignored);
        socket.close(ignored);
    }
private:
    void accept(Completion callback) {
        stream_.async_accept([self=this->shared_from_this(), callback=std::move(callback)](Error ec) {
            callback(ec);
        });
    }
    ws::stream<Stream> stream_;
    beast::flat_buffer buffer_;
};
}
std::shared_ptr<Channel> make_websocket(tcp::socket socket, asio::ssl::context* tls) {
    if (tls) return std::make_shared<WebSocketChannel<true>>(std::move(socket), *tls);
    return std::make_shared<WebSocketChannel<false>>(std::move(socket));
}
}
