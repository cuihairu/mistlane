// SPDX-License-Identifier: Apache-2.0
#include "test_common.hpp"
namespace mistlane::test {
void probe_control() {
    Context c;
    write(c.path("echo.lua"), echo);
    // Capacity 2 with a 0.5 readiness gate saturates at one session; two ticks flip state.
    const auto work=port(), probe=port(), work_metrics=port(), guarded=port(), guard_probe=port();
    c.node("gameServer", work, {"--script", c.path("echo.lua").string(), "--max-connections", "2",
        "--overload-ratio", "0.5", "--ready-hysteresis", "2", "--probe-listen", endpoint(probe),
        "--metrics-listen", endpoint(work_metrics)});
    const auto ask = [](unsigned listen, const std::string& line) {
        RawSocket raw(listen);
        raw.send(line + "\n");
        return raw.receive_line();
    };
    const auto metric_value = [](const std::string& body, const std::string& sample) {
        const auto at = body.find(sample);
        require(at != std::string::npos, "missing metric " + sample);
        return std::strtod(body.c_str() + at + sample.size(), nullptr);
    };
    equal(ask(probe, "PROBE1 n1 liveness"), "PROBE1 n1 ok alive");
    equal(ask(probe, "PROBE9 n2 liveness"), "PROBE1 n2 err version");
    equal(ask(probe, "PROBE1 n3 status"), "PROBE1 n3 err command");
    equal(ask(probe, "PROBE1 n4"), "PROBE1 n4 err malformed");
    equal(ask(probe, "PROBE1 bad!n liveness"), "PROBE1 - err malformed");
    equal(ask(probe, "hello world"), "PROBE1 world err version");
    // Every ask is a fresh connection, so reply buffers and teardown must hold
    // up across repeated probe lifetimes.
    for (int i = 0; i < 50; ++i) {
        const auto nonce = "loop" + std::to_string(i);
        equal(ask(probe, "PROBE1 " + nonce + " liveness"), "PROBE1 " + nonce + " ok alive");
    }
    const auto readiness = ask(probe, "PROBE1 n5 readiness");
    require(readiness.starts_with("PROBE1 n5 ok ready active=0 capacity=2 "),
        "unexpected readiness reply: " + readiness);
    require(readiness.find(" lag_ms=") != std::string::npos, "readiness lag summary missing");
    // A probe connection that never asks must be closed by the server read deadline.
    {
        RawSocket silent(probe);
        eventually([&] { return silent.closed(); });
    }
    // Saturating flips to notready, releasing the session flips back.
    auto session = tcp(work);
    equal(session->request("busy"), "busy");
    eventually([&] {
        return ask(probe, "PROBE1 n6 readiness").find(" ok notready active=1 capacity=2 ") != std::string::npos;
    });
    // The readiness gauge must follow the admission gate, not just start/stop.
    {
        const auto busy = http("http://"+endpoint(work_metrics)+"/metrics");
        require(metric_value(busy.body, "mistlane_ready{role=\"gameServer\"} ") == 0,
            "readiness gauge must drop with the admission gate: " + busy.body);
    }
    session.reset();
    eventually([&] {
        return ask(probe, "PROBE1 n7 readiness").find(" ok ready active=0 capacity=2 ") != std::string::npos;
    });
    {
        const auto recovered = http("http://"+endpoint(work_metrics)+"/metrics");
        require(metric_value(recovered.body, "mistlane_ready{role=\"gameServer\"} ") == 1,
            "readiness gauge must recover with the admission gate: " + recovered.body);
    }
    {
        // Minimal identity check: a token env makes unauthenticated probes fail.
        ScopedEnv token("MISTLANE_PROBE_TOKEN", "s3cret");
        c.node("gameServer", guarded, {"--script", c.path("echo.lua").string(), "--max-connections", "5",
            "--probe-listen", endpoint(guard_probe)});
    }
    equal(ask(guard_probe, "PROBE1 n8 readiness"), "PROBE1 n8 err auth");
    equal(ask(guard_probe, "PROBE1 n9 wrong readiness"), "PROBE1 n9 err auth");
    const auto authenticated = ask(guard_probe, "PROBE1 n10 s3cret readiness");
    require(authenticated.starts_with("PROBE1 n10 ok ready active=0 capacity=5 "),
        "unexpected authenticated readiness: " + authenticated);

    // Cross consults shard readiness for new rooms; established rooms keep flowing.
    const auto shard=port(), shard_probe=port(), ingress=port(), ingress_metrics=port(),
        ingress2=port(), plain=port(), broken=port();
    c.node("crossServer", shard, {"--script", c.path("echo.lua").string(), "--max-connections", "4",
        "--overload-ratio", "0.5", "--ready-hysteresis", "2", "--probe-listen", endpoint(shard_probe)});
    const auto upstream = endpoint(shard) + ":" + std::to_string(shard_probe);
    c.cross_gateway( ingress, {"--upstream", upstream, "--metrics-listen", endpoint(ingress_metrics)});
    c.cross_gateway( ingress2, {"--upstream", upstream});
    const auto join = [](cli::Client& peer, const std::string& room) {
        return peer.request("MLC1 1 realm g " + room);
    };
    auto room1 = tcp(ingress), room2 = tcp(ingress2);
    equal(join(*room1, "r1"), "MLC1 OK 1");
    equal(join(*room2, "r2"), "MLC1 OK 1");
    eventually([&] {
        return ask(shard_probe, "PROBE1 nr readiness").find(" ok notready active=2 ") != std::string::npos;
    }, 5s, "shard to reach notready at active=2");
    equal(join(*tcp(ingress), "r3"), "MLC1 ERR NOT_READY");
    equal(join(*tcp(ingress2), "r4"), "MLC1 ERR NOT_READY");
    // Admission rejections include cross-gated joins, not only local limits.
    {
        const auto rejects = http("http://"+endpoint(ingress_metrics)+"/metrics");
        require(metric_value(rejects.body, "mistlane_admission_rejections_total{role=\"gateway\"} ") >= 1,
            "cross admission rejects are not counted: " + rejects.body);
    }
    room1.reset();
    eventually([&] {
        return ask(shard_probe, "PROBE1 nr2 readiness").find(" ok ready active=1 ") != std::string::npos;
    }, 5s, "shard to recover ready at active=1");
    equal(join(*tcp(ingress), "r5"), "MLC1 OK 1");
    // Without a probe port the cross node cannot gate, so it keeps forwarding.
    c.cross_gateway( plain, {"--upstream", endpoint(shard)});
    equal(join(*tcp(plain), "r7"), "MLC1 OK 1");
    // A dead probe listener is an upstream failure, not a silent accept.
    c.cross_gateway( broken, {"--upstream", endpoint(shard) + ":" + std::to_string(port())});
    equal(join(*tcp(broken), "r8"), "MLC1 ERR UPSTREAM");

    // Bracketed IPv6 listen and upstream addresses, gated and ungated.
    const auto v6_shard=port(), v6_probe=port(), v6_gated=port(), v6_plain=port();
    auto& v6_node = c.spawn({self().string(), "--role", "crossServer",
        "--listen", "[::1]:" + std::to_string(v6_shard),
        "--script", c.path("echo.lua").string(), "--max-connections", "4",
        "--overload-ratio", "0.5", "--ready-hysteresis", "2",
        "--probe-listen", "[::1]:" + std::to_string(v6_probe)});
    v6_node.ready("ready crossServer");
    c.cross_gateway( v6_gated, {"--upstream",
        "[::1]:" + std::to_string(v6_shard) + ":" + std::to_string(v6_probe)});
    equal(join(*tcp(v6_gated), "v1"), "MLC1 OK 1");
    c.cross_gateway( v6_plain, {"--upstream", "[::1]:" + std::to_string(v6_shard)});
    equal(join(*tcp(v6_plain), "v2"), "MLC1 OK 1");

    // An oversized probe request line is closed without any reply bytes.
    {
        RawSocket long_line(probe);
        long_line.send(std::string(300, 'A') + "\n");
        must_fail([&] { (void)long_line.receive_line(); });
    }
    // Concurrent probe connections are capped; rejected sockets close at once.
    {
        std::vector<std::unique_ptr<RawSocket>> held;
        for (int i = 0; i < 64; ++i) held.push_back(std::make_unique<RawSocket>(probe));
        RawSocket over(probe);
        eventually([&] { return over.closed_now(); }, 5s,
            "peer cap did not close the 65th probe connection");
        held.clear();
        eventually([&] {
            return ask(probe, "PROBE1 freed liveness") == "PROBE1 freed ok alive";
        }, 5s, "probe slots did not free after held peers closed");
    }
}
}
