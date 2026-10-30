// SPDX-License-Identifier: Apache-2.0
#include "mistlane/lua_service.hpp"
#include "mistlane/transport.hpp" // max_frame bounds websocket pushes
#include "mistlane/entity_runtime.hpp"
#include "mistlane/entity_lua.hpp"
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
struct StackGuard {
    lua_State* l;
    int top;
    explicit StackGuard(lua_State* s) : l(s), top(lua_gettop(s)) {}
    ~StackGuard() { lua_settop(l, top); }
};
int traceback(lua_State* l) {
    const char* error = lua_tostring(l, 1);
    luaL_traceback(l, l, error ? error : "non-string Lua error", 1);
    return 1;
}
// Interrupt typical runaway callbacks; trusted scripts, not resource isolation.
void instruction_limit(lua_State* l, lua_Debug*) {
    luaL_error(l, "Lua instruction budget exceeded");
}
std::string checked_text(lua_State* l, int index, const char* label, std::size_t maximum = 1024) {
    if (lua_type(l, index) != LUA_TSTRING) throw std::runtime_error(std::string(label)+" must be a string");
    std::size_t size = 0; const char* data = lua_tolstring(l, index, &size);
    if (!size || size > maximum) throw std::runtime_error(std::string(label)+" length is invalid");
    for (std::size_t i=0; i<size; ++i) if (static_cast<unsigned char>(data[i]) < 0x20 || data[i] == 0x7f)
        throw std::runtime_error(std::string(label)+" contains a control character");
    return {data,size};
}
std::string stream_key(const std::string& stream) { return "mistlane:stream:"+stream; }
std::string board_key(const std::string& board) { return "mistlane:leaderboard:"+board; }
struct AsyncLua { std::weak_ptr<LuaService::AsyncContext> life; int callback = LUA_NOREF; };
template<class Push>
void finish_lua(std::weak_ptr<LuaService::AsyncContext> weak, int callback, std::string error, Push push) {
    auto life = weak.lock();
    if (!life || !life->state) return;
    lua_State* l = life->state;
    lua_rawgeti(l, LUA_REGISTRYINDEX, callback);
    luaL_unref(l, LUA_REGISTRYINDEX, callback);
    if (!error.empty()) lua_pushlstring(l, error.data(), error.size()); else lua_pushnil(l);
    push(l);
    if (lua_pcall(l, 2, 0, 0) != LUA_OK) {
        const char* message = lua_tostring(l, -1);
        std::cerr << "Redis stream callback failed: " << (message ? message : "unknown Lua error") << '\n';
        lua_pop(l, 1);
    }
}
template<class Response, class Push>
void submit_stream(std::shared_ptr<RedisRuntime> runtime, boost::redis::request request,
                   std::weak_ptr<LuaService::AsyncContext> life, int callback, Push push) {
    if (!runtime) { finish_lua(life, callback, "Redis is disabled; start mistlane with --redis IP:PORT", [](lua_State* l){ lua_pushnil(l); }); return; }
    if (!runtime->acquire_request()) { finish_lua(life, callback, "Redis request limit reached", [](lua_State* l){ lua_pushnil(l); }); return; }
    auto conn = runtime->connection();
    struct State { boost::redis::request request; boost::redis::response<Response> response; };
    auto state = std::make_shared<State>(); state->request = std::move(request);
    conn->async_exec(state->request, state->response,
      [state, runtime=std::move(runtime), life, callback, push=std::move(push)](boost::system::error_code ec, std::size_t) mutable {
        runtime->release_request();
        if (ec) finish_lua(life, callback, ec.message(), [](lua_State* l){ lua_pushnil(l); });
        else {
            auto& result = std::get<0>(state->response);
            if (!result.has_value()) finish_lua(life, callback, result.error().diagnostic, [](lua_State* l){ lua_pushnil(l); });
            else push(state->response, life, callback);
        }
      });
}
void push_stream_entries(lua_State* l, const std::vector<std::string>& flat, std::size_t offset) {
    lua_newtable(l); int index = 1;
    while (offset < flat.size()) {
        const std::string id = flat[offset++];
        if (offset >= flat.size()) throw std::runtime_error("malformed Redis stream response");
        std::size_t fields = 0;
        try { fields = static_cast<std::size_t>(std::stoul(flat[offset++])); }
        catch (...) { throw std::runtime_error("malformed Redis stream field count"); }
        if (fields % 2 || fields > 256 || offset + fields > flat.size()) throw std::runtime_error("malformed Redis stream fields");
        lua_createtable(l, 0, 2); lua_pushlstring(l, id.data(), id.size()); lua_setfield(l, -2, "id");
        lua_createtable(l, 0, static_cast<int>(fields/2));
        for (std::size_t i=0; i<fields; i+=2) {
            lua_pushlstring(l, flat[offset+i].data(), flat[offset+i].size());
            lua_pushlstring(l, flat[offset+i+1].data(), flat[offset+i+1].size());
            lua_settable(l, -3);
        }
        offset += fields; lua_setfield(l, -2, "fields"); lua_rawseti(l, -2, index++);
    }
}
int protected_call(lua_State* l, int arguments, int results, int handler) {
    lua_sethook(l, instruction_limit, LUA_MASKCOUNT, 1000000);
    int status = lua_pcall(l, arguments, results, handler);
    lua_sethook(l, nullptr, 0, 0);
    return status;
}
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
                       EntityRuntime* entity_runtime)
  : state_(luaL_newstate()), context_(std::move(context)), websocket_sink_(sink), session_sink_(sessions),
    placement_sink_(placement), entity_runtime_(entity_runtime),
    async_context_(std::make_shared<AsyncContext>()), redis_(std::move(redis)) {
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
void LuaService::push_stream_api(lua_State* l, LuaService* service) {
    lua_getglobal(l, "mistlane");
    if (!lua_istable(l, -1)) { lua_pop(l, 1); lua_newtable(l); }
    lua_newtable(l);
    auto bind = [&](const char* name, lua_CFunction fn) {
        lua_pushlightuserdata(l, service); lua_pushcclosure(l, fn, 1); lua_setfield(l, -2, name);
    };
    bind("create_group", stream_create_group); bind("publish", stream_publish);
    bind("read", stream_read); bind("ack", stream_ack); bind("claim", stream_claim);
    lua_setfield(l, -2, "stream");
    lua_newtable(l);
    bind("add", leaderboard_add); bind("get", leaderboard_get);
    bind("top", leaderboard_top); bind("remove", leaderboard_remove);
    lua_setfield(l, -2, "leaderboard"); lua_setglobal(l, "mistlane");
}
void LuaService::push_websocket_api(lua_State* l, LuaService* service) {
    if (!service->websocket_sink_) return; // no websocket-capable listener on this node
    lua_getglobal(l, "mistlane");
    if (!lua_istable(l, -1)) { lua_pop(l, 1); lua_newtable(l); }
    lua_newtable(l);
    lua_pushlightuserdata(l, service); lua_pushcclosure(l, websocket_send, 1); lua_setfield(l, -2, "send");
    lua_setfield(l, -2, "websocket"); lua_setglobal(l, "mistlane");
}
void LuaService::push_session_api(lua_State* l, LuaService* service) {
    if (!service->session_sink_) return; // no framed game sessions on this node
    lua_getglobal(l, "mistlane");
    if (!lua_istable(l, -1)) { lua_pop(l, 1); lua_newtable(l); }
    lua_newtable(l);
    lua_pushlightuserdata(l, service); lua_pushcclosure(l, session_send, 1); lua_setfield(l, -2, "send");
    lua_pushlightuserdata(l, service); lua_pushcclosure(l, session_broadcast, 1); lua_setfield(l, -2, "broadcast");
    lua_setfield(l, -2, "session"); lua_setglobal(l, "mistlane");
}
void LuaService::push_placement_api(lua_State* l, LuaService* service) {
    lua_getglobal(l, "mistlane");
    if (!lua_istable(l, -1)) { lua_pop(l, 1); lua_newtable(l); }
    lua_pushlightuserdata(l, service); lua_pushcclosure(l, placement_call, 1);
    lua_setfield(l, -2, "place");
    lua_setglobal(l, "mistlane");
}
void LuaService::push_entity_api(lua_State* l, EntityRuntime* runtime) {
    if (!runtime) return; // no contract bundle on this node
    lua_getglobal(l, "mistlane");
    if (!lua_istable(l, -1)) { lua_pop(l, 1); lua_newtable(l); }
    lua_newtable(l);
    lua_pushlightuserdata(l, runtime); lua_pushcclosure(l, entity_spawn, 1); lua_setfield(l, -2, "spawn");
    lua_pushlightuserdata(l, runtime); lua_pushcclosure(l, entity_destroy, 1); lua_setfield(l, -2, "destroy");
    lua_setfield(l, -2, "entity"); lua_setglobal(l, "mistlane");
}
// The fixture runtime addresses by explicit ids; the script supplies them
// (scenario/tests or a login flow that assigns ids). An entity that already
// exists at the address refuses, so a repeated spawn is never silent.
// The id fields are unsigned on the wire; a negative or overflowing Lua
// integer would silently wrap to an address the script never meant.
bool lua_u32(lua_State* l, int index, std::uint32_t& out) {
    if (!lua_isinteger(l, index)) return false;
    const auto value = lua_tointeger(l, index);
    if (value < 0 || static_cast<std::uint64_t>(value) > std::numeric_limits<std::uint32_t>::max())
        return false;
    out = static_cast<std::uint32_t>(value);
    return true;
}
int LuaService::entity_spawn(lua_State* l) {
    auto* runtime = static_cast<EntityRuntime*>(lua_touserdata(l, lua_upvalueindex(1)));
    std::uint32_t scope, type, incarnation;
    if (!lua_u32(l, 1, scope) || !lua_u32(l, 2, type) || !lua_u32(l, 3, incarnation)
            || !lua_isinteger(l, 4))
        return luaL_error(l, "entity.spawn expects scope, type, incarnation, entity_id integers");
    const auto entity_id = static_cast<std::int64_t>(lua_tointeger(l, 4));
    std::uint32_t generation = 1;
    if (!lua_isnoneornil(l, 5)) {
        if (!lua_u32(l, 5, generation)) return luaL_error(l, "entity.spawn generation must be a non-negative 32-bit integer");
    }
    lua_pushboolean(l, runtime->spawn({scope, type, incarnation, entity_id}, generation) ? 1 : 0);
    return 1;
}
int LuaService::entity_destroy(lua_State* l) {
    auto* runtime = static_cast<EntityRuntime*>(lua_touserdata(l, lua_upvalueindex(1)));
    std::uint32_t scope, type, incarnation;
    if (!lua_u32(l, 1, scope) || !lua_u32(l, 2, type) || !lua_u32(l, 3, incarnation)
            || !lua_isinteger(l, 4))
        return luaL_error(l, "entity.destroy expects scope, type, incarnation, entity_id integers");
    lua_pushboolean(l, runtime->destroy({scope, type, incarnation,
        static_cast<std::int64_t>(lua_tointeger(l, 4))}) ? 1 : 0);
    return 1;
}
// Entity method implementations are the script's `entities` table:
// service.entities[type][method] = function(args, ctx) -> returns-table|nil.
// The dispatcher answers NO_IMPLEMENTATION for any declared method the table
// never names, so partial implementations run degraded rather than refuse the
// whole contract. Unknown entity or method names are different: they mean the
// script disagrees with the contract, which is a load refusal, not a runtime
// answer.
bool LuaService::bind_entity_handlers(EntityDispatcher& dispatcher, const EntityContract& contract,
                                      std::string& error) {
    if (!state_) { error = "script is not loaded"; return false; }
    StackGuard guard(state_);
    lua_rawgeti(state_, LUA_REGISTRYINDEX, module_);
    lua_getfield(state_, -1, "entities");
    if (lua_isnil(state_, -1)) { dispatcher.clear_handlers(); return true; }
    if (!lua_istable(state_, -1)) { error = "service.entities must be a table"; return false; }
    auto text = [&](int index) {
        std::size_t size = 0;
        const auto* data = lua_tolstring(state_, index, &size);
        return std::string(data, size);
    };
    // Validate the whole table before touching the dispatcher: a bad entry must
    // not leave a partially rebound set, because a reload that fails keeps the
    // previous service and its handlers would otherwise be half-cleared.
    struct Binding { std::string type, name; unsigned method_id; ContractArgs returns; };
    std::vector<Binding> bindings;
    lua_pushnil(state_);
    while (lua_next(state_, -2) != 0) {
        if (lua_type(state_, -2) != LUA_TSTRING) { error = "service.entities keys must be entity names"; return false; }
        const auto entity_name = text(-2);
        const auto entity = std::find_if(contract.entities().begin(), contract.entities().end(),
            [&](const ContractEntity& e) { return e.name == entity_name; });
        if (entity == contract.entities().end()) {
            error = "service.entities names unknown entity '"+entity_name+"'";
            return false;
        }
        if (!lua_istable(state_, -1)) { error = "service.entities."+entity_name+" must be a table"; return false; }
        lua_pushnil(state_);
        while (lua_next(state_, -2) != 0) {
            if (lua_type(state_, -2) != LUA_TSTRING) {
                error = "service.entities."+entity_name+" keys must be method names";
                return false;
            }
            const auto method_name = text(-2);
            const auto method = std::find_if(entity->server_methods.begin(), entity->server_methods.end(),
                [&](const ContractMethod& m) { return m.name == method_name; });
            if (method == entity->server_methods.end()) {
                const auto client = std::find_if(entity->client_methods.begin(), entity->client_methods.end(),
                    [&](const ContractMethod& m) { return m.name == method_name; });
                if (client != entity->client_methods.end())
                    error = "'"+method_name+"' executes on the client and has no server implementation";
                else
                    error = "service.entities."+entity_name+" names unknown method '"+method_name+"'";
                return false;
            }
            if (!lua_isfunction(state_, -1)) {
                error = "service.entities."+entity_name+"."+method_name+" must be a function";
                return false;
            }
            bindings.push_back({entity_name, method_name, method->id, method->returns});
            lua_pop(state_, 1);
        }
        lua_pop(state_, 1);
    }
    // The table is valid: rebind from scratch so a reload never leaves a stale
    // closure into a closed Lua state, then install the fresh handlers.
    dispatcher.clear_handlers();
    for (const auto& b : bindings)
        dispatcher.bind(b.type, "server", b.method_id, make_entity_handler(b.type, b.name, b.method_id, b.returns, &contract));
    return true;
}
EntityDispatcher::Handler LuaService::make_entity_handler(const std::string& type, const std::string& name,
                                                          unsigned method_id, const ContractArgs returns,
                                                          const EntityContract* shapes) const {
    // The dispatch thread owns the Lua state, so the handler may enter it
    // directly; a failing call answers the closed INVALID instead of tearing
    // the session down.
    return [this, returns, shapes, method_id, type, name](const EntityCaller& caller, const EntityTarget& target,
                                                    const EntityValues& args) -> EntityValues {
                    StackGuard guard(state_);
                    lua_pushcfunction(state_, traceback);
                    const int base = lua_gettop(state_);
                    lua_rawgeti(state_, LUA_REGISTRYINDEX, module_);
                    lua_getfield(state_, -1, "entities");
                    lua_getfield(state_, -1, type.c_str());
                    lua_getfield(state_, -1, name.c_str());
                    if (!lua_isfunction(state_, -1)) return {};
                    lua_createtable(state_, 0, static_cast<int>(args.size()));
                    for (const auto& [field, value] : args) {
                        lua_pushlstring(state_, field.data(), field.size());
                        push_entity_value(state_, value);
                        lua_rawset(state_, -3);
                    }
                    // The context is readonly by convention: plain values, no
                    // engine members the script could mutate state through.
                    lua_createtable(state_, 0, 9);
                    lua_pushlstring(state_, caller.kind.data(), caller.kind.size());
                    lua_setfield(state_, -2, "caller_kind");
                    lua_pushinteger(state_, static_cast<lua_Integer>(caller.id));
                    lua_setfield(state_, -2, "caller_id");
                    lua_pushinteger(state_, static_cast<lua_Integer>(target.scope));
                    lua_setfield(state_, -2, "scope");
                    lua_pushinteger(state_, static_cast<lua_Integer>(target.type_id));
                    lua_setfield(state_, -2, "type_id");
                    lua_pushinteger(state_, static_cast<lua_Integer>(target.entity_id));
                    lua_setfield(state_, -2, "entity_id");
                    lua_pushinteger(state_, static_cast<lua_Integer>(target.incarnation));
                    lua_setfield(state_, -2, "incarnation");
                    lua_pushinteger(state_, static_cast<lua_Integer>(method_id));
                    lua_setfield(state_, -2, "method_id");
                    if (lua_pcall(state_, 2, 1, base) != LUA_OK) {
                        const auto* message = lua_tostring(state_, -1);
                        std::cerr << "entity handler "+type+"."+name+" failed: "
                                  << (message ? message : "unknown Lua error") << '\n';
                        return {}; // fail closed: dispatch validation answers INVALID
                    }
                    if (lua_isnil(state_, -1)) return {};
                    if (lua_type(state_, -1) != LUA_TTABLE) {
                        std::cerr << "entity handler "+type+"."+name+" must return a table or nil\n";
                        return {};
                    }
                    EntityValues out;
                    for (const auto& field : returns) {
                        lua_getfield(state_, -1, field.name.c_str());
                        if (lua_isnil(state_, -1)) {
                            std::cerr << "entity handler "+type+"."+name+" is missing return '"+field.name+"'\n";
                            return {};
                        }
                        EntityValue item(std::int64_t{0});
                        const auto reason = convert_entity_value(state_, -1, field.type, field.of, *shapes, &item);
                        lua_pop(state_, 1);
                        if (!reason.empty()) {
                            std::cerr << "entity handler "+type+"."+name+" return '"+field.name+"' "+reason+'\n';
                            return {};
                        }
                        out.emplace(field.name, std::move(item));
                    }
                    // Refuse keys the contract never declared: a silently
                    // dropped field would look like success to the sender.
                    lua_pushnil(state_);
                    while (lua_next(state_, -2) != 0) {
                        if (lua_type(state_, -2) != LUA_TSTRING) {
                            std::cerr << "entity handler "+type+"."+name+" returned a non-string key\n";
                            lua_pop(state_, 2);
                            return {};
                        }
                        std::size_t size = 0;
                        const auto* data = lua_tolstring(state_, -2, &size);
                        const std::string key(data, size);
                        const bool declared = std::any_of(returns.begin(), returns.end(),
                            [&](const ContractField& f) { return f.name == key; });
                        lua_pop(state_, 1);
                        if (!declared) {
                            std::cerr << "entity handler "+type+"."+name+" returned undeclared '"+key+"'\n";
                            return {};
                        }
                    }
                    return out;
    };
}
// A placement lookup cannot answer inside the handler: the callback runs on
// the same event loop only after the handler returned, so scripts answer
// through mistlane.session.send from the callback instead of a return value.
int LuaService::placement_call(lua_State* l) {
    auto* service = static_cast<LuaService*>(lua_touserdata(l, lua_upvalueindex(1)));
    auto word = [&](int index, const char* label) {
        if (!lua_isstring(l, index)) throw std::runtime_error(std::string(label)+" must be an identifier");
        std::size_t size = 0;
        const char* data = lua_tolstring(l, index, &size);
        if (!size || size > 64) throw std::runtime_error(std::string(label)+" length is invalid");
        for (std::size_t i = 0; i < size; ++i) {
            const unsigned char ch = data[i];
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
                    || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'))
                throw std::runtime_error(std::string(label)+" must be an identifier");
        }
        return std::string(data, size);
    };
    const auto role = word(1, "role");
    std::string namespace_;
    std::string scope;
    if (!lua_isnil(l, 2)) namespace_ = word(2, "namespace");
    if (!lua_isnil(l, 3)) scope = word(3, "scope");
    if (!lua_isfunction(l, 4)) throw std::runtime_error("last argument must be a callback function");
    lua_pushvalue(l, 4);
    const auto callback = luaL_ref(l, LUA_REGISTRYINDEX);
    const auto life = service->async_context_;
    if (!service->placement_sink_)
        return finish_lua(life, callback, "placement is disabled; start with --coordinator IP:PORT",
            [](lua_State* l) { lua_pushnil(l); }), 0;
    service->placement_sink_->place(role, namespace_, scope,
        [life, callback](std::string_view address, std::string_view reason) {
            if (!reason.empty())
                return finish_lua(life, callback, std::string(reason), [](lua_State* l) { lua_pushnil(l); });
            finish_lua(life, callback, {}, [address](lua_State* l) {
                lua_pushlstring(l, address.data(), address.size());
            });
        });
    return 0;
}
void LuaService::websocket_callback(const char* name, std::uint64_t id, std::string_view payload, const std::string& path) {
    StackGuard guard(state_);
    lua_pushcfunction(state_, traceback);
    lua_rawgeti(state_, LUA_REGISTRYINDEX, module_);
    lua_getfield(state_, -1, name);
    if (lua_isnil(state_, -1)) return;
    if (!lua_isfunction(state_, -1)) throw std::runtime_error(std::string(name)+" must be a function");
    lua_pushinteger(state_, static_cast<lua_Integer>(id));
    lua_pushlstring(state_, payload.data(), payload.size());
    lua_pushlstring(state_, path.data(), path.size());
    check(protected_call(state_, 3, 0, guard.top+1));
}
void LuaService::websocket_open(std::uint64_t id, const std::string& path) {
    if (started_ && !stopped_) websocket_callback("websocket_open", id, {}, path);
}
void LuaService::websocket_message(std::uint64_t id, std::string_view payload) {
    if (started_ && !stopped_) websocket_callback("websocket_message", id, payload, {});
}
void LuaService::websocket_close(std::uint64_t id) {
    // Not gated on started_/stopped_: close fires while the node drains sessions
    // during stop, before LuaService::stop() runs.
    websocket_callback("websocket_close", id, {}, {});
}
namespace {
LuaService* stream_service(lua_State* l) { return static_cast<LuaService*>(lua_touserdata(l, lua_upvalueindex(1))); }
int callback_ref(lua_State* l) {
    if (!lua_isfunction(l, -1)) throw std::runtime_error("last argument must be a callback function");
    lua_pushvalue(l, -1); return luaL_ref(l, LUA_REGISTRYINDEX);
}
std::string group_key(const std::string& group) { return group; }
int lua_error(lua_State* l, const std::exception& e) { return luaL_error(l, "%s", e.what()); }
constexpr const char* create_group_script =
    "local r=redis.pcall('XGROUP','CREATE',KEYS[1],ARGV[1],'0','MKSTREAM'); "
    "if type(r)=='table' and r.err and string.find(r.err,'BUSYGROUP',1,true) then return 'OK' end; "
    "if type(r)=='table' and r.err then return redis.error_reply(r.err) end; return r";
constexpr const char* read_group_script =
    "local r=redis.call('XREADGROUP','GROUP',ARGV[1],ARGV[2],'COUNT',ARGV[3],'STREAMS',KEYS[1],'>'); "
    "local o={}; if not r then return o end; for _,s in ipairs(r) do for _,e in ipairs(s[2]) do "
    "table.insert(o,e[1]); table.insert(o,tostring(#e[2])); for _,v in ipairs(e[2]) do table.insert(o,v) end end end; return o";
constexpr const char* claim_script =
    "local r=redis.call('XAUTOCLAIM',KEYS[1],ARGV[1],ARGV[2],ARGV[3],ARGV[4],'COUNT',ARGV[5]); "
    "local o={r[1]}; for _,e in ipairs(r[2]) do table.insert(o,e[1]); table.insert(o,tostring(#e[2])); "
    "for _,v in ipairs(e[2]) do table.insert(o,v) end end; return o";
constexpr const char* leaderboard_add_script =
    "local score=redis.call('ZINCRBY',KEYS[1],ARGV[1],ARGV[2]); "
    "local rank=redis.call('ZREVRANK',KEYS[1],ARGV[2]); return {score,tostring(rank+1)}";
constexpr const char* leaderboard_get_script =
    "local score=redis.call('ZSCORE',KEYS[1],ARGV[1]); if not score then return {} end; "
    "local rank=redis.call('ZREVRANK',KEYS[1],ARGV[1]); return {score,tostring(rank+1)}";
}
// mistlane.websocket.send(id, payload) -> true | false, reason
int LuaService::websocket_send(lua_State* l) {
    auto* self = stream_service(l);
    try {
        if (!self->websocket_sink_) throw std::runtime_error("websocket is not enabled for this node");
        if (!lua_isinteger(l, 1) || lua_tointeger(l, 1) <= 0)
            throw std::runtime_error("websocket id must be a positive integer");
        const auto id = static_cast<std::uint64_t>(lua_tointeger(l, 1));
        if (lua_type(l, 2) != LUA_TSTRING) throw std::runtime_error("websocket payload must be a string");
        std::size_t size = 0;
        const char* data = lua_tolstring(l, 2, &size);
        if (size > max_frame) {
            lua_pushboolean(l, 0);
            lua_pushstring(l, "payload exceeds 1 MiB");
            return 2;
        }
        const std::string reason = self->websocket_sink_->websocket_send(id, {data, size});
        if (reason.empty()) { lua_pushboolean(l, 1); return 1; }
        lua_pushboolean(l, 0);
        lua_pushlstring(l, reason.data(), reason.size());
        return 2;
    } catch (const std::exception& e) { return lua_error(l, e); }
}
// mistlane.session.send(id, payload) -> true | false, reason
int LuaService::session_send(lua_State* l) {
    auto* self = stream_service(l);
    try {
        if (!self->session_sink_) throw std::runtime_error("game sessions are not enabled for this node");
        if (!lua_isinteger(l, 1) || lua_tointeger(l, 1) <= 0)
            throw std::runtime_error("session id must be a positive integer");
        const auto id = static_cast<std::uint64_t>(lua_tointeger(l, 1));
        if (lua_type(l, 2) != LUA_TSTRING) throw std::runtime_error("session payload must be a string");
        std::size_t size = 0;
        const char* data = lua_tolstring(l, 2, &size);
        if (size > max_frame) {
            lua_pushboolean(l, 0);
            lua_pushstring(l, "payload exceeds 1 MiB");
            return 2;
        }
        const std::string reason = self->session_sink_->session_send(id, {data, size});
        if (reason.empty()) { lua_pushboolean(l, 1); return 1; }
        lua_pushboolean(l, 0);
        lua_pushlstring(l, reason.data(), reason.size());
        return 2;
    } catch (const std::exception& e) { return lua_error(l, e); }
}
// mistlane.session.broadcast(payload) -> sent | 0, reason
int LuaService::session_broadcast(lua_State* l) {
    auto* self = stream_service(l);
    try {
        if (!self->session_sink_) throw std::runtime_error("game sessions are not enabled for this node");
        if (lua_type(l, 1) != LUA_TSTRING) throw std::runtime_error("session payload must be a string");
        std::size_t size = 0;
        const char* data = lua_tolstring(l, 1, &size);
        if (size > max_frame) {
            lua_pushinteger(l, 0);
            lua_pushstring(l, "payload exceeds 1 MiB");
            return 2;
        }
        lua_pushinteger(l, static_cast<lua_Integer>(self->session_sink_->session_broadcast({data, size})));
        return 1;
    } catch (const std::exception& e) { return lua_error(l, e); }
}
int LuaService::stream_create_group(lua_State* l) {
    auto* self=stream_service(l);
    try {
        auto stream=checked_text(l,1,"stream name"), group=checked_text(l,2,"group name");
        int cb=callback_ref(l); boost::redis::request req; req.push("EVAL",create_group_script,1,stream_key(stream),group_key(group));
        submit_stream<std::string>(self->redis_,std::move(req),self->async_context_,cb,
          [](auto&,auto life,int callback){ finish_lua(life,callback,"",[](lua_State* s){lua_pushboolean(s,1);}); });
        return 0;
    } catch(const std::exception& e){return lua_error(l,e);}
}
int LuaService::stream_publish(lua_State* l) {
    auto* self=stream_service(l);
    try {
        auto stream=checked_text(l,1,"stream name"); if(!lua_istable(l,2)) throw std::runtime_error("fields must be a table");
        int cb=callback_ref(l); std::vector<std::string> args{"*"}; std::size_t bytes=0;
        lua_pushnil(l); while(lua_next(l,2)) {
            if(lua_type(l,-2)!=LUA_TSTRING || lua_type(l,-1)!=LUA_TSTRING) throw std::runtime_error("stream fields must be string pairs");
            std::size_t kn=0,vn=0; const char* k=lua_tolstring(l,-2,&kn); const char* v=lua_tolstring(l,-1,&vn);
            if(!kn || kn>256 || vn>65536 || (bytes+=kn+vn)>65536) throw std::runtime_error("stream fields exceed size limits");
            args.emplace_back(k,kn); args.emplace_back(v,vn); lua_pop(l,1);
            if(args.size()>129) throw std::runtime_error("stream entry has too many fields");
        }
        if(args.size()==1) throw std::runtime_error("stream entry must contain at least one field");
        boost::redis::request req; req.push_range("XADD",stream_key(stream),args);
        submit_stream<std::string>(self->redis_,std::move(req),self->async_context_,cb,
          [](auto& response,auto life,int callback){ auto result=std::get<0>(response).value(); finish_lua(life,callback,"",[result=std::move(result)](lua_State* s){lua_pushlstring(s,result.data(),result.size());}); });
        return 0;
    } catch(const std::exception& e){return lua_error(l,e);}
}
int LuaService::stream_read(lua_State* l) {
    auto* self=stream_service(l);
    try {
        auto stream=checked_text(l,1,"stream name"), group=checked_text(l,2,"group name"), consumer=checked_text(l,3,"consumer name");
        if(!lua_isinteger(l,4) || lua_tointeger(l,4)<1 || lua_tointeger(l,4)>100) throw std::runtime_error("count must be from 1 to 100");
        int cb=callback_ref(l); boost::redis::request req;
        req.push("EVAL",read_group_script,1,stream_key(stream),group,consumer,static_cast<int>(lua_tointeger(l,4)));
        submit_stream<std::vector<std::string>>(self->redis_,std::move(req),self->async_context_,cb,
          [](auto& response,auto life,int callback){ auto flat=std::get<0>(response).value(); finish_lua(life,callback,"",[flat=std::move(flat)](lua_State* s){push_stream_entries(s,flat,0);}); });
        return 0;
    } catch(const std::exception& e){return lua_error(l,e);}
}
int LuaService::stream_ack(lua_State* l) {
    auto* self=stream_service(l);
    try {
        auto stream=checked_text(l,1,"stream name"), group=checked_text(l,2,"group name"), id=checked_text(l,3,"message id");
        int cb=callback_ref(l); boost::redis::request req; req.push("XACK",stream_key(stream),group,id);
        submit_stream<long long>(self->redis_,std::move(req),self->async_context_,cb,
          [](auto& response,auto life,int callback){ int count=std::get<0>(response).value(); finish_lua(life,callback,"",[count](lua_State* s){lua_pushinteger(s,count);}); });
        return 0;
    } catch(const std::exception& e){return lua_error(l,e);}
}
int LuaService::stream_claim(lua_State* l) {
    auto* self=stream_service(l);
    try {
        auto stream=checked_text(l,1,"stream name"), group=checked_text(l,2,"group name"), consumer=checked_text(l,3,"consumer name"), start=checked_text(l,5,"start id");
        if(!lua_isinteger(l,4)||lua_tointeger(l,4)<0||lua_tointeger(l,4)>86400000) throw std::runtime_error("min_idle_ms must be from 0 to 86400000");
        if(!lua_isinteger(l,6)||lua_tointeger(l,6)<1||lua_tointeger(l,6)>100) throw std::runtime_error("count must be from 1 to 100");
        int cb=callback_ref(l); boost::redis::request req;
        req.push("EVAL",claim_script,1,stream_key(stream),group,consumer,static_cast<long long>(lua_tointeger(l,4)),start,static_cast<int>(lua_tointeger(l,6)));
        submit_stream<std::vector<std::string>>(self->redis_,std::move(req),self->async_context_,cb,
          [](auto& response,auto life,int callback){ auto flat=std::get<0>(response).value(); finish_lua(life,callback,"",[flat=std::move(flat)](lua_State* s){
            lua_createtable(s,0,2); if(!flat.empty()){lua_pushlstring(s,flat[0].data(),flat[0].size());lua_setfield(s,-2,"next");push_stream_entries(s,flat,1);}else lua_newtable(s);lua_setfield(s,-2,"entries"); }); });
        return 0;
    } catch(const std::exception& e){return lua_error(l,e);}
}
int LuaService::leaderboard_add(lua_State* l) {
    auto* self=stream_service(l);
    try {
        auto board=checked_text(l,1,"board name"), member=checked_text(l,2,"member id",256);
        if(!lua_isnumber(l,3)) throw std::runtime_error("score delta must be a number");
        double delta=lua_tonumber(l,3); if(!std::isfinite(delta)) throw std::runtime_error("score delta must be finite");
        int cb=callback_ref(l); boost::redis::request req;
        char delta_buffer[64]; auto [delta_end, delta_error] = std::to_chars(delta_buffer, delta_buffer+sizeof(delta_buffer), delta, std::chars_format::general, std::numeric_limits<double>::max_digits10);
        if(delta_error != std::errc{}) throw std::runtime_error("cannot format score delta");
        req.push("EVAL",leaderboard_add_script,1,board_key(board),std::string(delta_buffer,delta_end),member);
        submit_stream<std::vector<std::string>>(self->redis_,std::move(req),self->async_context_,cb,
          [](auto& response,auto life,int callback){ auto values=std::get<0>(response).value(); finish_lua(life,callback,"",[values=std::move(values)](lua_State* s){
            if(values.size()!=2) { lua_pushnil(s); return; } lua_createtable(s,0,2);
            lua_pushlstring(s,values[0].data(),values[0].size());lua_setfield(s,-2,"score");
            lua_pushinteger(s,std::stoll(values[1]));lua_setfield(s,-2,"rank"); }); });
        return 0;
    } catch(const std::exception& e){return lua_error(l,e);}
}
int LuaService::leaderboard_get(lua_State* l) {
    auto* self=stream_service(l);
    try {
        auto board=checked_text(l,1,"board name"), member=checked_text(l,2,"member id",256);
        int cb=callback_ref(l); boost::redis::request req; req.push("EVAL",leaderboard_get_script,1,board_key(board),member);
        submit_stream<std::vector<std::string>>(self->redis_,std::move(req),self->async_context_,cb,
          [](auto& response,auto life,int callback){ auto values=std::get<0>(response).value(); finish_lua(life,callback,"",[values=std::move(values)](lua_State* s){
            if(values.empty()) { lua_pushnil(s); return; } if(values.size()!=2) { lua_pushnil(s); return; }
            lua_createtable(s,0,2);lua_pushlstring(s,values[0].data(),values[0].size());lua_setfield(s,-2,"score");lua_pushinteger(s,std::stoll(values[1]));lua_setfield(s,-2,"rank"); }); });
        return 0;
    } catch(const std::exception& e){return lua_error(l,e);}
}
int LuaService::leaderboard_top(lua_State* l) {
    auto* self=stream_service(l);
    try {
        auto board=checked_text(l,1,"board name");
        if(!lua_isinteger(l,2)||lua_tointeger(l,2)<0||lua_tointeger(l,2)>1000000000) throw std::runtime_error("offset must be from 0 to 1000000000");
        if(!lua_isinteger(l,3)||lua_tointeger(l,3)<1||lua_tointeger(l,3)>100) throw std::runtime_error("limit must be from 1 to 100");
        auto offset=lua_tointeger(l,2), limit=lua_tointeger(l,3); int cb=callback_ref(l); boost::redis::request req;
        req.push("ZREVRANGE",board_key(board),offset,offset+limit-1,"WITHSCORES");
        submit_stream<std::vector<std::string>>(self->redis_,std::move(req),self->async_context_,cb,
          [offset](auto& response,auto life,int callback){ auto values=std::get<0>(response).value(); finish_lua(life,callback,"",[values=std::move(values),offset](lua_State* s){
            lua_createtable(s,static_cast<int>(values.size()/2),0); int index=1;
            for(std::size_t i=0;i+1<values.size();i+=2){lua_createtable(s,0,3);lua_pushlstring(s,values[i].data(),values[i].size());lua_setfield(s,-2,"member");
              lua_pushlstring(s,values[i+1].data(),values[i+1].size());lua_setfield(s,-2,"score");lua_pushinteger(s,offset+index);lua_setfield(s,-2,"rank");lua_rawseti(s,-2,index++);}
          }); });
        return 0;
    } catch(const std::exception& e){return lua_error(l,e);}
}
int LuaService::leaderboard_remove(lua_State* l) {
    auto* self=stream_service(l);
    try {
        auto board=checked_text(l,1,"board name"), member=checked_text(l,2,"member id",256);
        int cb=callback_ref(l); boost::redis::request req; req.push("ZREM",board_key(board),member);
        submit_stream<long long>(self->redis_,std::move(req),self->async_context_,cb,
          [](auto& response,auto life,int callback){ auto count=std::get<0>(response).value(); finish_lua(life,callback,"",[count](lua_State* s){lua_pushinteger(s,count);}); });
        return 0;
    } catch(const std::exception& e){return lua_error(l,e);}
}
}

namespace mistlane {
namespace {
void text_field(lua_State* l, const char* name, const std::string& value) {
    lua_pushlstring(l, value.data(), value.size()); lua_setfield(l, -2, name);
}
void push_request(lua_State* l, const WebRequest& request) {
    lua_createtable(l, 0, 5);
    text_field(l, "method", request.method); text_field(l, "path", request.path);
    text_field(l, "query", request.query); text_field(l, "body", request.body);
    lua_newtable(l);
    for (const auto& [key, value] : request.headers) text_field(l, key.c_str(), value);
    lua_setfield(l, -2, "headers");
}
std::string lua_string(lua_State* l, int index) {
    if (lua_type(l, index) != LUA_TSTRING) throw std::runtime_error("HTTP field must be a string");
    std::size_t size = 0;
    const char* data = lua_tolstring(l, index, &size);
    return {data, size};
}
WebResponse response(lua_State* l, int index) {
    if (!lua_istable(l, index)) throw std::runtime_error("HTTP handler must return a response table");
    index = lua_absindex(l, index);
    WebResponse result;
    lua_getfield(l, index, "status");
    if (!lua_isnil(l, -1)) {
        if (!lua_isinteger(l, -1) || lua_tointeger(l, -1) < 200 || lua_tointeger(l, -1) > 599)
            throw std::runtime_error("HTTP status must be an integer from 200 to 599");
        result.status = static_cast<unsigned>(lua_tointeger(l, -1));
    }
    lua_pop(l, 1);
    lua_getfield(l, index, "body");
    if (!lua_isnil(l, -1)) result.body = lua_string(l, -1);
    lua_pop(l, 1);
    if (result.body.size() > 1024*1024) throw std::runtime_error("HTTP response body exceeds 1 MiB");
    lua_getfield(l, index, "headers");
    if (!lua_isnil(l, -1)) {
        if (!lua_istable(l, -1)) throw std::runtime_error("HTTP headers must be a table");
        lua_pushnil(l);
        std::size_t bytes = 0;
        while (lua_next(l, -2)) {
            auto name = lua_string(l, -2), value = lua_string(l, -1);
            bytes += name.size()+value.size();
            if (bytes > 16384 || name.empty() || value.find_first_of("\r\n\0", 0, 3) != std::string::npos)
                throw std::runtime_error("invalid HTTP headers");
            for (unsigned char ch : name)
                if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                      (ch >= '0' && ch <= '9') || std::string_view("!#$%&'*+-.^_`|~").find(ch) != std::string_view::npos))
                    throw std::runtime_error("invalid HTTP header name");
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return std::tolower(ch); });
            if (name == "content-length" || name == "transfer-encoding" || name == "connection" || name == "upgrade")
                throw std::runtime_error("HTTP framing headers are managed by Drogon");
            result.headers[name] = std::move(value);
            lua_pop(l, 1);
        }
    }
    lua_pop(l, 1);
    return result;
}
}
void LuaService::validate_http() {
    StackGuard guard(state_);
    lua_rawgeti(state_, LUA_REGISTRYINDEX, module_);
    lua_getfield(state_, -1, "http");
    if (!lua_isfunction(state_, -1)) throw std::runtime_error("service.http is required for Lua HTTP nodes");
}
WebResponse LuaService::http(const WebRequest& request) {
    StackGuard guard(state_);
    lua_pushcfunction(state_, traceback);
    const int handler = lua_gettop(state_);
    push_request(state_, request);
    const int req = lua_gettop(state_);
    auto function = [&](const char* name) {
        lua_rawgeti(state_, LUA_REGISTRYINDEX, module_);
        lua_getfield(state_, -1, name); lua_remove(state_, -2);
        if (lua_isnil(state_, -1)) { lua_pop(state_, 1); return false; }
        if (!lua_isfunction(state_, -1)) throw std::runtime_error(std::string(name)+" must be a function");
        return true;
    };
    try {
        bool handled = false;
        if (function("before_request")) {
            lua_pushvalue(state_, req); check(protected_call(state_, 1, 1, handler));
            handled = !lua_isnil(state_, -1);
            if (!handled) lua_pop(state_, 1);
        }
        if (!handled) {
            if (!function("http")) throw std::runtime_error("service.http is required");
            lua_pushvalue(state_, req); check(protected_call(state_, 1, 1, handler));
        }
        // Validate before exposing the result to the post-handler hook.
        (void)response(state_, -1);
        const int res = lua_gettop(state_);
        if (function("after_request")) {
            lua_pushvalue(state_, req); lua_pushvalue(state_, res);
            check(protected_call(state_, 2, 1, handler));
            if (lua_isnil(state_, -1)) lua_pop(state_, 1); // retain mutations of the original table
        }
        return response(state_, -1);
    } catch (const std::exception& e) {
        std::cerr << "HTTP Lua callback failed: " << e.what() << '\n';
        lua_settop(state_, req);
        try {
            if (function("on_error")) {
                lua_pushvalue(state_, req); lua_pushstring(state_, e.what());
                check(protected_call(state_, 2, 1, handler));
                return response(state_, -1);
            }
        } catch (const std::exception& nested) { std::cerr << "HTTP on_error failed: " << nested.what() << '\n'; }
        return {500, {}, "Internal Server Error"};
    }
}
}
