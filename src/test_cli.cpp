// SPDX-License-Identifier: Apache-2.0
#include "test_common.hpp"
namespace mistlane::test {
void cli_bot() {
    Context c; write(c.path("echo.lua"),echo);
    const auto listen=port(); c.node("gameServer",listen,{"--script",c.path("echo.lua").string()});
    auto& bot=c.spawn({self().string(),"bot","--url","tcp://"+endpoint(listen),
        "--clients","4","--count","3","--payload","native-bot","--expect-echo"});
    require(bot.wait()==0 && bot.output().find("completed=12")!=std::string::npos,"bot request accounting failed");
    require(c.command({self().string(),"bot","--url","tcp://"+endpoint(listen),"--expect","mismatch"})!=0,
        "bot ignored response mismatch");
    require(c.command({self().string(),"bot","--count","0"})!=0,"bot accepted invalid count");
    require(c.command({self().string(),"test","--case","does-not-exist"})!=0,"unknown test returned success");
    // A listening socket that never replies validates a bounded receive timeout.
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor stalled(io,{boost::asio::ip::address_v4::loopback(),0});
    const auto stalled_port=stalled.local_endpoint().port();
    auto& timeout=c.spawn({self().string(),"bot","--url","tcp://"+endpoint(stalled_port),"--timeout-ms","100"});
    require(timeout.wait(3s)!=0,"bot receive deadline was not enforced");
}
// The business scenario end to end: a contract-hosting gameServer, a ramping
// scenario bot (MLS1 identity, per-connection spawn, login -> joinRoom ->
// move/heartbeat mix), and the embedded dashboard whose /metrics must agree
// with the framework counters and with the script's rooms gauge.
void business_bot() {
    Context c;
    write(c.path("business.def"), R"(<Contracts>
  <Entity name="Player" id="2">
    <ServerMethods>
      <Method name="login" id="1" mode="request" caller="server"><Arg name="token" id="0" type="STRING"/><Returns><Field name="ok" id="1" type="UINT16"/></Returns></Method>
      <Method name="joinRoom" id="2" mode="request" caller="server"><Arg name="room" id="0" type="UINT32"/><Returns><Field name="ok" id="1" type="UINT16"/></Returns></Method>
      <Method name="move" id="3" mode="request" caller="server"><Arg name="x" id="0" type="INT32"/><Arg name="y" id="1" type="INT32"/><Returns><Field name="seq" id="2" type="UINT32"/></Returns></Method>
      <Method name="heartbeat" id="4" mode="request" caller="server"><Returns><Field name="seq" id="0" type="UINT32"/></Returns></Method>
    </ServerMethods>
  </Entity>
</Contracts>)");
    write(c.path("business.lua"), R"(local service = {}
local rooms, seq = {}, 0
function service.message(payload)
    local id = tonumber(string.match(payload, "^spawn (%d+)$"))
    if id then return mistlane.entity.spawn(1, 2, 1, id, 1) and "spawned" or "duplicate" end
    if payload == "guards" then
        local out = {}
        local probes = {
            function() return mistlane.entity.spawn("x", 2, 1, 1, 1) end,
            function() return mistlane.entity.spawn(1, -1, 1, 1, 1) end,
            function() return mistlane.entity.spawn(1, 2, 1, 1, "x") end,
            function() return mistlane.entity.destroy(1, 2, 1) end,
            function() return mistlane.entity.destroy(1, 2, 1, "x") end,
            function() return mistlane.metrics.gauge("probe", "not-a-number") end,
        }
        for _, probe in ipairs(probes) do
            local ok, err = pcall(probe)
            out[#out+1] = tostring(ok)..":"..tostring(err):match("[^:]+$")
        end
        return table.concat(out, "|")
    end
    return "echo:"..payload
end
service.entities = {
    Player = {
        login = function(args, ctx) assert(#args.token > 0) return { ok = 1 } end,
        joinRoom = function(args, ctx)
            rooms[args.room] = (rooms[args.room] or 0) + 1
            mistlane.metrics.gauge("rooms", (function() local n = 0 for _ in pairs(rooms) do n = n + 1 end return n end)())
            return { ok = 1 }
        end,
        move = function(args, ctx) seq = seq + 1 return { seq = seq } end,
        heartbeat = function(args, ctx) seq = seq + 1 return { seq = seq } end,
    },
}
return service)");
    const auto listen = port(), dashboard = port(), metrics = port();
    auto& node = c.node("gameServer", listen, {"--script", c.path("business.lua").string(),
        "--contract", c.path("business.def").string(), "--service-token", "s3cret",
        "--metrics-listen", endpoint(metrics), "--dashboard-listen", endpoint(dashboard)});
    // A scenario without its contract refuses before touching the network.
    require(c.command({self().string(), "bot", "--url", "tcp://"+endpoint(listen),
        "--scenario", "business"}) != 0, "scenario without contract must refuse");
    // Four ramping connections: login + join + six room steps, every second
    // step a heartbeat (3 beats, 3 moves per connection), spread over two rooms.
    auto& bot = c.spawn({self().string(), "bot", "--url", "tcp://"+endpoint(listen),
        "--scenario", "business", "--contract", c.path("business.def").string(),
        "--service-token", "s3cret", "--spawn", "--clients", "4", "--count", "6",
        "--heartbeat-every", "2", "--rooms", "2", "--ramp-ms", "100"});
    require(bot.wait() == 0 && bot.output().find("failed_clients=0") != std::string::npos,
        "business bot failed:\n" + bot.output());
    const auto& out = bot.output();
    require(out.find("logins_ok=4") != std::string::npos && out.find("joins_ok=4") != std::string::npos
        && out.find("moves_ok=12") != std::string::npos && out.find("heartbeats_ok=12") != std::string::npos,
        "scenario counters wrong:\n" + out);
    // Adapter guards answer as pcall-visible Lua errors with reasons instead
    // of killing the session: bad ids, a bad generation and a non-numeric
    // gauge are each refused, and the node keeps serving afterwards. The
    // tokened node demands the MLS1 identity on the raw leg first.
    const auto send_frame = [](RawSocket& raw, std::string_view payload) {
        std::string frame(4, '\0');
        auto length = static_cast<std::uint32_t>(payload.size());
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(length & 255); length >>= 8; }
        raw.send(frame + std::string(payload));
    };
    const auto read_frame = [](RawSocket& raw) {
        std::size_t length = 0;
        for (unsigned char ch : raw.receive(4)) length = (length << 8) | ch;
        return raw.receive(length);
    };
    RawSocket leg(listen);
    send_frame(leg, "MLS1 gameServer biz-1 s3cret");
    equal(read_frame(leg), "MLS1 OK");
    const auto ask_guard = [&](std::string_view payload) {
        send_frame(leg, payload);
        return read_frame(leg);
    };
    equal(ask_guard("guards"),
        "false: entity.spawn expects scope, type, incarnation, entity_id integers|"
        "false: entity.spawn expects scope, type, incarnation, entity_id integers|"
        "false: entity.spawn generation must be a non-negative 32-bit integer|"
        "false: entity.destroy expects scope, type, incarnation, entity_id integers|"
        "false: entity.destroy expects scope, type, incarnation, entity_id integers|"
        "false: value must be a number");
    equal(ask_guard("echo"), "echo:echo");
    // The dashboard page is self-contained HTML; /metrics is Prometheus text.
    const auto page = http_request({"http://" + endpoint(dashboard) + "/dashboard"});
    require(page.status == 200 && page.body.rfind("<!doctype html>", 0) == 0
        && page.body.find("mistlane_script_gauge") != std::string::npos, "dashboard page missing");
    require(http_request({"http://" + endpoint(dashboard) + "/nope"}).status == 404, "dashboard 404 expected");
    const auto value_of = [](const std::string& body, const std::string& name,
                             const std::string& contains) -> std::string {
        std::size_t at = 0;
        while ((at = body.find(name, at)) != std::string::npos) {
            const auto end = body.find('\n', at);
            const auto line = body.substr(at, end == std::string::npos ? std::string::npos : end - at);
            if (line.find(contains) != std::string::npos && line.rfind("# ", 0) != 0)
                return line.substr(line.rfind(' ') + 1);
            at = end;
        }
        return {};
    };
    const auto dash = http_request({"http://" + endpoint(dashboard) + "/metrics"}).body;
    const auto exposed = http_request({"http://" + endpoint(metrics) + "/metrics"}).body;
    require(value_of(dash, "mistlane_ready", "gameServer") == "1", "dashboard readiness missing:\n" + dash);
    require(value_of(dash, "mistlane_ready", "gameServer") == value_of(exposed, "mistlane_ready", "gameServer"),
        "dashboard and exposer disagree");
    require(value_of(dash, "mistlane_script_gauge", "\"rooms\"") == "2", "rooms gauge wrong:\n" + dash);
    require(value_of(dash, "mistlane_entity_results_total", "\"ok\"") == "32",
        "entity ok counter wrong:\n" + dash);
    // Every connection sent one MLS1, one spawn and eight steps; the guard
    // probe leg adds one MLS1 plus two more frames. Each frame read in
    // produced exactly one reply, so rx and tx agree.
    require(value_of(dash, "mistlane_messages_total", "\"rx\"") == "43"
        && value_of(dash, "mistlane_messages_total", "\"tx\"") == "43",
        "message accounting wrong:\n" + dash);
    // Stop the node inside the test: a teardown kill would skip its graceful
    // shutdown, and killed children never flush coverage records.
    node.signal(SIGTERM);
    require(node.wait() == 0, "business node should stop cleanly");
}
}
