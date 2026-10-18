// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <utility>
#include <boost/asio.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/mysql/connection_pool.hpp>
#include <boost/mysql/results.hpp>
#include <boost/mysql/with_params.hpp>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>

namespace mistlane {
struct MysqlConfig {
    std::string host = "localhost";
    unsigned short port = 3306;
    std::string username, password, database;
    std::size_t initial_size = 1, max_size = 16;
    // Disabling TLS must be explicit (e.g. a local disposable integration server).
    bool tls = true;
};
// This is configuration glue, not a pool implementation. The caller directly owns
// the mature Boost.MySQL pool and its RAII pooled_connection objects.
inline std::unique_ptr<boost::mysql::connection_pool> make_mysql_pool(
    boost::asio::io_context& io, const MysqlConfig& config) {
    if (config.max_size == 0 || config.initial_size > config.max_size)
        throw std::invalid_argument("invalid MySQL pool sizes");
    boost::mysql::pool_params params;
    params.server_address.emplace_host_and_port(config.host, config.port);
    params.username = config.username;
    params.password = config.password;
    params.database = config.database;
    params.initial_size = config.initial_size;
    params.max_size = config.max_size;
    params.connect_timeout = std::chrono::seconds(5);
    params.retry_interval = std::chrono::seconds(1);
    params.ssl = config.tls ? boost::mysql::ssl_mode::require : boost::mysql::ssl_mode::disable;
    if (config.tls) {
        params.ssl_ctx.emplace(boost::asio::ssl::context::tls_client);
        params.ssl_ctx->set_default_verify_paths();
        params.ssl_ctx->set_verify_mode(boost::asio::ssl::verify_peer);
        params.ssl_ctx->set_verify_callback(boost::asio::ssl::host_name_verification(config.host));
    }
    return std::make_unique<boost::mysql::connection_pool>(io, std::move(params));
}
}
