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
// The outgoing VM's declaration, not a snapshot of the live runtime: only
// plain data survives a VM swap. Functions, threads and userdata would dangle
// against the new state, so they refuse the handoff. A table is an array when
// its keys are exactly 1..n; any other shape is an object with stringified
// keys. Depth and entry counts are bounded so a hostile or accidental cycle
// cannot unwind the reload through a stack overflow.
constexpr int kHandoffMaxDepth = 32;
constexpr std::size_t kHandoffMaxEntries = 4096;
struct HandoffBudget { std::size_t entries = 0; };
EntityValue read_handoff_value(lua_State* l, int index, HandoffBudget& budget, int depth) {
    // Callers pass -1 for "top"; the recursive walk pushes keys/values as it
    // goes, so a relative index would silently name a different value on the
    // next iteration. Normalize once, before anything is pushed.
    index = lua_absindex(l, index);
    if (depth > kHandoffMaxDepth) throw std::runtime_error("handoff state is too deep");
    switch (lua_type(l, index)) {
        case LUA_TBOOLEAN: return lua_toboolean(l, index) != 0;
        case LUA_TNUMBER: {
            lua_Integer integer = 0;
            if (!lua_isinteger(l, index)) {
                const lua_Number number = lua_tonumber(l, index);
                if (!std::isfinite(number) || number != std::floor(number) ||
                    number < static_cast<lua_Number>(std::numeric_limits<std::int64_t>::min()) ||
                    number > static_cast<lua_Number>(std::numeric_limits<std::int64_t>::max()))
                    throw std::runtime_error("handoff numbers must be integers");
                integer = static_cast<lua_Integer>(number);
            } else integer = lua_tointeger(l, index);
            return static_cast<std::int64_t>(integer);
        }
        case LUA_TSTRING: {
            std::size_t size = 0;
            const char* data = lua_tolstring(l, index, &size);
            return std::string(data, size);
        }
        case LUA_TTABLE: break;
        default: throw std::runtime_error("handoff state must be plain data");
    }
    if (lua_getmetatable(l, index)) {
        lua_pop(l, 1);
        throw std::runtime_error("handoff tables must be plain tables");
    }
    if (++budget.entries > kHandoffMaxEntries) throw std::runtime_error("handoff state is too large");
    lua_pushnil(l);
    std::size_t count = 0, array = 0;
    EntityObject object;
    EntityArray array_values;
    while (lua_next(l, index) != 0) {
        ++count;
        if (budget.entries + count > kHandoffMaxEntries)
            throw std::runtime_error("handoff state is too large");
        // lua_next leaves the key at -2 and the value at -1; tolstring only
        // ever runs on an already-string key, so the traversal is safe.
        const int key_at = lua_gettop(l) - 1;
        const bool key_is_integer = lua_isinteger(l, key_at);
        if (key_is_integer) {
            const auto key = lua_tointeger(l, key_at);
            if (key == static_cast<lua_Integer>(array) + 1) ++array;
        }
        std::string key;
        if (key_is_integer) key = std::to_string(lua_tointeger(l, key_at));
        else {
            if (lua_type(l, key_at) != LUA_TSTRING)
                throw std::runtime_error("handoff table keys must be strings or indices");
            std::size_t length = 0;
            const char* data = lua_tolstring(l, key_at, &length);
            key.assign(data, length);
        }
        auto value = read_handoff_value(l, key_at + 1, budget, depth + 1);
        if (key_is_integer && array == count
                && static_cast<lua_Integer>(count) == lua_tointeger(l, key_at))
            array_values.push_back(std::move(value));
        else object.insert_or_assign(std::move(key), std::move(value));
        lua_pop(l, 1);
    }
    if (count && array == count) return array_values;
    // An empty table carries no shape information; an object is the neutral
    // choice (it reads back as an empty table under the same name).
    return object;
}
// The validate-only shadow runs the candidate bundle with every mutating face
// replaced by a raise. A script that writes on init/start fails before it is
// promoted, so a rejected bundle cannot have side effects on live state.
int readonly_call(lua_State* l) {
    // seal_field stores the dotted path as the closure's one upvalue.
    const char* name = "unknown";
    if (lua_type(l, lua_upvalueindex(1)) == LUA_TLIGHTUSERDATA)
        name = static_cast<const char*>(lua_touserdata(l, lua_upvalueindex(1)));
    return luaL_error(l, "readonly: %s is disabled while validating a script", name);
}
void seal_field(lua_State* l, const char* path, const char* label) {
    const std::string_view full{path};
    const auto dot = full.find('.');
    const std::string table = dot == std::string_view::npos ? "" : std::string(full.substr(0, dot));
    const std::string field = std::string(full.substr(dot == std::string_view::npos ? 0 : dot + 1));
    lua_getglobal(l, "mistlane");
    if (!lua_istable(l, -1)) { lua_pop(l, 1); return; }
    if (!table.empty()) {
        lua_getfield(l, -1, table.c_str());
        if (!lua_istable(l, -1)) { lua_pop(l, 2); return; }
    }
    // Stack: [mistlane, (subtable)] — setfield pops the closure but leaves the
    // tables, so the final pop mirrors exactly what was pushed above.
    lua_pushlightuserdata(l, const_cast<char*>(label));
    lua_pushcclosure(l, readonly_call, 1);
    lua_setfield(l, -2, field.c_str());
    lua_pop(l, table.empty() ? 1 : 2);
}
void seal_mutations(lua_State* l) {
    static const char* const fields[] = {
        "entity.spawn", "entity.destroy", "entity.properties", "entity.observe",
        "entity.depart", "entity.flush", "entity.set_owner", "entity.transfer",
        "transfer", "scene.switch", "grant", "returning.seed", "returning.enqueue",
        "ticket.issue", "metrics.gauge", "session.send", "session.broadcast",
        "websocket.send", "stream.create_group", "stream.publish", "stream.ack",
        "stream.claim", "leaderboard.add", "leaderboard.remove"
    };
    for (const char* field : fields) seal_field(l, field, field);
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
                       EntityRuntime* entity_runtime, const TicketIssuer* ticket_issuer, ReturnSink* return_sink,
                       ScriptMetricsSink* metrics_sink, GrantSink* grant_sink, TransferSink* transfer_sink,
                       SceneSink* scene_sink, DataSink* data_sink)
  : state_(luaL_newstate()), context_(std::move(context)), websocket_sink_(sink), session_sink_(sessions),
    placement_sink_(placement), grant_sink_(grant_sink), data_sink_(data_sink),
    transfer_sink_(transfer_sink), scene_sink_(scene_sink), entity_runtime_(entity_runtime),
    ticket_issuer_(ticket_issuer), return_sink_(return_sink), metrics_sink_(metrics_sink),
    async_context_(std::make_shared<AsyncContext>()),
    redis_(std::move(redis)), avatar_runtime_(*this) {
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
        push_entity_api(state_, this);
        push_ticket_api(state_, this);
        push_return_api(state_, this);
        push_grant_api(state_, this);
        push_data_api(state_, this);
        push_transfer_api(state_, this);
        push_scene_api(state_, this);
        push_metrics_api(state_, this);
        // The validate-only shadow gets the same surface with every mutating
        // face replaced by a raising stub — init/start run read-only, so a
        // bundle that writes during validation fails the reload before it can
        // touch live state. unseal() re-runs the push sequence above, which
        // overwrites the sealed fields with the real closures.
        if (context_.validate_only) seal_mutations(state_);
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
        // avatar_load is the avatar opt-in gate: a script that never declares it
        // hosts no avatars at all, so the node skips the whole lifecycle instead
        // of refusing every session join. Declaring it non-function is a broken
        // surface, not an opt-out — avatar_load is driven as a hook either way.
        lua_rawgeti(state_, LUA_REGISTRYINDEX, module_);
        lua_getfield(state_, -1, "avatar_load");
        avatar_enabled_ = lua_isfunction(state_, -1);
        lua_settop(state_, 0);
        callback("init");
    } catch (...) { async_context_->state = nullptr; lua_close(state_); state_ = nullptr; throw; }
}
LuaService::~LuaService() {
    if (!state_) return;
    // A reload swaps the VM without running the scripted stop hook: the
    // retirement face (best-effort avatar_save then close for every avatar)
    // runs here, while the VM is still alive, before the module state is
    // destroyed. stop() already retired on the graceful-shutdown path, so the
    // guard keeps it a single flush.
    if (!stopped_) retire_avatars();
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
        lua_createtable(state_, 0, 4);
        lua_pushstring(state_, context_.role.c_str()); lua_setfield(state_, -2, "role");
        lua_pushstring(state_, context_.transport.c_str()); lua_setfield(state_, -2, "transport");
        lua_pushinteger(state_, context_.epoch); lua_setfield(state_, -2, "epoch");
        // The outgoing VM's plain-data hand-off, or an empty table on a cold
        // boot: the new implementation's init sees exactly what the old one
        // declared, under the same `context.script_state` name both bundles
        // agree on. It is never populated from a live runtime — functions,
        // userdata and closures cannot cross a VM boundary.
        lua_createtable(state_, 0, static_cast<int>(context_.script_state.size()));
        for (const auto& [key, value] : context_.script_state) {
            push_entity_value(state_, value);
            lua_setfield(state_, -2, key.c_str());
        }
        lua_setfield(state_, -2, "script_state");
        arguments = 1;
    }
    check(protected_call(state_, arguments, 0, guard.top+1));
}
bool LuaService::handoff(EntityValues& state, std::string& error) const {
    StackGuard guard(state_);
    lua_pushcfunction(state_, traceback);
    lua_rawgeti(state_, LUA_REGISTRYINDEX, module_);
    lua_getfield(state_, -1, "handoff");
    if (lua_isnil(state_, -1)) return true; // no hook: nothing to carry over
    if (!lua_isfunction(state_, -1)) { error = "handoff must be a function"; return false; }
    if (protected_call(state_, 0, 1, guard.top+1) != LUA_OK) {
        const auto* text = lua_tostring(state_, -1);
        error = text ? text : "handoff failed";
        return false;
    }
    if (lua_isnil(state_, -1)) { state.clear(); return true; } // nothing to carry over
    try {
        HandoffBudget budget;
        EntityValue value = read_handoff_value(state_, -1, budget, 0);
        if (auto* object = std::get_if<EntityObject>(&value.data)) state = std::move(*object);
        else if (auto* array = std::get_if<EntityArray>(&value.data)) {
            // A top-level array is carried under its positional names, so the
            // named `context.script_state` table stays a table on the other side.
            for (std::size_t i = 0; i < array->size(); ++i)
                state.emplace(std::to_string(i+1), std::move((*array)[i]));
        } else { error = "handoff must return a table"; return false; }
    } catch (const std::exception& e) { error = e.what(); return false; }
    return true;
}
void LuaService::unseal() {
    if (!context_.validate_only) return;
    StackGuard guard(state_);
    push_stream_api(state_, this);
    push_websocket_api(state_, this);
    push_session_api(state_, this);
    push_placement_api(state_, this);
    push_entity_api(state_, this);
    push_ticket_api(state_, this);
    push_return_api(state_, this);
    push_grant_api(state_, this);
    push_data_api(state_, this);
    push_transfer_api(state_, this);
    push_scene_api(state_, this);
    push_metrics_api(state_, this);
}
void LuaService::tick() {
    if (!started_ || stopped_) return;
    // The framework layer ticks before the script's own tick, so an avatar's
    // business step sees the state the framework just settled. The reclaim
    // sweep runs after the grace-expiry pass: a suspended avatar whose grace
    // ended moves to reclaiming and then the ordered reclaim (final save,
    // closed) completes in the same tick, so an offline avatar does not linger.
    if (avatar_enabled_) { avatar_runtime_.tick(); avatar_runtime_.sweep_reclaims(); }
    callback("tick");
}
void LuaService::start() { if (!started_) { started_ = true; callback("start"); } }
void LuaService::stop() {
    if (!stopped_) {
        stopped_ = true;
        callback("stop");
        // Graceful shutdown: the scripted stop hook ran, then every avatar
        // flushes (best-effort save) and closes before the VM goes away.
        retire_avatars();
    }
}
void LuaService::retire_avatars() {
    if (avatar_enabled_) avatar_runtime_.retire_all();
}
void LuaService::shutdown() {
    if (shutdown_) return;
    try { stop(); } catch (const std::exception& e) { std::cerr << "Lua stop failed: " << e.what() << '\n'; }
    shutdown_ = true;
    callback("shutdown");
}
}
