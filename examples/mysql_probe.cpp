// SPDX-License-Identifier: Apache-2.0
#include "mistlane/mysql_pool.hpp"
#include <boost/asio/cancel_after.hpp>
#include <cstdlib>
#include <iostream>

namespace asio = boost::asio;
namespace mysql = boost::mysql;
using namespace std::chrono_literals;
std::string env(const char* name, const char* fallback = "") {
    auto* value = std::getenv(name);
    return value ? value : fallback;
}
asio::awaitable<void> probe(mysql::connection_pool& pool) {
    // With max_size=1 the second acquisition succeeds only after the first
    // pooled_connection has returned. Reset/health/reconnect belong to Boost.
    for (int iteration = 0; iteration < 3; ++iteration) {
        auto connection = co_await pool.async_get_connection(asio::cancel_after(5s));
        mysql::results result;
        std::string parameter = "quote' and backslash\\; --";
        co_await connection->async_execute(mysql::with_params("SELECT {}, {}", 42, parameter), result,
                                           asio::cancel_after(5s));
        if (result.rows().at(0).at(0).as_int64() != 42 ||
            result.rows().at(0).at(1).as_string() != parameter)
            throw std::runtime_error("unexpected parameterized SELECT result");
    }
    std::cout << "MySQL pool: 3 acquisitions and parameterized queries passed\n";
}
int main() {
    try {
        asio::io_context io;
        mistlane::MysqlConfig config;
        config.max_size = 1;
        config.host = env("MISTLANE_MYSQL_HOST", "localhost");
        config.username = env("MISTLANE_MYSQL_USER");
        config.password = env("MISTLANE_MYSQL_PASSWORD");
        config.database = env("MISTLANE_MYSQL_DATABASE");
        auto port = env("MISTLANE_MYSQL_PORT", "3306");
        size_t end = 0;
        int value = std::stoi(port, &end);
        if (end != port.size() || value < 1 || value > 65535) throw std::invalid_argument("invalid MySQL port");
        config.port = static_cast<unsigned short>(value);
        auto tls = env("MISTLANE_MYSQL_TLS", "require");
        if (tls != "require" && tls != "disable") throw std::invalid_argument("MISTLANE_MYSQL_TLS must be require or disable");
        config.tls = tls == "require";
        auto pool = mistlane::make_mysql_pool(io, config);
        pool->async_run(asio::detached);
        std::exception_ptr failure;
        asio::co_spawn(io, probe(*pool), [&](std::exception_ptr error) {
            failure = error;
            pool->cancel();
        });
        io.run();
        if (failure) std::rethrow_exception(failure);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "MySQL probe failed: " << e.what() << '\n';
        return 1;
    }
}
