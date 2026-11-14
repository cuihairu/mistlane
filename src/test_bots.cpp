// SPDX-License-Identifier: Apache-2.0
// CI smoke for tools/bots: the standalone driver rides a real contract-hosting
// gameServer through a load plan, then runs the injection scenarios against it
// plus a journal-backed data node, and both nodes must still stop cleanly.
#include "test_common.hpp"
#include <filesystem>
namespace mistlane::test {
void bots_smoke() {
    const auto tool = self().parent_path()/"mistlane-bots";
    require(fs::is_regular_file(tool),
        "mistlane-bots missing next to the test binary (build the tools/bots target)");
    Context c;
    // The business fixtures mirror business_bot(): the four server methods the
    // scenario resolves by name, the rooms gauge, and the spawn message.
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
function service.message(payload)
    local id = tonumber(string.match(payload, "^spawn (%d+)$"))
    if id then return mistlane.entity.spawn(1, 2, 1, id, 1) and "spawned" or "duplicate" end
    return "echo:"..payload
end
service.entities = {
    Player = {
        login = function(args, ctx) assert(#args.token > 0) return { ok = 1 } end,
        joinRoom = function(args, ctx) return { ok = 1 } end,
        move = function(args, ctx) return { seq = args.x } end,
        heartbeat = function(args, ctx) return { seq = 0 } end,
    },
}
return service)");
    write(c.path("echo.lua"), "local service = {}\nfunction service.message(payload) return \"echo:\"..tostring(payload) end\nreturn service");
    const auto listen = port(), data = port(), metrics = port();
    auto& node = c.node("gameServer", listen, {"--script", c.path("business.lua").string(),
        "--contract", c.path("business.def").string(), "--service-token", "s3cret",
        "--metrics-listen", endpoint(metrics)});
    auto& data_node = c.node("data", data, {"--script", c.path("echo.lua").string(),
        "--journal", c.path("data.sqlite").string()});
    // Load plan: the same shape as business.bot (4 ramping connections, login +
    // join + six room steps, every second step a heartbeat, spread over rooms).
    write(c.path("load.toml"),
        "[run]\nurl = \"tcp://127.0.0.1:" + std::to_string(listen) + "\"\n"
        "concurrency = 4\nrequests = 6\nramp_ms = 100\ntimeout_ms = 5000\n"
        "[scenario]\ncontract = \"" + c.path("business.def").string() + "\"\n"
        "service_token = \"s3cret\"\nspawn = true\nrooms = 2\nheartbeat_every = 2\n");
    auto& load = c.spawn({tool.string(), "--plan", c.path("load.toml").string()});
    require(load.wait() == 0, "bots load plan failed:\n" + load.output());
    const auto& out = load.output();
    require(out.find("failed_connections=0") != std::string::npos
        && out.find("completed=32") != std::string::npos
        && out.find("type=login ok=4") != std::string::npos
        && out.find("type=joinRoom ok=4") != std::string::npos
        && out.find("type=move ok=12") != std::string::npos
        && out.find("type=heartbeat ok=12") != std::string::npos,
        "load accounting wrong:\n" + out);
    // A malformed plan must be refused before touching the network.
    write(c.path("bad.toml"), "[run]\nurl = \"tcp://127.0.0.1:" + std::to_string(listen) + "\"\n"
        "requests = 3\nnonsense_key = 1\n[scenario]\ncontract = \""
        + c.path("business.def").string() + "\"\n");
    require(c.command({tool.string(), "--plan", c.path("bad.toml").string()}) != 0,
        "unknown plan key was accepted");
    // Injection plan: the guards must hold against a journal-backed data node.
    write(c.path("inject.toml"),
        "[run]\nurl = \"tcp://127.0.0.1:" + std::to_string(listen) + "\"\ntimeout_ms = 5000\n"
        "[inject]\ncontract = \"" + c.path("business.def").string() + "\"\n"
        "service_token = \"s3cret\"\nurl_data = \"tcp://127.0.0.1:" + std::to_string(data) + "\"\n"
        "url_metrics = \"http://127.0.0.1:" + std::to_string(metrics) + "\"\n"
        "slow_wait_ms = 100\nstorm_rounds = 2\nstorm_burst = 4\n");
    auto& inject = c.spawn({tool.string(), "--plan", c.path("inject.toml").string(),
        "--mode", "inject"});
    require(inject.wait() == 0, "bots inject plan broke a guard:\n" + inject.output());
    const auto& guards = inject.output();
    require(guards.find("verdict=BROKEN") == std::string::npos
        && guards.find("guard=bad-frames.truncated-envelope verdict=held") != std::string::npos
        && guards.find("guard=bad-frames.bad-side verdict=held") != std::string::npos
        && guards.find("guard=bad-frames.oversize-refused verdict=held") != std::string::npos
        && guards.find("guard=late-acks.durable-dedup-gap-stale verdict=held") != std::string::npos
        && guards.find("guard=slow-clients.backpressure-reply-after-stall verdict=held") != std::string::npos
        && guards.find("guard=disconnect-storm.survives-and-serves verdict=held") != std::string::npos,
        "injection guard verdicts wrong:\n" + guards);
    // Stop both nodes inside the test: killed children skip graceful shutdown
    // and never flush coverage records.
    node.signal(SIGTERM);
    require(node.wait() == 0, "gameServer should stop cleanly after the bots run");
    data_node.signal(SIGTERM);
    require(data_node.wait() == 0, "data node should stop cleanly");
}
}
