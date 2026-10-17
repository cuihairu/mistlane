#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <lua.hpp>

namespace mistlane {
// One VM per node, only accessed from that node's io_context thread.
class LuaService {
public:
    explicit LuaService(const std::string& path);
    ~LuaService();
    LuaService(const LuaService&) = delete;
    LuaService& operator=(const LuaService&) = delete;
    std::string message(std::string_view payload);
    void tick();
    void shutdown();
private:
    void callback(const char* name);
    void check(int status);
    lua_State* state_ = nullptr;
    int module_ = LUA_NOREF;
};
}
