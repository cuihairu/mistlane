#pragma once
#include <utility>
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
std::shared_ptr<Channel> make_websocket(boost::asio::ip::tcp::socket socket,
                                      boost::asio::ssl::context* tls);
}
