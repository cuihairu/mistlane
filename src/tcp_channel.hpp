// SPDX-License-Identifier: Apache-2.0
// The framed internal-TCP channel, extracted from the node implementation.
// Wire format: u32 BE length + payload; the backlog bound turns a stalled
// reader into a refusal instead of unbounded growth.
#pragma once
#include "node_internal.hpp"
#include "mistlane/transport.hpp"
#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <string>

namespace mistlane {

// Writes from independent callbacks (replies and Lua pushes) must not
// interleave, so every frame queues; the backlog bound turns a stalled reader
// into a refusal instead of unbounded growth. Both refusal paths report
// message_size synchronously before returning.
inline constexpr std::size_t write_backlog = 2*max_frame;

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
        if (backlog_ + payload.size() + 4 > write_backlog) return callback(asio::error::message_size);
        auto frame = std::make_shared<std::string>(4, '\0');
        auto n = static_cast<std::uint32_t>(payload.size());
        for (int i = 3; i >= 0; --i) { (*frame)[i] = static_cast<char>(n & 255); n >>= 8; }
        frame->append(std::move(payload));
        backlog_ += frame->size();
        pending_.push_back({std::move(frame), std::move(callback)});
        pump();
    }
    void close() {
        Error ignored;
        socket_.cancel(ignored);
        socket_.shutdown(tcp::socket::shutdown_both, ignored);
        socket_.close(ignored);
    }
private:
    void pump() {
        if (sending_ || pending_.empty()) return;
        sending_ = true;
        auto frame = std::move(pending_.front().frame);
        auto callback = std::move(pending_.front().callback);
        pending_.pop_front();
        backlog_ -= frame->size();
        const auto buffer = asio::buffer(*frame); // built before the capture moves the owner
        asio::async_write(socket_, buffer,
          [self=shared_from_this(), frame=std::move(frame), callback=std::move(callback)]
          (Error ec, std::size_t) mutable {
            self->sending_ = false;
            callback(ec);
            self->pump(); // queue may have grown while the callback ran
          });
    }
    struct Pending { std::shared_ptr<std::string> frame; std::function<void(Error)> callback; };
    tcp::socket socket_;
    std::array<unsigned char, 4> header_{};
    std::deque<Pending> pending_;
    std::size_t backlog_ = 0;
    bool sending_ = false;
};

}
