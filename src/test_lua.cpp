// SPDX-License-Identifier: Apache-2.0
#include "test_common.hpp"
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
