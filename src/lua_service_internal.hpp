// SPDX-License-Identifier: Apache-2.0
// Helpers shared by the LuaService implementation files, extracted from the
// single translation unit when the Lua API adapters moved beside it. The
// VM-lifecycle file and the adapter file both pcall through the same
// instruction budget and the same async Redis plumbing.
#pragma once
#include "mistlane/lua_service.hpp"
#include <boost/redis/request.hpp>
#include <boost/redis/response.hpp>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace mistlane {

struct StackGuard {
    lua_State* l;
    int top;
    explicit StackGuard(lua_State* s) : l(s), top(lua_gettop(s)) {}
    ~StackGuard() { lua_settop(l, top); }
};
inline int traceback(lua_State* l) {
    const char* error = lua_tostring(l, 1);
    luaL_traceback(l, l, error ? error : "non-string Lua error", 1);
    return 1;
}
// Interrupt typical runaway callbacks; trusted scripts, not resource isolation.
inline void instruction_limit(lua_State* l, lua_Debug*) {
    luaL_error(l, "Lua instruction budget exceeded");
}
inline std::string checked_text(lua_State* l, int index, const char* label, std::size_t maximum = 1024) {
    if (lua_type(l, index) != LUA_TSTRING) throw std::runtime_error(std::string(label)+" must be a string");
    std::size_t size = 0; const char* data = lua_tolstring(l, index, &size);
    if (!size || size > maximum) throw std::runtime_error(std::string(label)+" length is invalid");
    for (std::size_t i=0; i<size; ++i) if (static_cast<unsigned char>(data[i]) < 0x20 || data[i] == 0x7f)
        throw std::runtime_error(std::string(label)+" contains a control character");
    return {data,size};
}
inline std::string stream_key(const std::string& stream) { return "mistlane:stream:"+stream; }
inline std::string board_key(const std::string& board) { return "mistlane:leaderboard:"+board; }
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
inline void push_stream_entries(lua_State* l, const std::vector<std::string>& flat, std::size_t offset) {
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
inline int protected_call(lua_State* l, int arguments, int results, int handler) {
    lua_sethook(l, instruction_limit, LUA_MASKCOUNT, 1000000);
    int status = lua_pcall(l, arguments, results, handler);
    lua_sethook(l, nullptr, 0, 0);
    return status;
}

}
