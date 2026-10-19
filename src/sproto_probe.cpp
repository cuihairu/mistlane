// SPDX-License-Identifier: Apache-2.0
#include <lua.hpp>
#include <iostream>
#include <string>

extern "C" int luaopen_sproto_core(lua_State*);
extern "C" int luaopen_lpeg(lua_State*);

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: mistlane-sproto-probe MODE SCHEMA.sproto\n";
        return 2;
    }
    lua_State* state = luaL_newstate();
    if (!state) return 2;
    luaL_openlibs(state);
    luaL_requiref(state, "lpeg", luaopen_lpeg, 1);
    lua_pop(state, 1);
    luaL_requiref(state, "sproto.core", luaopen_sproto_core, 1);
    lua_pop(state, 1);
    const std::string package_path =
        "package.path = '" MISTLANE_SPROTO_LUA_DIR "/?.lua;' "
        ".. package.path; "
        "package.cpath = '" MISTLANE_LPEG_LUA_DIR "/?.so;' .. package.cpath";
    if (luaL_dostring(state, package_path.c_str()) != LUA_OK) {
        std::cerr << lua_tostring(state, -1) << '\n';
        lua_close(state);
        return 1;
    }
    lua_pushstring(state, argv[1]); lua_setglobal(state, "MISTLANE_SPROTO_MODE");
    lua_pushstring(state, argv[2]); lua_setglobal(state, "MISTLANE_SPROTO_SCHEMA");
    const std::string script = std::string(MISTLANE_SPROTO_LUA_DIR) + "/../../examples/sproto_lua.lua";
    const int result = luaL_dofile(state, script.c_str());
    if (result != LUA_OK) std::cerr << lua_tostring(state, -1) << '\n';
    lua_close(state);
    return result == LUA_OK ? 0 : 1;
}
