// SPDX-License-Identifier: Apache-2.0
#include "lua_tests.hpp"
#include "cli_client.hpp"
#include <lua.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <thread>

namespace mistlane::cli {
namespace {
using Clock = std::chrono::steady_clock;
struct Budget { Clock::time_point end; };
Budget& budget(lua_State* state) { return **static_cast<Budget**>(lua_getextraspace(state)); }
void hook(lua_State* state, lua_Debug*) {
    if (Clock::now() >= budget(state).end) luaL_error(state, "Lua test deadline exceeded");
}
struct Vm {
    Budget limit;
    lua_State* state = nullptr;
    explicit Vm(unsigned timeout) : limit{Clock::now()+std::chrono::milliseconds(timeout)}, state(luaL_newstate()) {
        if (!state) throw std::runtime_error("cannot create Lua test VM");
        *static_cast<Budget**>(lua_getextraspace(state)) = &limit;
        lua_sethook(state, hook, LUA_MASKCOUNT, 10000);
        luaL_openlibs(state);
    }
    ~Vm() { if (state) lua_close(state); }
};
struct ClientHolder { std::unique_ptr<Client> client; };
unsigned remaining(lua_State* state);
int close_client(lua_State* state) {
    auto* holder = static_cast<ClientHolder*>(luaL_checkudata(state, 1, "mistlane.test.client"));
    holder->client.reset(); return 0;
}
int gc_client(lua_State* state) {
    static_cast<ClientHolder*>(lua_touserdata(state, 1))->~ClientHolder(); return 0;
}
int client_request(lua_State* state) {
    auto* holder = static_cast<ClientHolder*>(luaL_checkudata(state, 1, "mistlane.test.client"));
    std::size_t size=0; auto* bytes=luaL_checklstring(state, 2, &size);
    try {
        if (!holder->client) throw std::runtime_error("test client is closed");
        if (Clock::now() >= budget(state).end) throw std::runtime_error("Lua test deadline exceeded");
        auto result=holder->client->request(std::string_view(bytes,size),remaining(state));
        if (Clock::now() >= budget(state).end) throw std::runtime_error("Lua test deadline exceeded");
        lua_pushlstring(state,result.data(),result.size()); return 1;
    } catch (const std::exception& error) {lua_pushstring(state,error.what());}
    return lua_error(state);
}
std::string field(lua_State* state, const char* name, std::string fallback = {}) {
    lua_getfield(state,1,name);
    if (lua_isnil(state,-1)) {lua_pop(state,1);return fallback;}
    if (lua_type(state,-1)!=LUA_TSTRING) {lua_pop(state,1);throw std::invalid_argument(std::string(name)+" must be a string");}
    std::size_t n=0;const auto* p=lua_tolstring(state,-1,&n);std::string result(p,n);
    lua_pop(state,1);return result;
}
unsigned remaining(lua_State* state) {
    const auto duration=std::chrono::duration_cast<std::chrono::milliseconds>(budget(state).end-Clock::now()).count();
    if (duration <= 0) throw std::runtime_error("Lua test deadline exceeded");
    return static_cast<unsigned>(std::min<std::int64_t>(duration,60000));
}
int connect_client(lua_State* state) {
    luaL_checktype(state,1,LUA_TTABLE);
    try {
        ClientOptions options;
        options.url=field(state,"url"); options.ca_file=field(state,"ca_file"); options.method=field(state,"method","POST");
        unsigned timeout=std::min(remaining(state),5000u);
        lua_getfield(state,1,"timeout_ms");
        if (!lua_isnil(state,-1)) {
            int valid=0;const auto n=lua_tointegerx(state,-1,&valid);
            if (!valid || n<1 || n>60000) throw std::invalid_argument("timeout_ms must be between 1 and 60000");
            timeout=std::min(remaining(state),static_cast<unsigned>(n));
        }
        lua_pop(state,1);
        options.timeout_ms=timeout;
        auto client=make_client(options);
        auto* holder=static_cast<ClientHolder*>(lua_newuserdatauv(state,sizeof(ClientHolder),0));
        new(holder) ClientHolder{std::move(client)};
        luaL_setmetatable(state,"mistlane.test.client");return 1;
    } catch (const std::exception& error) {lua_pushstring(state,error.what());}
    return lua_error(state);
}
int assert_equal(lua_State* state) {
    luaL_checkany(state,1);luaL_checkany(state,2);
    if (!lua_compare(state,1,2,LUA_OPEQ))
        return luaL_error(state,"%s",luaL_optstring(state,3,"assert_equal: values differ"));
    return 0;
}
int sleep(lua_State* state) {
    const auto value=luaL_checkinteger(state,1);
    try {
        if (value<0 || value>60000 || static_cast<unsigned>(value)>=remaining(state))
            throw std::invalid_argument("sleep_ms exceeds the remaining test deadline");
        std::this_thread::sleep_for(std::chrono::milliseconds(value));return 0;
    } catch (const std::exception& error) {lua_pushstring(state,error.what());}
    return lua_error(state);
}
void api(lua_State* state, const std::string& name) {
    if (luaL_newmetatable(state,"mistlane.test.client")) {
        lua_pushcfunction(state,gc_client);lua_setfield(state,-2,"__gc");
        lua_newtable(state);
        lua_pushcfunction(state,client_request);lua_setfield(state,-2,"request");
        lua_pushcfunction(state,close_client);lua_setfield(state,-2,"close");
        lua_setfield(state,-2,"__index");
    }
    lua_pop(state,1);
    lua_newtable(state);
    lua_pushcfunction(state,connect_client);lua_setfield(state,-2,"connect");
    lua_pushcfunction(state,assert_equal);lua_setfield(state,-2,"assert_equal");
    lua_pushcfunction(state,sleep);lua_setfield(state,-2,"sleep_ms");
    lua_pushlstring(state,name.data(),name.size());lua_setfield(state,-2,"name");
}
int traceback(lua_State* state) {
    auto* message=lua_tostring(state,1);
    luaL_traceback(state,state,message?message:"Lua error",1);return 1;
}
void check(lua_State* state,int status) {
    if (status==LUA_OK) return;
    const auto* value=lua_tostring(state,-1);
    throw std::runtime_error(value?value:"Lua operation failed");
}
struct Script {std::filesystem::path path;std::string source;};
void load(lua_State* state,const Script& script) {
    lua_getglobal(state,"package");lua_getfield(state,-1,"path");
    std::string search=script.path.parent_path().string()+"/?.lua;"+script.path.parent_path().string()+"/?/init.lua;";
    if (auto* old=lua_tostring(state,-1))search+=old;
    lua_pop(state,1);lua_pushlstring(state,search.data(),search.size());lua_setfield(state,-2,"path");lua_pop(state,1);
    const auto name="@"+script.path.string();
    check(state,luaL_loadbufferx(state,script.source.data(),script.source.size(),name.c_str(),"t"));
    check(state,lua_pcall(state,0,1,0));
    if (!lua_istable(state,-1))throw std::invalid_argument("Lua test script must return {name=..., cases={...}}");
}
std::string call(lua_State* state,int reference,const std::string& name) {
    if (reference==LUA_NOREF)return {};
    const auto base=lua_gettop(state);
    lua_pushcfunction(state,traceback);const int handler=lua_gettop(state);
    lua_rawgeti(state,LUA_REGISTRYINDEX,reference);api(state,name);
    const auto result=lua_pcall(state,1,0,handler);
    std::string error;
    if (result!=LUA_OK) {auto* p=lua_tostring(state,-1);error=p?p:"unknown Lua error";}
    lua_settop(state,base);return error;
}
int reference(lua_State* state,int table,const char* name) {
    lua_getfield(state,table,name);
    if (lua_isnil(state,-1)){lua_pop(state,1);return LUA_NOREF;}
    if (!lua_isfunction(state,-1))throw std::invalid_argument(std::string(name)+" must be a function");
    return luaL_ref(state,LUA_REGISTRYINDEX);
}
}
int run_lua_tests(const std::vector<std::string>& paths,const std::string& selected,bool list,unsigned timeout_ms) {
    struct Case {std::shared_ptr<Script> script;std::string key;};
    std::map<std::string,Case> cases;
    for (const auto& path:paths) {
        auto script=std::make_shared<Script>();script->path=std::filesystem::canonical(path);
        std::ifstream stream(script->path,std::ios::binary|std::ios::ate);
        if (!stream || stream.tellg()<0 || stream.tellg()>1024*1024)throw std::invalid_argument("Lua test script exceeds 1 MiB");
        script->source.resize(static_cast<std::size_t>(stream.tellg()));stream.seekg(0);
        if (!stream.read(script->source.data(),static_cast<std::streamsize>(script->source.size())))
            throw std::runtime_error("cannot read Lua test script");
        Vm vm(timeout_ms);auto* state=vm.state;load(state,*script);
        lua_getfield(state,-1,"name");
        std::string suite=lua_isstring(state,-1)?lua_tostring(state,-1):script->path.stem().string();
        lua_pop(state,1);
        if (suite.empty()||suite.size()>128)throw std::invalid_argument("Lua test suite name must be 1..128 bytes");
        lua_getfield(state,-1,"cases");
        if (!lua_istable(state,-1))throw std::invalid_argument("Lua test module requires a cases table");
        lua_pushnil(state);
        while(lua_next(state,-2)) {
            if (lua_type(state,-2)!=LUA_TSTRING || !lua_isfunction(state,-1))
                throw std::invalid_argument("Lua cases must map string names to functions");
            std::string key=lua_tostring(state,-2);
            if (key.empty() || key.size()>128)throw std::invalid_argument("Lua case name must be 1..128 bytes");
            const auto name=suite+"."+key;
            if (!cases.emplace(name,Case{script,key}).second)throw std::invalid_argument("duplicate Lua case: "+name);
            lua_pop(state,1);
        }
    }
    unsigned passed=0,failed=0,matched=0;
    for (const auto& [name,item]:cases) {
        if (!selected.empty() && name!=selected)continue;
        ++matched;
        if (list){std::cout<<name<<" [lua script]\n";continue;}
        try {
            Vm vm(timeout_ms);auto* state=vm.state;load(state,*item.script);const auto module=lua_gettop(state);
            const auto setup=reference(state,module,"setup"),teardown=reference(state,module,"teardown");
            lua_getfield(state,module,"cases");const auto body=reference(state,lua_gettop(state),item.key.c_str());
            if (body==LUA_NOREF)throw std::runtime_error("case disappeared on reload");
            auto error=call(state,setup,name);
            if (error.empty())error=call(state,body,name);
            if (!error.empty())vm.limit.end=Clock::now()+std::chrono::milliseconds(std::min(timeout_ms,1000u));
            auto cleanup=call(state,teardown,name);
            if (!cleanup.empty())error+=(error.empty()?"":"\n")+std::string("teardown: ")+cleanup;
            if (!error.empty())throw std::runtime_error(error);
            ++passed;std::cout<<"PASS "<<name<<'\n';
        } catch(const std::exception& error) {++failed;std::cerr<<"FAIL "<<name<<": "<<error.what()<<'\n';}
    }
    if (!matched)throw std::invalid_argument("no matching Lua test cases");
    if (!list)std::cout<<"tests="<<matched<<" passed="<<passed<<" failed="<<failed<<'\n';
    return failed?1:0;
}
}
