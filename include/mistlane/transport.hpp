// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <utility>
#include "mistlane/http_service.hpp"
#include <boost/asio.hpp>
#include <boost/asio/ssl/context.hpp>
#include <functional>
#include <memory>
#include <string>

namespace mistlane {
constexpr std::size_t max_frame = 1024*1024;
class Channel {
public:
    using Error = boost::system::error_code;
    using Reader = std::function<void(Error, std::string)>;
    using Completion = std::function<void(Error)>;
    virtual ~Channel() = default;
    virtual boost::asio::any_io_executor executor() = 0;
    virtual void start(Completion callback) = 0;
    virtual void read(Reader callback) = 0;
    virtual void write(std::string payload, Completion callback) = 0;
    virtual void close() = 0;
};
// Drogon owns the public listeners. Accepted payload channels execute on executor.
struct Config;
using Attach = std::function<void(std::shared_ptr<Channel>)>;
// A WebSocket upgrade on an HTTP/HTTPS listener of a script role: the connection
// joins the Lua websocket lifecycle (open/message/close) instead of a game session.
using WsAttach = std::function<void(std::shared_ptr<Channel>, std::string path)>;
void run_web(const Config&, boost::asio::any_io_executor, Attach attach, WsAttach ws_attach,
             HttpHandler http_handler, std::function<void()> ready);
void stop_web();
}
