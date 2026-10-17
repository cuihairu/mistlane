#include "mistlane/lua_service.hpp"
#include <chrono>
#include <stdexcept>

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
int protected_call(lua_State* l, int arguments, int results, int handler) {
    lua_sethook(l, instruction_limit, LUA_MASKCOUNT, 1000000);
    int status = lua_pcall(l, arguments, results, handler);
    lua_sethook(l, nullptr, 0, 0);
    return status;
}
}
void LuaService::check(int status) {
    if (status != LUA_OK) {
        auto* text = lua_tostring(state_, -1);
        throw std::runtime_error(text ? text : "Lua error");
    }
}
LuaService::LuaService(const std::string& path) : state_(luaL_newstate()) {
    if (!state_) throw std::runtime_error("cannot create Lua state");
    try {
        // Trusted application scripts only; intentionally no debug/io/os/package library.
        luaL_requiref(state_, "_G", luaopen_base, 1); lua_pop(state_, 1);
        luaL_requiref(state_, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(state_, 1);
        luaL_requiref(state_, LUA_TABLIBNAME, luaopen_table, 1); lua_pop(state_, 1);
        luaL_requiref(state_, LUA_MATHLIBNAME, luaopen_math, 1); lua_pop(state_, 1);
        lua_pushcfunction(state_, traceback);
        check(luaL_loadfile(state_, path.c_str()));
        check(protected_call(state_, 0, 1, 1));
        if (!lua_istable(state_, -1)) throw std::runtime_error("script must return a service table");
        lua_getfield(state_, -1, "message");
        if (!lua_isfunction(state_, -1)) throw std::runtime_error("service.message is required");
        lua_pop(state_, 1);
        module_ = luaL_ref(state_, LUA_REGISTRYINDEX);
        lua_settop(state_, 0);
        callback("init");
    } catch (...) { lua_close(state_); state_ = nullptr; throw; }
}
LuaService::~LuaService() { if (state_) lua_close(state_); }
std::string LuaService::message(std::string_view payload) {
    StackGuard guard(state_);
    lua_pushcfunction(state_, traceback);
    lua_rawgeti(state_, LUA_REGISTRYINDEX, module_);
    lua_getfield(state_, -1, "message");
    if (!lua_isfunction(state_, -1)) throw std::runtime_error("service.message is required");
    lua_pushlstring(state_, payload.data(), payload.size());
    check(protected_call(state_, 1, 1, guard.top+1));
    if (lua_type(state_, -1) != LUA_TSTRING) throw std::runtime_error("message must return a string");
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
    check(protected_call(state_, 0, 0, guard.top+1));
}
void LuaService::tick() { callback("tick"); }
void LuaService::shutdown() { callback("shutdown"); }
}
