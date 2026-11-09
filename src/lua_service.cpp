// SPDX-License-Identifier: Apache-2.0
#include "mistlane/lua_service.hpp"
#include "lua_service_internal.hpp"
#include "mistlane/transport.hpp" // max_frame bounds websocket pushes
#include "mistlane/entity_runtime.hpp"
#include "mistlane/entity_lua.hpp"
#include "mistlane/entry_ticket.hpp"
#include <boost/asio/ssl.hpp>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <chrono>
#include <stdexcept>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <boost/asio/detached.hpp>
#include <boost/redis/request.hpp>
#include <boost/redis/response.hpp>
#include <boost/redis/ignore.hpp>
#include <boost/redis/src.hpp>
#include <limits>
#include <utility>
#include <vector>
#include <cstdlib>
#include <cmath>
#include <charconv>

namespace mistlane {
namespace {
boost::redis::config redis_config(std::string host, std::uint16_t port) {
    boost::redis::config config;
    config.addr.host = std::move(host);
    config.addr.port = std::to_string(port);
    if (const char* username = std::getenv("MISTLANE_REDIS_USERNAME")) config.username = username;
    if (const char* password = std::getenv("MISTLANE_REDIS_PASSWORD")) config.password = password;
    const char* tls = std::getenv("MISTLANE_REDIS_TLS");
    const bool enabled = tls && std::string_view{tls} == "on";
    if (tls && *tls && !enabled) throw std::runtime_error("MISTLANE_REDIS_TLS accepts only 'on'");
    if (std::getenv("MISTLANE_REDIS_CA") && !enabled) throw std::runtime_error("MISTLANE_REDIS_CA requires MISTLANE_REDIS_TLS=on");
    if (enabled) config.use_ssl = true;
    return config;
}
}
void LuaService::check(int status) {
    if (status != LUA_OK) {
        auto* text = lua_tostring(state_, -1);
        throw std::runtime_error(text ? text : "Lua error");
    }
}
LuaService::LuaService(const std::string& path, ServiceContext context, std::shared_ptr<RedisRuntime> redis,
                       WebsocketSink* sink, GameSessionSink* sessions, PlacementSink* placement,
                       EntityRuntime* entity_runtime, const TicketIssuer* ticket_issuer, ReturnSink* return_sink,
                       ScriptMetricsSink* metrics_sink)
  : state_(luaL_newstate()), context_(std::move(context)), websocket_sink_(sink), session_sink_(sessions),
    placement_sink_(placement), entity_runtime_(entity_runtime), ticket_issuer_(ticket_issuer),
    return_sink_(return_sink), metrics_sink_(metrics_sink), async_context_(std::make_shared<AsyncContext>()),
    redis_(std::move(redis)) {
    if (!state_) throw std::runtime_error("cannot create Lua state");
    async_context_->state = state_;
    try {
        // Trusted application scripts only; intentionally no debug/io/os/package library.
        luaL_requiref(state_, "_G", luaopen_base, 1); lua_pop(state_, 1);
        luaL_requiref(state_, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(state_, 1);
        luaL_requiref(state_, LUA_TABLIBNAME, luaopen_table, 1); lua_pop(state_, 1);
        luaL_requiref(state_, LUA_MATHLIBNAME, luaopen_math, 1); lua_pop(state_, 1);
        push_stream_api(state_, this);
        push_websocket_api(state_, this);
        push_session_api(state_, this);
        push_placement_api(state_, this);
        push_entity_api(state_, entity_runtime_);
        push_ticket_api(state_, this);
        push_return_api(state_, this);
        push_metrics_api(state_, this);
        lua_pushcfunction(state_, traceback);
        check(luaL_loadfile(state_, path.c_str()));
        check(protected_call(state_, 0, 1, 1));
        if (!lua_istable(state_, -1)) throw std::runtime_error("script must return a service table");
        // One script may serve both framed sessions and HTTP, so each transport
        // its listeners use requires its own handler.
        if (context_.game_sessions) {
            lua_getfield(state_, -1, "message");
            if (!lua_isfunction(state_, -1)) throw std::runtime_error("service.message is required");
            lua_pop(state_, 1);
        }
        if (context_.http_service) {
            lua_getfield(state_, -1, "http");
            if (!lua_isfunction(state_, -1)) throw std::runtime_error("service.http is required");
            lua_pop(state_, 1);
        }
        module_ = luaL_ref(state_, LUA_REGISTRYINDEX);
        lua_settop(state_, 0);
        callback("init");
    } catch (...) { async_context_->state = nullptr; lua_close(state_); state_ = nullptr; throw; }
}
LuaService::~LuaService() {
    if (!state_) return;
    async_context_->state = nullptr;
    try { shutdown(); } catch (const std::exception& e) { std::cerr << "Lua cleanup failed: " << e.what() << '\n'; }
    lua_close(state_);
}
RedisRuntime::RedisRuntime(boost::asio::io_context& io, std::string host, std::uint16_t port)
  : config_(redis_config(std::move(host), port)) {
    // Boost.Redis verifies nothing by itself; the ssl::context below owns chain and
    // hostname checks, so TLS is only as trustworthy as this configuration.
    boost::asio::ssl::context context{boost::asio::ssl::context::tlsv12_client};
    if (config_.use_ssl) {
        if (const char* ca = std::getenv("MISTLANE_REDIS_CA")) context.load_verify_file(ca);
        else context.set_default_verify_paths();
        context.set_verify_mode(boost::asio::ssl::verify_peer | boost::asio::ssl::verify_fail_if_no_peer_cert);
        // Match SSL_set1_host semantics: IP literals verify against IP SANs.
        in_addr v4{}; in6_addr v6{};
        auto* param = ::SSL_CTX_get0_param(context.native_handle());
        if (::inet_pton(AF_INET, config_.addr.host.c_str(), &v4) == 1 ||
            ::inet_pton(AF_INET6, config_.addr.host.c_str(), &v6) == 1)
            ::X509_VERIFY_PARAM_set1_ip_asc(param, config_.addr.host.c_str());
        else
            ::X509_VERIFY_PARAM_set1_host(param, config_.addr.host.c_str(), 0);
    }
    connection_ = std::make_shared<boost::redis::connection>(io, std::move(context));
}
RedisRuntime::~RedisRuntime() { stop(); }
void RedisRuntime::start() {
    connection_->async_run(config_, boost::asio::detached);
}
void RedisRuntime::stop() { if (connection_) connection_->cancel(); }
bool RedisRuntime::acquire_request() { if (outstanding_ >= 256) return false; ++outstanding_; return true; }
void RedisRuntime::release_request() { if (outstanding_) --outstanding_; }
std::optional<std::string> LuaService::message(std::string_view payload, const CrossContext* context) {
    StackGuard guard(state_);
    lua_pushcfunction(state_, traceback);
    lua_rawgeti(state_, LUA_REGISTRYINDEX, module_);
    lua_getfield(state_, -1, "message");
    if (!lua_isfunction(state_, -1)) throw std::runtime_error("service.message is required");
    lua_pushlstring(state_, payload.data(), payload.size());
    // Game sessions carry their identity; cross sessions add their routing fields.
    lua_createtable(state_, 0, 10);
    if (context) {
        if (context->session) {
            lua_pushinteger(state_, static_cast<lua_Integer>(context->session));
            lua_setfield(state_, -2, "session");
        }
        if (context->request) {
            lua_pushinteger(state_, static_cast<lua_Integer>(context->request));
            lua_setfield(state_, -2, "request");
        }
        if (!context->realm.empty()) {
            lua_pushlstring(state_, context->realm.data(), context->realm.size()); lua_setfield(state_, -2, "realm");
            lua_pushlstring(state_, context->group.data(), context->group.size()); lua_setfield(state_, -2, "group");
            lua_pushlstring(state_, context->room.data(), context->room.size()); lua_setfield(state_, -2, "room");
            lua_pushinteger(state_, context->epoch); lua_setfield(state_, -2, "epoch");
        }
        if (!context->character.empty()) {
            // Participant context (cross return protocol): identity, hosting
            // generation and the nested return route; absent for anonymous joins.
            lua_pushlstring(state_, context->character.data(), context->character.size());
            lua_setfield(state_, -2, "character");
            lua_pushinteger(state_, static_cast<lua_Integer>(context->generation));
            lua_setfield(state_, -2, "generation");
            lua_createtable(state_, 0, 3);
            const std::pair<const std::string*, const char*> route[] = {
                {&context->return_realm, "realm"}, {&context->return_group, "group"},
                {&context->return_room, "room"}};
            for (const auto& field : route) {
                lua_pushlstring(state_, field.first->data(), field.first->size());
                lua_setfield(state_, -2, field.second);
            }
            lua_setfield(state_, -2, "route");
        }
        if (context->verified) {
            // The host verified this join's entry ticket; absent means the
            // session carries no verified durable-write eligibility.
            lua_pushboolean(state_, 1);
            lua_setfield(state_, -2, "verified");
        }
    }
    check(protected_call(state_, 2, 1, guard.top+1));
    if (lua_isnil(state_, -1)) return std::nullopt; // bounded push instead of a reply
    if (lua_type(state_, -1) != LUA_TSTRING) throw std::runtime_error("message must return a string or nil");
    size_t len = 0;
    const char* data = lua_tolstring(state_, -1, &len);
    return std::string(data, len);
}
void LuaService::callback(const char* name) {
    StackGuard guard(state_);
    lua_pushcfunction(state_, traceback);
    lua_rawgeti(state_, LUA_REGISTRYINDEX, module_);
    lua_getfield(state_, -1, name);
    if (lua_isnil(state_, -1)) return;
    if (!lua_isfunction(state_, -1)) throw std::runtime_error(std::string(name)+" must be a function");
    int arguments = 0;
    if (std::string_view(name) == "init") {
        lua_createtable(state_, 0, 3);
        lua_pushstring(state_, context_.role.c_str()); lua_setfield(state_, -2, "role");
        lua_pushstring(state_, context_.transport.c_str()); lua_setfield(state_, -2, "transport");
        lua_pushinteger(state_, context_.epoch); lua_setfield(state_, -2, "epoch");
        arguments = 1;
    }
    check(protected_call(state_, arguments, 0, guard.top+1));
}
void LuaService::tick() { if (started_ && !stopped_) callback("tick"); }
void LuaService::start() { if (!started_) { started_ = true; callback("start"); } }
void LuaService::stop() { if (!stopped_) { stopped_ = true; callback("stop"); } }
void LuaService::shutdown() {
    if (shutdown_) return;
    try { stop(); } catch (const std::exception& e) { std::cerr << "Lua stop failed: " << e.what() << '\n'; }
    shutdown_ = true;
    callback("shutdown");
}
}
