// SPDX-License-Identifier: Apache-2.0
#include <lua.hpp>
#include "mistlane/entity_dispatch.hpp"
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
    if (result == LUA_OK && std::string(argv[1]) == "dispatch") {
        lua_getglobal(state, "MISTLANE_DECODE_OK");
        const bool decoded_ok = lua_toboolean(state, -1) != 0;
        lua_pop(state, 1);
        lua_getglobal(state, "MISTLANE_DECODED_ACCOUNT");
        const auto account = lua_tointeger(state, -1);
        lua_pop(state, 1);
        if (!decoded_ok) { std::cerr << "upstream sproto decode marker missing\n"; lua_close(state); return 1; }
        const mistlane::EntityContract contract(R"(<Contracts><Entity name="Avatar" id="1"><ServerMethods><Method name="getProfile" id="2" mode="request" caller="server"><Arg name="account" id="0" type="UINT32"/><Returns><Field name="level" id="1" type="UINT16"/></Returns></Method></ServerMethods></Entity></Contracts>)");
        mistlane::EntityDispatcher dispatcher(contract);
        dispatcher.bind("Avatar", "server", 2, [](const auto&,const auto&,const auto&) { return mistlane::EntityValues{{"level",std::int64_t{7}}}; });
        auto response=dispatcher.dispatch({{1,1,42,3},{"server",9},"server",2,true,{{"account",std::int64_t{account}}}});
        if (!response.ok) { std::cerr << "sproto decode dispatch failed\n"; lua_close(state); return 1; }
        std::cout << "SPROTO_DECODE_DISPATCH_TYPED_RESPONSE level=" << std::get<std::int64_t>(response.response.at("level").data) << " [PASS]\n";
    }
    lua_close(state);
    return result == LUA_OK ? 0 : 1;
}
