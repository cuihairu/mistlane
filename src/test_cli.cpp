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
    c.node("gameServer", listen, {"--script", c.path("business.lua").string(),
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
    // Every connection sent one MLS1, one spawn and eight steps; each frame
    // read in produced exactly one reply, so rx and tx agree.
    require(value_of(dash, "mistlane_messages_total", "\"rx\"") == "40"
        && value_of(dash, "mistlane_messages_total", "\"tx\"") == "40",
        "message accounting wrong:\n" + dash);
}
}
