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
// Row 73, Part A (docs/entity-contract.md "No server rebuild"): SIGHUP hands
// the replacement VM the outgoing one's plain-data declaration through
// context.script_state, and the shadow VM runs init/start against a sealed
// API — every mutating face raises instead of touching the live runtime — so
// a candidate that writes on boot is rejected before it is promoted. A
// refusal anywhere (handoff, init, bindings) retains the old service, its
// handlers and its module state.
void lua_reload_handoff() {
    Context c;
    const std::string xml =
        "<Contracts><Entity name=\"Avatar\" id=\"1\"><ServerMethods>"
        "<Method name=\"get\" id=\"2\" mode=\"request\" caller=\"server\">"
        "<Returns><Field name=\"level\" id=\"1\" type=\"UINT16\"/></Returns>"
        "</Method></ServerMethods></Entity></Contracts>";
    const auto def = c.path("avatar.def");
    write(def, xml);
    const auto script = c.path("service.lua");
    write(script, R"(local service = {}
local counter = 0
function service.init(context)
  counter = (context.script_state and context.script_state.counter) or 0
  service.tag = "v1"
end
function service.handoff()
  return { counter = counter }
end
function service.message(payload)
  if payload == "spawn" then
    return mistlane.entity.spawn(1, 1, 3, 42) and "spawned" or "duplicate"
  end
  if payload == "bump" then counter = counter + 1 end
  if payload == "count" then return service.tag..":"..counter end
  return "echo:"..payload
end
service.entities = {
  Avatar = {
    get = function(args, ctx) return { level = counter } end,
  },
}
return service)");
    const auto listen = port();
    auto& node = c.node("gameServer", listen,
        {"--script", script.string(), "--contract", def.string(), "--service-token", "s3cret"});
    auto peer = tcp(listen);
    equal(peer->request("MLS1 gameServer inst-1 s3cret"), "MLS1 OK");
    equal(peer->request("spawn"), "spawned");
    equal(peer->request("bump"), "echo:bump");
    equal(peer->request("count"), "v1:1");
    // The entity handler reads the live VM's module state through the
    // dispatcher, so a typed call pins which bundle is bound.
    EntityContract contract(xml);
    SprotoCodec codec(contract);
    const auto frame = [](std::uint64_t correlation, const std::string& body) {
        return "MLE1" + encode_entity_envelope({1, 1, 3, 42, kEntitySideServer,
            2, correlation, 1, 0, body});
    };
    const auto reply_of = [](const std::string& body) -> EntityReply {
        require(body.size() >= 12 && body.starts_with("MLE1"), "expected an MLE1 reply");
        const auto decoded = decode_entity_reply(std::string_view(body).substr(12));
        require(decoded.has_value(), "reply did not decode");
        return *decoded;
    };
    auto reply = reply_of(peer->request(frame(7, {})));
    require(!reply.is_error, "the v1 handler was not bound");
    EntityValues got;
    std::string reason;
    const auto* method = &contract.entities()[0].server_methods[0];
    require(codec.decode_args(method->returns, reply.payload, got, reason) && reason.empty()
            && std::get<std::int64_t>(got.at("level").data) == 1,
        "the v1 handler did not read the module counter");
    // The replacement bundle tags itself v2 and adopts the handed-off counter;
    // nothing else changes, so every observable after the reload must name v2
    // with the carried state.
    write(script, R"(local service = {}
local counter = 0
function service.init(context)
  counter = (context.script_state and context.script_state.counter) or 0
  service.tag = "v2"
end
function service.handoff()
  return { counter = counter }
end
function service.message(payload)
  if payload == "spawn" then
    return mistlane.entity.spawn(1, 1, 3, 42) and "spawned" or "duplicate"
  end
  if payload == "bump" then counter = counter + 1 end
  if payload == "count" then return service.tag..":"..counter end
  return "echo:"..payload
end
service.entities = {
  Avatar = {
    get = function(args, ctx) return { level = counter } end,
  },
}
return service)");
    node.signal(SIGHUP);
    node.ready("script reloaded");
    equal(peer->request("count"), "v2:1");
    equal(peer->request("bump"), "echo:bump");
    equal(peer->request("count"), "v2:2");
    reply = reply_of(peer->request(frame(8, {})));
    require(!reply.is_error, "the reloaded handlers were not re-bound");
    require(codec.decode_args(method->returns, reply.payload, got, reason) && reason.empty()
            && std::get<std::int64_t>(got.at("level").data) == 2,
        "the reloaded handler did not read the carried counter");
    equal(peer->request("spawn"), "duplicate");
    peer.reset();
    node.signal(SIGTERM);
    require(node.wait() == 0, "gameServer graceful shutdown failed");
    std::cout << "entity.reload.safe: handoff carries module state, the promoted VM serves it [PASS]\n";
}
// Every refusal face of the same seam: a throwing init, a handoff that is not
// a function, an init that writes through the sealed API, and a broken
// implementation table each keep the old service — its handlers, its module
// state and the live runtime untouched (the sealed spawn of the shadow never
// lands, so the address is still free on the live plane).
void lua_reload_isolation() {
    Context c;
    const auto def = c.path("avatar.def");
    write(def,
        "<Contracts><Entity name=\"Avatar\" id=\"1\"><ServerMethods>"
        "<Method name=\"get\" id=\"2\" mode=\"request\" caller=\"server\">"
        "<Returns><Field name=\"level\" id=\"1\" type=\"UINT16\"/></Returns>"
        "</Method></ServerMethods></Entity></Contracts>");
    const auto good = R"(local service = {}
local counter = 0
function service.init(context)
  counter = (context.script_state and context.script_state.counter) or 0
  service.tag = "v1"
end
function service.message(payload)
  if payload == "spawn" then
    return mistlane.entity.spawn(1, 1, 3, 42) and "spawned" or "duplicate"
  end
  if payload == "spawn99" then
    return mistlane.entity.spawn(1, 1, 3, 99) and "spawned" or "duplicate"
  end
  if payload == "bump" then counter = counter + 1 end
  if payload == "count" then return service.tag..":"..counter end
  return "echo:"..payload
end
service.entities = {
  Avatar = {
    get = function(args, ctx) return { level = counter } end,
  },
}
return service)";
    const auto script = c.path("service.lua");
    write(script, good);
    const auto listen = port();
    auto& node = c.node("gameServer", listen,
        {"--script", script.string(), "--contract", def.string(), "--service-token", "s3cret"});
    auto peer = tcp(listen);
    equal(peer->request("MLS1 gameServer inst-1 s3cret"), "MLS1 OK");
    equal(peer->request("spawn"), "spawned");
    equal(peer->request("bump"), "echo:bump");
    equal(peer->request("count"), "v1:1");
    const auto rejected = [&](const std::string& bundle, std::string_view why,
                              const std::string& count) {
        write(script, bundle);
        node.signal(SIGHUP);
        node.ready("reload rejected");
        require(peer->request("count") == count, std::string(why)+": old service not retained");
    };
    rejected("return {}", "a bundle without message is rejected", "v1:1");
    rejected("return {init=function() error('boom') end, message=function(p) return p end}",
        "a throwing init is rejected", "v1:1");
    // A bad handoff is only visible on the FOLLOWING reload (the outgoing VM
    // runs it), so the bad bundle is promoted first and the next reload must
    // refuse: the metatable table refuses the plain-data walk.
    write(script, R"(local service = {}
local counter = 0
function service.init(context)
  counter = (context.script_state and context.script_state.counter) or 0
  service.tag = "v2"
end
local sealed = setmetatable({}, {})
function service.handoff() return sealed end
function service.message(payload)
  if payload == "spawn99" then
    return mistlane.entity.spawn(1, 1, 3, 99) and "spawned" or "duplicate"
  end
  if payload == "bump" then counter = counter + 1 end
  if payload == "count" then return service.tag..":"..counter end
  return "echo:"..payload
end
service.entities = {
  Avatar = {
    get = function(args, ctx) return { level = counter } end,
  },
}
return service)");
    node.signal(SIGHUP);
    node.ready("script reloaded");
    equal(peer->request("count"), "v2:0");
    write(script, good);
    node.signal(SIGHUP);
    node.ready("reload rejected");
    require(peer->request("count") == "v2:0", "a metatable handoff must retain the old service");
    // The shadow writes through the sealed entity API during init: the raise
    // must fail the reload, and the live runtime must not hold the address.
    rejected("return {init=function() mistlane.entity.spawn(1, 1, 3, 99) end,"
        "message=function(p) return p end}",
        "a sealed spawn in init is rejected", "v2:0");
    equal(peer->request("spawn99"), "spawned");
    require(peer->request("spawn99") == "duplicate",
        "the shadow's sealed spawn leaked onto the live runtime");
    rejected("return {message=function(p) return p end, entities={Avatar={get=123}}}",
        "a broken implementation table is rejected", "v2:0");
    equal(peer->request("spawn99"), "duplicate");
    // The surviving bundle is the one the refusals kept: its handlers still
    // answer through the dispatcher, bound to the untouched VM.
    node.signal(SIGTERM);
    require(node.wait() == 0, "gameServer graceful shutdown failed");
    std::cout << "entity.reload.isolation: every refusal keeps the old service and runtime [PASS]\n";
}
}
