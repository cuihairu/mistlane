// SPDX-License-Identifier: Apache-2.0
#include "test_common.hpp"
#include "lua_service_internal.hpp"
#include <lua.hpp>
namespace mistlane::test {
void lua_lifecycle() {
    Context c; const auto script = c.path("service.lua"); write(script, echo);
    const auto listen = port();
    auto& gameServer = c.node("gameServer", listen, {"--script", script.string()});
    auto client = tcp(listen); equal(client->request("v1"), "v1");
    write(script, "return {message=function(p) return 'v2:'..p end}");
    gameServer.signal(SIGHUP); gameServer.ready("script reloaded");
    equal(client->request("x"), "v2:x");
    write(script, "invalid lua !!!");
    gameServer.signal(SIGHUP); gameServer.ready("reload rejected");
    equal(client->request("x"), "v2:x");
    write(script, "return {}");
    gameServer.signal(SIGHUP); gameServer.ready("service.message is required");
    equal(client->request("x"), "v2:x");
    write(c.path("bad.lua"), "return {message=function(p) if p=='error' then error('bad') end "
        "if p=='loop' then while true do end end return p end}");
    const auto bad = port();
    c.node("gameServer", bad, {"--script", c.path("bad.lua").string()});
    for (const auto& payload : {"error", "loop"}) must_fail([&] { (void)tcp(bad)->request(payload); });
    equal(tcp(bad)->request("healthy"), "healthy");
    // The actual exported Lua source is embedded at configure time, so installed
    // binaries can run this regression without depending on a source checkout.
    write(c.path("heap.lua"), "local H=assert(load([==["+std::string(heap_lua)+"]==]))()\n"
        "assert(MinHeap==nil); local h=H:new(3,function(a,b) return a<b end)\n"
        "h:push(3);h:push(1);h:push(2);assert(h:pop()==1 and h:pop()==2 and h:pop()==3)\n"
        "return {message=function(p)return p end}");
    const auto heap = port(); c.node("gameServer", heap, {"--script", c.path("heap.lua").string()});
    equal(tcp(heap)->request("heap"), "heap");
    gameServer.signal(SIGTERM); require(gameServer.wait() == 0, "gameServer graceful shutdown failed");
    // A SIGHUP reload hands the replacement service the same optional sinks
    // (ticket issuer, return sender, metrics), so the API surface a script
    // sees survives a reload instead of silently shrinking to the plain set.
    // No role carries all three: the issuer lives on login, the return sender
    // on crossServer, metrics on any script host, so each role's surface is
    // asserted before and after its own reload.
    {
        const auto cross = port(), login = port();
        ScopedEnv secrets("MISTLANE_TICKET_SECRETS",
            "beta:3333333333333333333333333333333333333333333333333333333333333333");
        write(c.path("deps.lua"),
            "return {message=function(p)\n"
            "  if p == 'deps' then\n"
            "    return tostring(mistlane.ticket ~= nil)..','..tostring(mistlane.returning ~= nil)\n"
            "      ..','..tostring(mistlane.metrics ~= nil)\n"
            "  end\n"
            "  return p\n"
            "end}\n");
        auto& node = c.node("crossServer", cross, {"--script", c.path("deps.lua").string(),
            "--data", endpoint(port()), "--upstream-token", "t", "--service-name", "xs"});
        auto& issuer = c.node("login", login, {"--script", c.path("deps.lua").string()});
        const auto send_frame = [](RawSocket& raw, std::string_view payload) {
            std::string frame(4, '\0');
            auto length = static_cast<std::uint32_t>(payload.size());
            for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(length & 255); length >>= 8; }
            raw.send(frame+std::string(payload));
        };
        const auto read_frame = [](RawSocket& raw) {
            std::size_t length = 0;
            for (unsigned char ch : raw.receive(4)) length = (length << 8) | ch;
            return raw.receive(length);
        };
        const auto deps = [&](std::uint16_t where) {
            RawSocket raw(where);
            if (where == cross) {
                send_frame(raw, "MLC1 1 home g r1 ch hero gen 1 ret home g r1");
                read_frame(raw);
            }
            send_frame(raw, "deps");
            return read_frame(raw);
        };
        eventually([&] { return deps(cross) == "false,true,true"; }, 5s,
            "the crossServer optional sinks were not wired before the reload");
        eventually([&] { return deps(login) == "true,false,true"; }, 5s,
            "the login optional sinks were not wired before the reload");
        node.signal(SIGHUP); node.ready("script reloaded");
        equal(deps(cross), "false,true,true");
        issuer.signal(SIGHUP); issuer.ready("script reloaded");
        equal(deps(login), "true,false,true");
        node.signal(SIGTERM); require(node.wait() == 0, "crossServer graceful shutdown failed");
        issuer.signal(SIGTERM); require(issuer.wait() == 0, "login graceful shutdown failed");
    }
    // A push that throws reaches the Lua callback as its value argument with
    // the stack rebalanced, instead of unwinding through the caller.
    {
        lua_State* l = luaL_newstate();
        require(l != nullptr, "cannot open a probe lua state");
        luaL_openlibs(l);
        require(luaL_dostring(l,
                "probe = nil return function(err, value) probe = tostring(err)..'|'..tostring(value) end")
                == LUA_OK, "probe callback script failed");
        const int reference = luaL_ref(l, LUA_REGISTRYINDEX);
        auto life = std::make_shared<LuaService::AsyncContext>();
        life->state = l;
        finish_lua(life, reference, "", [](lua_State*) { throw std::runtime_error("push exploded"); });
        lua_getglobal(l, "probe");
        const char* probe = lua_tostring(l, -1);
        require(probe && std::string(probe) == "nil|push exploded",
            std::string("a throwing push did not reach the callback: ")+(probe ? probe : "?"));
        require(lua_gettop(l) == 1, "the callback stack was left unbalanced");
        lua_pop(l, 1);
        lua_close(l);
    }
}
void lua_http() {
    Context c; const auto script = c.path("http.lua"); write(script, http_lua);
    const auto listen = port();
    auto& server = c.node("apiServer", listen, {"--transport", "http", "--script", script.string()});
    const auto origin = "http://"+endpoint(listen);
    auto response = http(origin+"/echo?x=1", "POST", std::string("\0\xff", 2), {}, true);
    require(response.status == 201 && response.headers.find("x-hook: after") != std::string::npos, "HTTP metadata/after hook");
    equal(response.body, std::string("\0\xff", 2));
    for (const auto& [path, status, body] : std::vector<std::tuple<std::string, long, std::string>>{
        {"/private",401,"denied"}, {"/replace",202,"replaced"}, {"/error",502,"handled"},
        {"/badheader",502,"handled"}, {"/fatal",500,"Internal Server Error"}, {"/missing",404,"missing"}}) {
        response = http(origin+path); require(response.status == status, "HTTP hook status"); equal(response.body, body);
    }
    write(script, "return {http=function(r)return{body='bad'}end,start=function()error('start rejected')end}");
    server.signal(SIGHUP); server.ready("reload rejected"); equal(http(origin+"/missing").body, "missing");
    write(script, "return {http=function(r)return{body='v2'}end,"
        "init=function()print('HOOK init v2')end,start=function()print('HOOK start v2')end,"
        "stop=function()print('HOOK stop v2')end,shutdown=function()print('HOOK shutdown v2')end}");
    server.signal(SIGHUP); server.ready("script reloaded"); equal(http(origin+"/").body, "v2");
    server.signal(SIGTERM); require(server.wait() == 0, "HTTP shutdown failed");
    std::istringstream log(server.output()); std::string line; std::vector<std::string> hooks;
    while (std::getline(log, line)) if (line.starts_with("HOOK ")) hooks.push_back(line);
    require(hooks == std::vector<std::string>{"HOOK init v1","HOOK start v1","HOOK init v2","HOOK start v2",
        "HOOK stop v1","HOOK shutdown v1","HOOK stop v2","HOOK shutdown v2"}, "Lua lifecycle order changed");
}
}
