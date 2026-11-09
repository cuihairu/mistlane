// SPDX-License-Identifier: Apache-2.0
#include "test_common.hpp"
namespace mistlane::test {
void entity_transport() {
    const std::string xml =
        "<Contracts><Entity name=\"Avatar\" id=\"1\"><ServerMethods>"
        "<Method name=\"get\" id=\"2\" mode=\"request\" caller=\"server\">"
        "<Arg name=\"account\" id=\"0\" type=\"INT32\"/>"
        "<Returns><Field name=\"level\" id=\"1\" type=\"UINT16\"/></Returns></Method>"
        "<Method name=\"touch\" id=\"3\" mode=\"notify\" caller=\"server\">"
        "<Arg name=\"x\" id=\"0\" type=\"INT32\"/></Method>"
        "<Method name=\"admin\" id=\"5\" mode=\"request\" caller=\"owner\">"
        "<Returns><Field name=\"ok\" id=\"0\" type=\"UINT16\"/></Returns></Method>"
        "<Method name=\"missing\" id=\"6\" mode=\"request\" caller=\"server\">"
        "<Returns><Field name=\"ok\" id=\"0\" type=\"UINT16\"/></Returns></Method>"
        "</ServerMethods></Entity></Contracts>";
    Context c;
    const auto def = c.path("avatar.def");
    write(def, xml);
    const auto script = c.path("avatar.lua");
    write(script, R"(local service = {}
function service.message(payload)
    if payload == "spawn" then
        return mistlane.entity.spawn(1, 1, 3, 42) and "spawned" or "duplicate"
    end
    if payload == "badspawn" then
        -- Negative and overflowing ids must be refused, not wrapped to a
        -- different address; the runtime then reports a duplicate spawn.
        local ok = pcall(function() return mistlane.entity.spawn(-1, 1, 3, 42) end)
        local over = pcall(function() return mistlane.entity.spawn(1, 1, 3, 42, 4294967296) end)
        if ok or over then return "accepted" end
        return "refused"
    end
    return "echo:"..payload
end
service.entities = {
    Avatar = {
        get = function(args, ctx)
            assert(args.account == 4)
            assert(ctx.caller_kind == "server")
            assert(ctx.caller_id > 0)
            assert(ctx.method_id == 2)
            return { level = 7 }
        end,
        touch = function(args, ctx)
            assert(args.x == 4)
            return nil
        end,
    },
}
return service)");
    const auto internal = port(), anonymous = port(), metrics = port();
    c.node("gameServer", internal,
        {"--script", script.string(), "--contract", def.string(), "--service-token", "s3cret",
         "--metrics-listen", endpoint(metrics)});
    c.node("gameServer", anonymous, {"--script", script.string(), "--contract", def.string()});

    EntityContract contract(xml);
    SprotoCodec codec(contract);
    const ContractMethod* get = nullptr;
    for (const auto& e : contract.entities())
        for (const auto& m : e.server_methods)
            if (m.name == "get") get = &m;
    require(get != nullptr, "test contract lost the get method");
    const auto payload = [&](std::int64_t value) -> std::string {
        auto encoded = codec.encode_returns(get->args, {{"account", value}});
        require(encoded.has_value(), "test arg encode failed");
        return *encoded;
    };
    const auto frame = [](std::uint64_t correlation, std::uint32_t method, std::uint32_t type_id,
                          std::uint32_t incarnation, std::uint32_t generation, const std::string& body) {
        return "MLE1" + encode_entity_envelope({1, type_id, incarnation, 42, kEntitySideServer,
            static_cast<std::uint16_t>(method), correlation, generation, 0, body});
    };
    const auto reply_of = [](const std::string& body) -> EntityReply {
        require(body.size() >= 12 && body.starts_with("MLE1"), "expected an MLE1 reply frame");
        const auto decoded = decode_entity_reply(std::string_view(body).substr(12));
        require(decoded.has_value(), "reply body did not decode");
        return *decoded;
    };
    const auto correlation_of = [](const std::string& body) {
        require(body.size() >= 12, "reply frame too short");
        std::uint64_t value = 0;
        for (std::size_t i = 4; i < 12; ++i) value = value << 8 | static_cast<unsigned char>(body[i]);
        return value;
    };

    // The authorized internal service presents MLS1, seeds the runtime over the
    // same leg, then drives the entity path.
    auto peer = tcp(internal);
    equal(peer->request("MLS1 gameServer inst-1 s3cret"), "MLS1 OK");
    equal(peer->request("spawn"), "spawned");
    equal(peer->request("badspawn"), "refused");
    // The refused spawn did not wrap to a valid address: the seeded entity
    // still answers a request at its real address, not the wrapped one.
    auto wrapped = peer->request(frame(80, 2, 1, 3, 1, payload(4)));
    require(correlation_of(wrapped) == 80 && !reply_of(wrapped).is_error,
        "a refused negative spawn leaked a wrapped address");
    std::cout << "entity.transport spawn range: negative/overflow ids refused, no wrap [PASS]\n";

    auto ok = peer->request(frame(77, 2, 1, 3, 1, payload(4)));
    require(correlation_of(ok) == 77 && !reply_of(ok).is_error, "happy request was not answered OK");
    EntityValues got;
    std::string reason;
    require(codec.decode_args(get->returns, reply_of(ok).payload, got, reason) && reason.empty()
            && std::get<std::int64_t>(got.at("level").data) == 7, "typed return did not round-trip");
    std::cout << "entity.transport request: MLE1 round-trip, typed return, trusted caller [PASS]\n";

    auto malformed = peer->request("MLE1short");
    require(correlation_of(malformed) == 0 && reply_of(malformed).is_error
            && reply_of(malformed).error == EntityError::INVALID
            && reply_of(malformed).reason == "malformed envelope", "malformed envelope was not typed INVALID");
    std::cout << "entity.transport malformed: typed INVALID with a reason [PASS]\n";

    auto unrouted = peer->request(frame(78, 2, 7, 3, 1, payload(4)));
    require(reply_of(unrouted).is_error && reply_of(unrouted).error == EntityError::UNROUTED, "unknown entity was not UNROUTED");
    auto stale = peer->request(frame(79, 2, 1, 3, 2, payload(4)));
    require(reply_of(stale).is_error && reply_of(stale).error == EntityError::STALE, "wrong generation was not STALE");
    std::cout << "entity.transport route: unknown entity UNROUTED, wrong generation STALE [PASS]\n";

    // A declared method with no implementation answers NO_IMPLEMENTATION.
    auto unbound = peer->request(frame(81, 6, 1, 3, 1, {}));
    require(reply_of(unbound).is_error && reply_of(unbound).error == EntityError::NO_IMPLEMENTATION, "unimplemented method was not NO_IMPLEMENTATION");
    std::cout << "entity.transport gate: unimplemented NO_IMPLEMENTATION [PASS]\n";

    // A notify (zero correlation) answers nothing; a follow-up frame is the
    // only reply the reader sees.
    {
        RawSocket raw(internal);
        const auto send_frame = [&](const std::string& body) {
            std::string header(4, '\0');
            const auto length = static_cast<std::uint32_t>(body.size());
            for (unsigned i = 0; i < 4; ++i) header[i] = static_cast<char>(length >> (24 - i*8));
            raw.send(header + body);
        };
        const auto read_frame = [&]() {
            std::string header = raw.receive(4);
            std::uint32_t length = 0;
            for (unsigned i = 0; i < 4; ++i) length = length << 8 | static_cast<unsigned char>(header[i]);
            return raw.receive(length);
        };
        send_frame("MLS1 gameServer inst-2 s3cret");
        equal(read_frame(), "MLS1 OK");
        // The runtime is node-global, so the seeded entity answers a notify
        // here too; a zero-correlation frame produces no bytes.
        send_frame(frame(0, 3, 1, 3, 1, payload(4)));
        send_frame("hi");
        equal(read_frame(), "echo:hi");
        std::cout << "entity.transport notify: handled silently, follow-up is the only reply [PASS]\n";
    }

    // A plain client leg holds no trusted service identity: server methods are
    // FORBIDDEN and owner methods too (owner attribution is not built), while
    // ordinary script traffic still flows on the same node.
    auto outsider = tcp(anonymous);
    equal(outsider->request("spawn"), "spawned");
    auto denied = outsider->request(frame(90, 2, 1, 3, 0, payload(4)));
    require(reply_of(denied).is_error && reply_of(denied).error == EntityError::FORBIDDEN, "a client leg was not FORBIDDEN a server method");
    auto owner_only = outsider->request(frame(91, 5, 1, 3, 0, {}));
    require(reply_of(owner_only).is_error && reply_of(owner_only).error == EntityError::FORBIDDEN, "a client leg was not FORBIDDEN an owner method");
    equal(outsider->request("hi"), "echo:hi");
    std::cout << "entity.transport gate: client leg FORBIDDEN, script traffic unaffected [PASS]\n";

    // The result label set is bounded and the verdicts were recorded.
    auto samples = http("http://" + endpoint(metrics) + "/metrics");
    require(samples.status == 200, "metrics endpoint failed");
    require(samples.body.find("mistlane_entity_results_total") != std::string::npos, "entity result metric missing");
    require(samples.body.find("result=\"ok\"") != std::string::npos, "no ok entity verdict recorded");
    require(samples.body.find("result=\"malformed\"") != std::string::npos, "no malformed entity verdict recorded");
    require(samples.body.find("result=\"forbidden\"") != std::string::npos, "no forbidden entity verdict recorded");
    std::cout << "entity.transport metrics: bounded result labels recorded [PASS]\n";
}


// The gateway fronts the contract host's trusted leg: a plain client reaches
// caller="server" methods through the relay, and the relay swaps the client's
// outer transport correlation for a fresh inner entity id, so two channels can
// reuse one outer id without colliding on the host's duplicate fence. Replies
// map back by inner id, a notify stays silent end to end, and a full table
// answers CAPACITY without forwarding.
void entity_transport_gateway() {
    const std::string xml =
        "<Contracts><Entity name=\"Avatar\" id=\"1\"><ServerMethods>"
        "<Method name=\"get\" id=\"2\" mode=\"request\" caller=\"server\">"
        "<Arg name=\"account\" id=\"0\" type=\"INT32\"/>"
        "<Returns><Field name=\"level\" id=\"1\" type=\"UINT16\"/></Returns></Method>"
        "<Method name=\"touch\" id=\"3\" mode=\"notify\" caller=\"server\">"
        "<Arg name=\"x\" id=\"0\" type=\"INT32\"/></Method>"
        "</ServerMethods></Entity></Contracts>";
    Context c;
    const auto def = c.path("avatar.def");
    write(def, xml);
    const auto script = c.path("avatar.lua");
    write(script, R"(local service = {}
function service.message(payload)
    if payload == "spawn" then
        return mistlane.entity.spawn(1, 1, 3, 42) and "spawned" or "duplicate"
    end
    return "echo:"..payload
end
service.entities = {
    Avatar = {
        get = function(args, ctx)
            assert(args.account == 4)
            assert(ctx.caller_kind == "server")
            assert(ctx.caller_id > 0)
            assert(ctx.method_id == 2)
            return { level = 7 }
        end,
        touch = function(args, ctx)
            assert(args.x == 4)
            return nil
        end,
    },
}
return service)");
    const auto host = port(), relay = port();
    c.node("gameServer", host,
        {"--script", script.string(), "--contract", def.string(), "--service-token", "s3cret"});
    c.node("gateway", relay, {"--upstream", endpoint(host), "--upstream-token", "s3cret",
        "--service-name", "gw-1"});

    EntityContract contract(xml);
    SprotoCodec codec(contract);
    const ContractMethod* get = nullptr;
    for (const auto& e : contract.entities())
        for (const auto& m : e.server_methods)
            if (m.name == "get") get = &m;
    require(get != nullptr, "test contract lost the get method");
    const auto payload = [&](std::int64_t value) -> std::string {
        auto encoded = codec.encode_returns(get->args, {{"account", value}});
        require(encoded.has_value(), "test arg encode failed");
        return *encoded;
    };
    const auto frame = [](std::uint64_t correlation, std::uint32_t method, std::uint32_t type_id,
                          std::uint32_t incarnation, std::uint32_t generation, const std::string& body) {
        return "MLE1" + encode_entity_envelope({1, type_id, incarnation, 42, kEntitySideServer,
            static_cast<std::uint16_t>(method), correlation, generation, 0, body});
    };
    const auto reply_of = [](const std::string& body) -> EntityReply {
        require(body.size() >= 12 && body.starts_with("MLE1"), "expected an MLE1 reply frame");
        const auto decoded = decode_entity_reply(std::string_view(body).substr(12));
        require(decoded.has_value(), "reply body did not decode");
        return *decoded;
    };
    const auto correlation_of = [](const std::string& body) {
        require(body.size() >= 12, "reply frame too short");
        std::uint64_t value = 0;
        for (std::size_t i = 4; i < 12; ++i) value = value << 8 | static_cast<unsigned char>(body[i]);
        return value;
    };

    // Entity traffic through the relay: the client's outer correlation is its
    // own id, the host's leg carries the gateway's trusted identity, and the
    // typed return round-trips with the outer correlation echoed back.
    auto first = tcp(relay);
    equal(first->request("spawn"), "spawned");
    auto ok = first->request(frame(42, 2, 1, 3, 1, payload(4)));
    require(correlation_of(ok) == 42 && !reply_of(ok).is_error, "gateway request was not answered OK");
    EntityValues got;
    std::string reason;
    require(codec.decode_args(get->returns, reply_of(ok).payload, got, reason) && reason.empty()
            && std::get<std::int64_t>(got.at("level").data) == 7, "typed return did not round-trip");
    std::cout << "entity.transport.gateway request: relayed MLE1, outer correlation echoed [PASS]\n";

    // Two client channels reusing one outer correlation must both succeed: the
    // table hands each a distinct inner id, so the host's duplicate fence never
    // fires. Without the rewrite the second call would answer CONFLICT.
    auto second = tcp(relay);
    auto same = second->request(frame(42, 2, 1, 3, 1, payload(4)));
    require(correlation_of(same) == 42 && !reply_of(same).is_error,
        "a second channel reusing the outer correlation was not OK");
    std::cout << "entity.transport.gateway multiplex: same outer id on two channels, no host conflict [PASS]\n";

    // Entity traffic on a correlated (MLC2) session carries a 4-byte transport
    // id before the MLE1 frame. The gateway must still rewrite the entity
    // correlation, so two MLC2 channels sharing one outer entity correlation
    // do not collide on the host's duplicate fence.
    const auto correlated = [&](const std::string& body) {
        std::string out(4, '\0');
        auto id = static_cast<std::uint32_t>(0x0A0B0C0D);
        for (int i = 3; i >= 0; --i) { out[i] = static_cast<char>(id & 255); id >>= 8; }
        return out + body;
    };
    const auto uncorrelate = [](const std::string& body) -> std::string {
        require(body.size() >= 4, "correlated reply frame too short");
        return body.substr(4);
    };
    {
        RawSocket c1(relay);
        const auto send_frame = [&](const std::string& body) {
            std::string header(4, '\0');
            const auto length = static_cast<std::uint32_t>(body.size());
            for (unsigned i = 0; i < 4; ++i) header[i] = static_cast<char>(length >> (24 - i*8));
            c1.send(header + body);
        };
        const auto read_frame = [&]() {
            std::string header = c1.receive(4);
            std::uint32_t length = 0;
            for (unsigned i = 0; i < 4; ++i) length = length << 8 | static_cast<unsigned char>(header[i]);
            return c1.receive(length);
        };
        send_frame("MLC2");
        equal(read_frame(), "MLC2 OK");
        send_frame(correlated(frame(42, 2, 1, 3, 1, payload(4))));
        auto answer = read_frame();
        require(answer.size() >= 4, "MLC2 entity reply too short");
        std::uint32_t tid = 0;
        for (unsigned i = 0; i < 4; ++i) tid = tid << 8 | static_cast<unsigned char>(answer[i]);
        require(tid == 0x0A0B0C0D, "MLC2 entity reply lost the transport id");
        require(correlation_of(uncorrelate(answer)) == 42 && !reply_of(uncorrelate(answer)).is_error,
            "MLC2 entity reply lost the outer correlation");
    }
    {
        RawSocket c2(relay);
        const auto send_frame = [&](const std::string& body) {
            std::string header(4, '\0');
            const auto length = static_cast<std::uint32_t>(body.size());
            for (unsigned i = 0; i < 4; ++i) header[i] = static_cast<char>(length >> (24 - i*8));
            c2.send(header + body);
        };
        const auto read_frame = [&]() {
            std::string header = c2.receive(4);
            std::uint32_t length = 0;
            for (unsigned i = 0; i < 4; ++i) length = length << 8 | static_cast<unsigned char>(header[i]);
            return c2.receive(length);
        };
        send_frame("MLC2");
        equal(read_frame(), "MLC2 OK");
        send_frame(correlated(frame(42, 2, 1, 3, 1, payload(4))));
        auto answer = read_frame();
        require(answer.size() >= 4, "MLC2 second entity reply too short");
        require(correlation_of(uncorrelate(answer)) == 42 && !reply_of(uncorrelate(answer)).is_error,
            "a second MLC2 channel reusing the outer correlation was not OK");
    }
    std::cout << "entity.transport.gateway correlated: MLC2 prefix survives, rewrite still applies [PASS]\n";

    // A marker alone is not an entity frame. The relay must not read the
    // correlation slice before checking the frame length, and the endpoint
    // answers the same typed malformed-envelope error as a direct request.
    {
        RawSocket raw(relay);
        const auto send_frame = [&](const std::string& body) {
            std::string header(4, '\0');
            const auto length = static_cast<std::uint32_t>(body.size());
            for (unsigned i = 0; i < 4; ++i) header[i] = static_cast<char>(length >> (24 - i*8));
            raw.send(header + body);
        };
        const auto read_frame = [&]() {
            const auto header = raw.receive(4);
            std::uint32_t length = 0;
            for (unsigned i = 0; i < 4; ++i) length = length << 8 | static_cast<unsigned char>(header[i]);
            return raw.receive(length);
        };
        send_frame("MLE1");
        auto direct_short = read_frame();
        require(direct_short.size() >= 12 && direct_short.starts_with("MLE1"),
            "short MLE1 request did not answer a typed malformed envelope");
        require(correlation_of(direct_short) == 0 && reply_of(direct_short).is_error
                && reply_of(direct_short).error == EntityError::INVALID
                && reply_of(direct_short).reason == "malformed envelope",
            "short MLE1 request did not answer malformed envelope");
        send_frame(correlated("MLE1"));
        auto prefixed_short = read_frame();
        const std::string expected_short = "echo:" + std::string("\x0a\x0b\x0c\x0d", 4) + "MLE1";
        require(prefixed_short == expected_short,
            "short correlated MLE1 request passed through without corruption");
    }
    std::cout << "entity.transport.gateway malformed: short marker frames answered without rewrite [PASS]\n";

    // A notify forwards unchanged (zero correlation has no reply) and the
    // follow-up frame is the only bytes the reader sees.
    {
        RawSocket raw(relay);
        const auto send_frame = [&](const std::string& body) {
            std::string header(4, '\0');
            const auto length = static_cast<std::uint32_t>(body.size());
            for (unsigned i = 0; i < 4; ++i) header[i] = static_cast<char>(length >> (24 - i*8));
            raw.send(header + body);
        };
        const auto read_frame = [&]() {
            std::string header = raw.receive(4);
            std::uint32_t length = 0;
            for (unsigned i = 0; i < 4; ++i) length = length << 8 | static_cast<unsigned char>(header[i]);
            return raw.receive(length);
        };
        send_frame(frame(0, 3, 1, 3, 1, payload(4)));
        send_frame("hi");
        equal(read_frame(), "echo:hi");
        std::cout << "entity.transport.gateway notify: silent, follow-up is the only reply [PASS]\n";
    }

    // Refusal wiring: a duplicate outer correlation on one channel is already
    // pending, so allocate refuses and the gateway answers CAPACITY to the
    // client with the outer id instead of forwarding a second request the host
    // would fence as CONFLICT. A silent upstream (a contractless script that
    // answers nothing) keeps the first slot unresolved. The full-table bound is
    // unit-tested in entity.gateway; this proves the refusal reaches the wire.
    const auto quiet = port(), bounded = port();
    write(c.path("quiet.lua"), "local service = {}\nfunction service.message(payload) end\nreturn service");
    c.node("gameServer", quiet, {"--script", c.path("quiet.lua").string()});
    c.node("gateway", bounded, {"--upstream", endpoint(quiet)});
    {
        RawSocket hold(bounded);
        const auto send_frame = [&](const std::string& body) {
            std::string header(4, '\0');
            const auto length = static_cast<std::uint32_t>(body.size());
            for (unsigned i = 0; i < 4; ++i) header[i] = static_cast<char>(length >> (24 - i*8));
            hold.send(header + body);
        };
        const auto read_frame = [&]() {
            std::string header = hold.receive(4);
            std::uint32_t length = 0;
            for (unsigned i = 0; i < 4; ++i) length = length << 8 | static_cast<unsigned char>(header[i]);
            return hold.receive(length);
        };
        send_frame(frame(77, 2, 1, 3, 1, payload(4)));
        send_frame(frame(77, 2, 1, 3, 1, payload(4)));
        const auto refusal = read_frame();
        require(correlation_of(refusal) == 77 && reply_of(refusal).is_error
                && reply_of(refusal).error == EntityError::CAPACITY,
            "a duplicate outer correlation did not answer CAPACITY with the outer id");
        std::cout << "entity.transport.gateway capacity: duplicate refuses without forwarding [PASS]\n";
    }
}


void transport_tcp() {
    Context c;
    const auto script = c.path("echo.lua");
    write(script, echo);
    const auto gameServer=port(), forwarder=port(), gateway=port(), metrics=port();
    c.node("gameServer", gameServer, {"--script", script.string(), "--metrics-listen", endpoint(metrics)});
    c.node("gateway", forwarder, {"--upstream", endpoint(gameServer)});
    c.node("gateway", gateway, {"--upstream", endpoint(forwarder)});
    auto client = tcp(gateway);
    for (const auto& payload : std::vector<std::string>{{}, "hello", std::string("\0\xff", 2), "中文", std::string(1024*1024, 'x')})
        equal(client->request(payload), payload);
    for (int i=0; i<100; ++i) equal(client->request("stack"), "stack");
    auto samples = http("http://"+endpoint(metrics)+"/metrics");
    require(samples.status == 200 && samples.body.find("mistlane_ready{role=\"gameServer\"} 1") != std::string::npos,
        "Prometheus readiness missing");
    require(samples.body.find("mistlane_lua_callback_seconds_count") != std::string::npos, "callback histogram missing");
    const auto metric_value = [](const std::string& body, const std::string& sample) {
        const auto at = body.find(sample);
        require(at != std::string::npos, "missing metric " + sample);
        return std::strtod(body.c_str() + at + sample.size(), nullptr);
    };
    require(metric_value(samples.body, "mistlane_messages_total{role=\"gameServer\",direction=\"rx\"}") > 0,
        "rx message counter missing");
    require(metric_value(samples.body, "mistlane_messages_total{role=\"gameServer\",direction=\"tx\"}") > 0,
        "tx message counter missing");
    require(metric_value(samples.body, "mistlane_payload_bytes_total{role=\"gameServer\",direction=\"tx\"}") > 1024*1024,
        "payload byte counter missing");
    require(metric_value(samples.body, "mistlane_request_seconds_count{role=\"gameServer\"}") > 0,
        "request latency histogram missing");
    require(metric_value(samples.body, "mistlane_request_seconds_sum{role=\"gameServer\"}") > 0,
        "request latency not observed");
    // The first scheduled tick fires at least 100ms after start; wait for it.
    Http ticks;
    eventually([&] {
        ticks = http("http://"+endpoint(metrics)+"/metrics");
        return metric_value(ticks.body, "mistlane_event_loop_lag_seconds_count{role=\"gameServer\"}") > 0;
    });
    require(metric_value(ticks.body, "mistlane_event_loop_lag_max_seconds{role=\"gameServer\"}") > 0,
        "tick lag maximum missing");
    require(metric_value(samples.body, "mistlane_upstream_errors_total{role=\"gameServer\"}") == 0,
        "unexpected upstream errors");
    require(metric_value(samples.body, "mistlane_upstream_timeouts_total{role=\"gameServer\"}") == 0,
        "unexpected upstream timeouts");
    const std::string frame("\0\0\0\5abcde", 9);
    {
        RawSocket raw(gateway);
        for (char ch : frame) raw.send(std::string_view(&ch, 1));
        equal(raw.receive(frame.size()), frame);
        raw.send(frame+frame);
        equal(raw.receive(2*frame.size()), frame+frame);
        raw.send(std::string("\0\x10\0\1", 4));
        require(raw.closed(), "oversize frame was accepted");
    }
    std::vector<std::future<void>> tasks;
    for (int i=0; i<8; ++i) tasks.push_back(std::async(std::launch::async, [=] {
        auto peer = tcp(gateway); equal(peer->request("parallel"), "parallel");
    }));
    for (auto& task : tasks) task.get();
    const auto limited = port();
    c.node("gameServer", limited, {"--script", script.string(), "--max-connections", "1"});
    auto first = tcp(limited); equal(first->request("one"), "one");
    RawSocket extra(limited);
    require(extra.closed(), "connection admission was not enforced");
    equal(first->request("still-alive"), "still-alive");
    const auto dead = port(), ingress = port(), gateway_metrics = port();
    c.node("gateway", ingress, {"--upstream", endpoint(dead), "--metrics-listen", endpoint(gateway_metrics)});
    must_fail([&] { (void)tcp(ingress)->request("unavailable"); });
    auto failure = http("http://"+endpoint(gateway_metrics)+"/metrics");
    require(metric_value(failure.body, "mistlane_upstream_errors_total{role=\"gateway\"}") >= 1,
        "upstream connect failure not counted");
}
void transport_websocket() {
    Context c; c.cert(); write(c.path("echo.lua"), echo);
    const auto gameServer = port(), forwarder = port();
    c.node("gameServer", gameServer, {"--script", c.path("echo.lua").string()});
    c.node("gateway", forwarder, {"--upstream", endpoint(gameServer)});
    for (const auto& transport : {"ws", "wss"}) {
        const bool secure = std::string_view(transport) == "wss";
        for (const auto& role : {"gateway", "gameServer"}) {
            const auto listen = port();
            std::vector<std::string> args{"--transport", transport};
            if (secure) { auto cert = tls(c); args.insert(args.end(), cert.begin(), cert.end()); }
            if (std::string_view(role) == "gateway") args.insert(args.end(), {"--upstream", endpoint(forwarder)});
            else args.insert(args.end(), {"--script", c.path("echo.lua").string()});
            c.node(role, listen, args);
            for (const auto& path : {"/", "/ws"}) {
                cli::ClientOptions options{std::string(transport)+"://localhost:"+std::to_string(listen)+path,
                    secure ? c.path("cert.pem").string() : ""};
                auto client = cli::make_client(options);
                for (const auto& data : std::vector<std::string>{{}, "中文", std::string("\0\xff", 2), std::string(65536, 'w')})
                    equal(client->request(data), data);
            }
            if (secure) {
                // Certificate trust and hostname verification must stay enabled.
                must_fail([&] { auto peer = cli::make_client({"wss://localhost:"+std::to_string(listen)+"/ws", {}, 500}); });
                must_fail([&] { auto peer = cli::make_client({"wss://"+endpoint(listen)+"/ws", c.path("cert.pem").string(), 500}); });
            }
        }
    }
}
void transport_http() {
    Context c; c.cert(); write(c.path("echo.lua"), echo);
    const auto gameServer = port();
    c.node("gameServer", gameServer, {"--script", c.path("echo.lua").string()});
    for (const auto& transport : {"http", "https"}) {
        const bool secure = std::string_view(transport) == "https";
        const auto listen = port();
        std::vector<std::string> args{"--transport", transport, "--upstream", endpoint(gameServer)};
        if (secure) { auto cert = tls(c); args.insert(args.end(), cert.begin(), cert.end()); }
        c.node("gateway", listen, args);
        const auto origin = std::string(transport)+"://localhost:"+std::to_string(listen);
        HttpRequest request{origin+"/message", "POST", std::string("\0\xffhello", 7),
            secure ? c.path("cert.pem").string() : ""};
        const auto result = http_request(request);
        require(result.status == 200, "HTTP adapter status"); equal(result.body, request.body);
        require(http(origin+"/missing", "GET", {}, request.ca_file).status == 404, "unknown HTTP route");
        request.max_response_bytes = 2;
        must_fail([&] { (void)http_request(request); });
        if (secure) {
            request.max_response_bytes = 1024; request.ca_file.clear();
            must_fail([&] { (void)http_request(request); });
        }
    }
    must_fail([] { (void)http_request({"file:///etc/hosts"}); });
}

// MLA1 rides the node's HTTP listener: a native /mla1 intercept beside the Lua
// routes, enabled by credential environment variables only.
void transport_listeners() {
    Context c; c.cert();
    write(c.path("echo.lua"), correlate_lua);
    write(c.path("ws.lua"), websocket_lua);
    // One process, three listeners: framed tcp plus ws and wss with a
    // per-listener certificate override. The ws listeners stay framed game
    // sessions (Lua message), exactly like single-transport runs.
    const auto plain = port(), ws_port = port(), wss_port = port();
    auto& mixed = c.spawn({self().string(), "--role", "gameServer", "--listen", endpoint(plain),
        "--script", c.path("echo.lua").string(),
        "--listener", "ws://127.0.0.1:"+std::to_string(ws_port),
        "--listener", "wss://127.0.0.1:"+std::to_string(wss_port)+"?cert="+c.path("cert.pem").string()+"&key="+c.path("key.pem").string()});
    mixed.ready("ready gameServer 127.0.0.1:"+std::to_string(wss_port));
    equal(tcp(plain)->request("plain"), "plain");
    equal(cli::make_client({"ws://localhost:"+std::to_string(ws_port)+"/ws"})->request("session"), "session");
    equal(cli::make_client({"wss://localhost:"+std::to_string(wss_port)+"/ws", c.path("cert.pem").string()})->request("secure"), "secure");

    // apiServer: one http listener serving the Lua HTTP service and websocket
    // lifecycle side by side. Plain requests stay with Lua; upgrades upgrade.
    const auto api = port();
    auto& apiServer = c.node("apiServer", api, {"--transport", "http", "--script", c.path("ws.lua").string()});
    const auto origin = "http://"+endpoint(api);
    require(http(origin+"/ws").status == 404, "plain HTTP on the ws path must reach the Lua service");
    require(http(origin+"/missing").status == 404, "Lua HTTP routing broken");
    std::string lifecycle_id;
    {
        WsRaw ws(api);
        ws.text("hello");
        equal(ws.read_text(), "echo:hello");
        ws.text("state");
        const auto state = ws.read_text();
        require(state.starts_with("open=1 closed=0 "), "websocket registry state wrong: "+state);
        lifecycle_id = state.substr(state.rfind('=')+1);
        // A lifecycle message that sends nothing must not wedge the bridge.
        ws.text("noreply");
        ws.text("state");
        require(ws.read_text().starts_with("open=1 closed=0 "), "reply-less message broke the bridge");
        ws.text("oversize");
        equal(ws.read_text(), "guarded");
        ws.text("unknown");
        equal(ws.read_text(), "guarded");
        const auto response = http(origin+"/echo", "POST", "via-lua");
        require(response.status == 201, "HTTP service must share the port with websocket upgrades");
        equal(response.body, "via-lua");
        equal(http(origin+"/state").body, "open=1 closed=0");
    }
    // Clean disconnect: close hook runs, registry bookkeeping updates, same id.
    eventually([&] { return apiServer.output().find("WSHOOK close "+lifecycle_id) != std::string::npos; },
        5s, "websocket close hook did not run");
    eventually([&] { return http(origin+"/state").body == "open=1 closed=1"; }, 5s, "websocket session leak");
    // A Lua failure inside the message callback closes that session only.
    {
        WsRaw ws(api);
        ws.text("boom");
        eventually([&] { return apiServer.output().find("websocket message failed") != std::string::npos; },
            5s, "lua failure in websocket message not logged");
    }
    require(http(origin+"/state").status == 200, "HTTP service must survive a failed websocket session");
    equal(cli::make_client({"ws://localhost:"+std::to_string(api)+"/ws"})->request("still"), "echo:still");
    // Exit strategy: SIGTERM with a live websocket session still exits cleanly.
    {
        WsRaw ws(api);
        ws.text("hello");
        equal(ws.read_text(), "echo:hello");
        apiServer.signal(SIGTERM);
        require(apiServer.wait() == 0, "shutdown with live websocket sessions failed");
    }
    // Slow consumer: pushes past the send bound must close the runaway session
    // while the same process keeps serving fresh ones.
    const auto slow = port();
    auto& lagging = c.node("apiServer", slow, {"--transport", "http", "--script", c.path("ws.lua").string()});
    {
        WsRaw ws(slow);
        ws.text(flood_pushes());
        std::this_thread::sleep_for(500ms); // pushes queue while nobody reads
        const auto drained = ws.drain_until_closed(20s);
        require(drained > 0, "server closed before any flood data reached the client");
        equal(cli::make_client({"ws://localhost:"+std::to_string(slow)+"/ws"})->request("alive"), "echo:alive");
    }
    // HTTPS and WSS on one process: the https primary uses the global cert,
    // the wss listener carries its own via the query override.
    const auto tls_port = port(), wss_extra = port();
    {
        auto cert = tls(c);
        std::vector<std::string> args{"--transport", "https", "--script", c.path("ws.lua").string(),
            "--listener", "wss://127.0.0.1:"+std::to_string(wss_extra)+"?cert="+c.path("cert.pem").string()+"&key="+c.path("key.pem").string()};
        args.insert(args.end(), cert.begin(), cert.end());
        c.node("apiServer", tls_port, args);
    }
    require(http("https://localhost:"+std::to_string(tls_port)+"/state", "GET", {}, c.path("cert.pem").string()).status == 200,
        "https listener not serving the Lua service");
    // The wss listener stays a framed game session: message echo, not lifecycle.
    equal(cli::make_client({"wss://localhost:"+std::to_string(wss_extra)+"/ws", c.path("cert.pem").string()})->request("tls"), "tls");
    // Gateway: framed tcp listener beside an http listener, both forwarding.
    const auto gate = port(), gate_http = port();
    c.node("gateway", gate, {"--upstream", endpoint(plain), "--listener", "http://127.0.0.1:"+std::to_string(gate_http)});
    equal(tcp(gate)->request("tcp-via-gateway"), "tcp-via-gateway");
    const auto forwarded = http("http://"+endpoint(gate_http)+"/message", "POST", std::string("\0\xff", 2));
    require(forwarded.status == 200, "gateway /message forwarding");
    equal(forwarded.body, std::string("\0\xff", 2));
    equal(cli::make_client({"ws://localhost:"+std::to_string(gate_http)+"/ws"})->request("ws-via-gateway"), "ws-via-gateway");
    // Config files carry LISTENERS like any other key.
    const auto cfg_plain = port(), cfg_ws = port();
    write(c.path("node.conf"), "# mistlane-config 1\nROLE=gameServer\nLISTEN=127.0.0.1:"+std::to_string(cfg_plain)+
        "\nSCRIPT="+c.path("echo.lua").string()+"\nLISTENERS=ws://127.0.0.1:"+std::to_string(cfg_ws)+"\n");
    auto& configured = c.spawn({self().string(), "--config", c.path("node.conf").string()});
    configured.ready("ready gameServer 127.0.0.1:"+std::to_string(cfg_ws));
    equal(tcp(cfg_plain)->request("config"), "config");
    equal(cli::make_client({"ws://localhost:"+std::to_string(cfg_ws)+"/ws"})->request("config-ws"), "config-ws");
    // Conflicting listener sets must fail at startup, never half-serve.
    must_fail([&] { c.node("apiServer", port(), {"--transport", "http", "--script", c.path("ws.lua").string(),
        "--listener", "tcp://127.0.0.1:"+std::to_string(port())}); });
    must_fail([&] { const auto reused = port();
        c.node("gameServer", port(), {"--script", c.path("echo.lua").string(),
            "--listener", "tcp://127.0.0.1:"+std::to_string(reused),
            "--listener", "ws://127.0.0.1:"+std::to_string(reused)}); });
}
// Framed game sessions carry a session identity and accept Lua pushes:
// mistlane.session.send/broadcast deliver frames without a matching request,
// a handler may answer nothing, and outbound pushes stay bounded per session.
// Optional correlation mode: the first frame "MLC2" switches the session to
// u32 BE request ids carried inside the frame payload; pushes carry id 0.
void transport_correlate() {
    Context c;
    write(c.path("correlate.lua"), correlate_lua);
    const auto gameServer = port(), gateway = port(), ws_port = port();
    auto& node = c.spawn({self().string(), "--role", "gameServer", "--listen", endpoint(gameServer),
        "--script", c.path("correlate.lua").string(),
        "--listener", "ws://127.0.0.1:"+std::to_string(ws_port)});
    node.ready("ready gameServer "+endpoint(gameServer));
    c.node("gateway", gateway, {"--upstream", endpoint(gameServer)});
    const auto send_frame = [](RawSocket& raw, std::string_view payload) {
        std::string frame(4, '\0');
        auto length = static_cast<std::uint32_t>(payload.size());
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(length & 255); length >>= 8; }
        raw.send(frame+std::string(payload));
    };
    const auto read_frame = [](RawSocket& raw) {
        const auto header = raw.receive(4);
        std::size_t length = 0;
        for (unsigned char ch : header) length = (length << 8) | ch;
        return raw.receive(length);
    };
    const auto tagged = [](std::uint32_t id, std::string_view payload) {
        std::string frame(4, '\0');
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(id & 255); id >>= 8; }
        return frame+std::string(payload);
    };
    std::uint32_t id = 0;
    std::string body;
    const auto split = [&id, &body](const std::string& frame) {
        require(frame.size() >= 4, "correlated frame too short");
        id = 0;
        for (int i = 0; i < 4; ++i) id = (id<<8) | static_cast<unsigned char>(frame[i]);
        body = frame.substr(4);
    };
    // Handshake, then replies echo the request id byte for byte.
    RawSocket first(gameServer);
    send_frame(first, "MLC2");
    equal(read_frame(first), "MLC2 OK");
    send_frame(first, tagged(0x01020304, "hello"));
    split(read_frame(first));
    require(id == 0x01020304 && body == "hello", "correlated echo lost id or payload");
    send_frame(first, tagged(9, "self"));
    split(read_frame(first));
    require(id == 9 && body.starts_with("session=") && body.ends_with(":req=9"),
        "lua request id missing: "+body);
    // A nil reply still means no frame; pushes carry id 0 and precede the reply.
    send_frame(first, tagged(5, "silent"));
    std::this_thread::sleep_for(300ms);
    require(!first.pending(), "nil reply produced a frame");
    send_frame(first, tagged(6, "push"));
    split(read_frame(first));
    require(id == 0 && body == "pushed", "push must stay uncorrelated");
    split(read_frame(first));
    require(id == 6 && body == "done", "reply after push lost its id");
    // An oversized correlated push reports instead of failing the handler.
    send_frame(first, tagged(7, "big"));
    split(read_frame(first));
    require(id == 7 && body == "big=false:payload exceeds 1 MiB", "oversize push not reported: "+body);
    // A plain client on the same server stays uncorrelated; broadcast shapes differ.
    RawSocket second(gameServer);
    send_frame(second, "hello");
    equal(read_frame(second), "hello");
    send_frame(second, "bcast");
    split(read_frame(first));
    require(id == 0 && body == "bc", "broadcast to a correlated session must carry id 0");
    equal(read_frame(second), "bc");
    equal(read_frame(second), "bcast=2");
    // The handshake is honored as the first frame only.
    send_frame(second, "MLC2");
    equal(read_frame(second), "MLC2");
    // The same contract through a gateway: ids survive the relay.
    RawSocket via(gateway);
    send_frame(via, "MLC2");
    equal(read_frame(via), "MLC2 OK");
    send_frame(via, tagged(0x0A0B0C0D, "ping"));
    split(read_frame(via));
    require(id == 0x0A0B0C0D && body == "ping", "gateway lost correlation");
    send_frame(via, tagged(3, "self"));
    split(read_frame(via));
    require(id == 3 && body.ends_with(":req=3"), "gateway lua request id missing: "+body);
    // The ws game-session leg speaks the same mode in binary frames.
    {
        WsRaw over(ws_port);
        over.text("MLC2");
        equal(over.read_text(), "MLC2 OK");
        over.bin(tagged(11, "w"));
        split(over.read_text());
        require(id == 11 && body == "w", "ws correlated echo failed");
    }
    // Cross routing keeps its own handshake and rejects correlation.
    const auto shard = port(), cross_gate = port();
    c.node("crossServer", shard, {"--script", c.path("correlate.lua").string()});
    c.cross_gateway(cross_gate, {"--upstream", endpoint(shard)});
    {
        RawSocket crossed(cross_gate);
        send_frame(crossed, "MLC2");
        equal(read_frame(crossed), "MLC1 ERR INVALID");
    }
    require(node.output().find("callback failed") == std::string::npos, "correlation path logged Lua failures");
}
// MLS1 identifies internal legs: a node with --service-token only serves
// peers that present "MLS1 <role> <instance> <token>", a gateway with
// --upstream-token presents itself before any business frame, and every
// refusal closes the connection with a reason instead of leaking frames into
// the script.
void transport_identity() {
    Context c;
    write(c.path("correlate.lua"), correlate_lua);
    const auto gameServer = port(), open_server = port(), gateway = port(),
              no_token = port(), wrong_token = port(), mismatched = port();
    auto& node = c.spawn({self().string(), "--role", "gameServer", "--listen", endpoint(gameServer),
        "--script", c.path("correlate.lua").string(), "--service-token", "s3cret",
        "--service-name", "game-1"});
    auto& open_node = c.spawn({self().string(), "--role", "gameServer", "--listen", endpoint(open_server),
        "--script", c.path("correlate.lua").string()});
    node.ready("ready gameServer " + endpoint(gameServer));
    open_node.ready("ready gameServer " + endpoint(open_server));
    c.node("gateway", gateway, {"--upstream", endpoint(gameServer),
        "--upstream-token", "s3cret", "--service-name", "gwy-1"});
    c.node("gateway", no_token, {"--upstream", endpoint(gameServer)});
    c.node("gateway", wrong_token, {"--upstream", endpoint(gameServer), "--upstream-token", "nope"});
    c.node("gateway", mismatched, {"--upstream", endpoint(open_server), "--upstream-token", "s3cret"});
    const auto send_frame = [](RawSocket& raw, std::string_view payload) {
        std::string frame(4, '\0');
        auto length = static_cast<std::uint32_t>(payload.size());
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(length & 255); length >>= 8; }
        raw.send(frame+std::string(payload));
    };
    const auto read_frame = [](RawSocket& raw) {
        const auto header = raw.receive(4);
        std::size_t length = 0;
        for (unsigned char ch : header) length = (length << 8) | ch;
        return raw.receive(length);
    };
    const auto tagged = [](std::uint32_t id, std::string_view payload) {
        std::string frame(4, '\0');
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(id & 255); id >>= 8; }
        return frame+std::string(payload);
    };
    std::uint32_t id = 0;
    std::string body;
    const auto split = [&id, &body](const std::string& frame) {
        require(frame.size() >= 4, "correlated frame too short");
        id = 0;
        for (int i = 0; i < 4; ++i) id = (id<<8) | static_cast<unsigned char>(frame[i]);
        body = frame.substr(4);
    };
    const auto expect_close = [](RawSocket& raw) {
        for (int i = 0; i < 40 && !raw.closed(); ++i) std::this_thread::sleep_for(50ms);
        require(raw.closed(), "connection stayed open after a refusal");
    };
    // An unauthenticated first frame never reaches the script.
    RawSocket anonymous(gameServer);
    send_frame(anonymous, "self");
    equal(read_frame(anonymous), "MLS1 ERR TOKEN");
    expect_close(anonymous);
    // Malformed handshakes report the shape, not the secret.
    for (const auto& hello : {"MLS1 onlyrole", "MLS1 a b c d"}) {
        RawSocket malformed(gameServer);
        send_frame(malformed, hello);
        equal(read_frame(malformed), "MLS1 ERR INVALID");
        expect_close(malformed);
    }
    RawSocket refused(gameServer);
    send_frame(refused, "MLS1 gateway inst-9 nope");
    equal(read_frame(refused), "MLS1 ERR TOKEN");
    expect_close(refused);
    // Correct credentials admit the session; the handshake stays invisible after.
    RawSocket joined(gameServer);
    send_frame(joined, "MLS1 gateway inst-9 s3cret");
    equal(read_frame(joined), "MLS1 OK");
    send_frame(joined, "self");
    body = read_frame(joined);
    require(body.starts_with("session=") && body.ends_with(":req=nil"), "handshaken session broken: "+body);
    // MLC2 stacks on the accepted handshake: both close at the first payload.
    RawSocket correlated(gameServer);
    send_frame(correlated, "MLS1 gateway inst-9 s3cret");
    equal(read_frame(correlated), "MLS1 OK");
    send_frame(correlated, "MLC2");
    equal(read_frame(correlated), "MLC2 OK");
    send_frame(correlated, tagged(9, "self"));
    split(read_frame(correlated));
    require(id == 9 && body.ends_with(":req=9"), "correlation lost after the handshake: "+body);
    // A token-bearing gateway shows no handshake at either end.
    RawSocket via(gateway);
    send_frame(via, "MLC2");
    equal(read_frame(via), "MLC2 OK");
    send_frame(via, tagged(3, "ping"));
    split(read_frame(via));
    require(id == 3 && body == "ping", "gateway handshake leaked into the relay: "+body);
    RawSocket plain_via(gateway);
    send_frame(plain_via, "self");
    body = read_frame(plain_via);
    require(body.starts_with("session=") && body.ends_with(":req=nil"), "gateway session broke: "+body);
    // An upstream refusal ends the session without relaying the verdict.
    RawSocket silent(wrong_token);
    send_frame(silent, "ping");
    std::this_thread::sleep_for(300ms);
    require(!silent.pending(), "upstream verdict leaked to the client");
    expect_close(silent);
    // A gateway without a token cannot hide the mismatch, so it relays the reason.
    RawSocket relayed(no_token);
    send_frame(relayed, "ping");
    equal(read_frame(relayed), "MLS1 ERR TOKEN");
    expect_close(relayed);
    // A server that needs no handshake refuses the stray one.
    RawSocket to_plain(open_server);
    send_frame(to_plain, "MLS1 gateway inst-9 s3cret");
    equal(read_frame(to_plain), "MLS1 ERR UNEXPECTED");
    expect_close(to_plain);
    RawSocket via_open(mismatched);
    send_frame(via_open, "ping");
    std::this_thread::sleep_for(300ms);
    require(!via_open.pending(), "server refusal leaked to the client");
    expect_close(via_open);
    // ...and keeps serving anonymous clients.
    RawSocket open_client(open_server);
    send_frame(open_client, "hello");
    equal(read_frame(open_client), "hello");
    // Cross routing keeps its own identity, so the token flags refuse to start.
    const auto cross_arg = std::vector<std::string>{"--routing", "cross"};
    for (const auto& extra : {std::vector<std::string>{"--service-token", "s3cret"},
                              std::vector<std::string>{"--upstream-token", "s3cret"}}) {
        auto args = std::vector<std::string>{self().string(), "--role", "gateway",
            "--listen", endpoint(port()), "--upstream", endpoint(gameServer)};
        args.insert(args.end(), cross_arg.begin(), cross_arg.end());
        args.insert(args.end(), extra.begin(), extra.end());
        auto& child = c.spawn(args);
        require(child.wait() != 0 && child.output().find("does not apply to cross routing") != std::string::npos,
                "token flags accepted on cross routing");
    }
    for (const auto& node_output : {node.output(), open_node.output()})
        require(node_output.find("callback failed") == std::string::npos, "handshake path logged Lua failures");
}
// The gateway --upstream-multiplex mode (MLM1) shares one upstream connection
// across client sessions: logical sessions stay isolated, pushes and broadcast
// address a single session, a Lua error or disconnect closes only its own
// logical session, and plain clients are told off instead of upgrading.
void transport_multiplex() {
    Context c;
    write(c.path("correlate.lua"), correlate_lua);
    const auto gameServer = port(), metrics = port(), gateway = port();
    auto& node = c.spawn({self().string(), "--role", "gameServer", "--listen", endpoint(gameServer),
        "--script", c.path("correlate.lua").string(), "--service-token", "s3cret",
        "--metrics-listen", endpoint(metrics)});
    // Gateway starts its shared MuxLink on the first client. Wait until the
    // upstream listener has announced readiness before allowing that connect.
    node.ready("ready gameServer");
    c.node("gateway", gateway, {"--upstream", endpoint(gameServer),
        "--upstream-token", "s3cret", "--upstream-multiplex", "1"});
    std::string phase = "initial client echo";
    const auto send_frame = [](RawSocket& raw, std::string_view payload) {
        std::string frame(4, '\0');
        auto length = static_cast<std::uint32_t>(payload.size());
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(length & 255); length >>= 8; }
        raw.send(frame+std::string(payload));
    };
    const auto read_frame = [&phase](RawSocket& raw) {
        try {
            const auto header = raw.receive(4);
            std::size_t length = 0;
            for (unsigned char ch : header) length = (length << 8) | ch;
            return raw.receive(length);
        } catch (const std::exception& error) {
            throw std::runtime_error("multiplex phase '"+phase+"': "+error.what());
        }
    };
    const auto tagged = [](std::uint32_t id, std::string_view payload) {
        std::string frame(4, '\0');
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(id & 255); id >>= 8; }
        return frame+std::string(payload);
    };
    std::uint32_t id = 0;
    std::string body;
    const auto split = [&id, &body](const std::string& frame) {
        require(frame.size() >= 4, "correlated frame too short");
        id = 0;
        for (int i = 0; i < 4; ++i) id = (id<<8) | static_cast<unsigned char>(frame[i]);
        body = frame.substr(4);
    };
    const auto expect_close = [](RawSocket& raw) {
        for (int i = 0; i < 40 && !raw.closed(); ++i) std::this_thread::sleep_for(50ms);
        require(raw.closed(), "connection stayed open after a refusal");
    };
    const auto established = [](unsigned port) {
        char hex[8];
        std::snprintf(hex, sizeof hex, "%04X", port);
        std::ifstream net("/proc/net/tcp");
        std::string sl, local, remote, state, rest;
        int count = 0;
        while (net >> sl >> local >> remote >> state) {
            if (state == "01" && local.ends_with(":"+std::string(hex))) ++count;
            std::getline(net, rest);
        }
        return count;
    };
    // Two clients ride one upstream connection; the server still counts both.
    RawSocket a(gateway), b(gateway);
    send_frame(a, "hello"); equal(read_frame(a), "hello");
    send_frame(b, "world"); equal(read_frame(b), "world");
    // Synchronize on server admission and the kernel socket state. The fixed
    // sleep used to sample /proc/net/tcp before the shared connection was
    // visible under load.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    int upstream_connections = 0;
    std::string live;
    do {
        live = http("http://"+endpoint(metrics)+"/metrics").body;
        upstream_connections = established(gameServer);
        if (upstream_connections == 1 &&
            live.find("mistlane_active_sessions{role=\"gameServer\"} 2") != std::string::npos) break;
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    require(upstream_connections == 1, "expected one shared upstream TCP connection; observed " + std::to_string(upstream_connections));
    require(live.find("mistlane_active_sessions{role=\"gameServer\"} 2") != std::string::npos,
            "logical sessions missing from server admission");
    // Correlation is a first-frame handshake, so it lives on a third client and
    // must survive the envelope of a session it never shared.
    RawSocket corr(gateway);
    phase = "correlation handshake";
    send_frame(corr, "MLC2"); equal(read_frame(corr), "MLC2 OK");
    send_frame(corr, tagged(5, "self"));
    split(read_frame(corr));
    require(id == 5 && body.ends_with(":req=5"), "correlation lost through the shared leg: "+body);
    send_frame(b, "self");
    body = read_frame(b);
    require(body.starts_with("session=") && body.ends_with(":req=nil"), "plain session broken: "+body);
    // A push and a broadcast stay inside their logical session.
    phase = "push and correlated reply";
    send_frame(corr, tagged(6, "push"));
    split(read_frame(corr)); require(id == 0 && body == "pushed", "push lost on the shared leg");
    split(read_frame(corr)); require(id == 6 && body == "done", "reply lost after a push");
    require(!b.pending(), "push leaked into another logical session");
    // A directly authenticated session shares the registry too, so the count
    // reaches four: a, b, corr and the direct peer.
    phase = "direct peer and broadcast";
    RawSocket direct(gameServer);
    send_frame(direct, "MLS1 gateway inst-9 s3cret");
    equal(read_frame(direct), "MLS1 OK");
    send_frame(direct, "hi"); equal(read_frame(direct), "hi");
    send_frame(b, "bcast");
    split(read_frame(corr)); require(id == 0 && body == "bc", "broadcast shape lost on the correlated session");
    equal(read_frame(a), "bc");
    equal(read_frame(b), "bc"); equal(read_frame(b), "bcast=4");
    equal(read_frame(direct), "bc");
    // A Lua error closes only that logical session.
    {
        phase = "isolated Lua error";
        RawSocket err(gateway);
        send_frame(err, "oops");
        expect_close(err);
    }
    require(node.output().find("mux oops") != std::string::npos,
            "logical session error not reported");
    phase = "survivor after Lua error";
    send_frame(b, "again"); equal(read_frame(b), "again");
    // A disconnect retires its logical session; the rest of the hub survives.
    phase = "disconnect retirement";
    {
        RawSocket gone(gateway);
        send_frame(gone, "bye"); equal(read_frame(gone), "bye");
        send_frame(b, "bcast");
        equal(read_frame(b), "bc"); equal(read_frame(b), "bcast=5");
        equal(read_frame(a), "bc"); // the survivors read the broadcast twice
    }
    std::this_thread::sleep_for(300ms);
    send_frame(b, "bcast");
    equal(read_frame(b), "bc"); equal(read_frame(b), "bcast=4");
    equal(read_frame(a), "bc");
    // Nobody upgrades mid-session, whether through the gateway or directly.
    // Check for malformed hub traffic before the deliberate MLM1 refusals
    // below: those expected refusals are logged as "unexpected frame" too.
    require(node.output().find("unexpected frame") == std::string::npos,
            "hub parsed a malformed frame before the refusal cases");
    phase = "mid-session refusal probes";
    send_frame(a, "MLM1"); equal(read_frame(a), "MLM1");
    {
        RawSocket mid(gateway);
        send_frame(mid, "hi"); equal(read_frame(mid), "hi");
        send_frame(mid, "MLM1"); equal(read_frame(mid), "MLM1");
    }
    RawSocket plain(gameServer);
    send_frame(plain, "MLM1");
    equal(read_frame(plain), "MLM1 ERR UNEXPECTED");
    expect_close(plain);
    {
        RawSocket through(gateway);
        send_frame(through, "MLM1");
        equal(read_frame(through), "MLM1 ERR UNEXPECTED");
        expect_close(through);
    }
    // Multiplexing is an internal mode, so it needs the identity handshake.
    {
        auto& bad = c.spawn({self().string(), "--role", "gateway", "--listen", endpoint(port()),
                             "--upstream", endpoint(gameServer), "--upstream-multiplex", "1"});
        require(bad.wait() != 0 && bad.output().find("requires --upstream-token") != std::string::npos,
                "multiplex accepted without a service identity");
    }
}
// The coordinator role (MLR1) keeps a live registry instead of forwarding
// gameplay: members register with a scope and heartbeat readiness/load, a
// registry client lists or places against it, disconnects drop entries, and a
// token-requiring coordinator admits only identified members.
void transport_push() {
    Context c;
    write(c.path("push.lua"), push_lua);
    const auto gameServer = port(), gateway = port(), ws_port = port();
    auto& node = c.spawn({self().string(), "--role", "gameServer", "--listen", endpoint(gameServer),
        "--script", c.path("push.lua").string(),
        "--listener", "ws://127.0.0.1:"+std::to_string(ws_port)});
    node.ready("ready gameServer " + endpoint(gameServer));
    c.node("gateway", gateway, {"--upstream", endpoint(gameServer)});
    const auto send_frame = [](RawSocket& raw, std::string_view payload) {
        std::string frame(4, '\0');
        auto length = static_cast<std::uint32_t>(payload.size());
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(length & 255); length >>= 8; }
        raw.send(frame+std::string(payload));
    };
    const auto read_frame = [](RawSocket& raw) {
        const auto header = raw.receive(4);
        std::size_t length = 0;
        for (unsigned char ch : header) length = (length << 8) | ch;
        return raw.receive(length);
    };
    RawSocket first(gameServer);
    send_frame(first, "self");
    equal(read_frame(first), "session=1");
    // A handler may answer nothing; the session must stay usable afterwards.
    send_frame(first, "silent");
    std::this_thread::sleep_for(300ms);
    require(!first.pending(), "nil handler reply produced a frame");
    send_frame(first, "self");
    equal(read_frame(first), "session=1");
    // Lua push reaches the client without a request; the reply follows the pushes.
    send_frame(first, "push");
    equal(read_frame(first), "pushed");
    equal(read_frame(first), "done");
    // Broadcast reaches every session; each client sees the initiator's id.
    RawSocket second(gameServer);
    send_frame(second, "self");
    equal(read_frame(second), "session=2");
    send_frame(first, "bcast");
    equal(read_frame(second), "bc:1");
    equal(read_frame(first), "bc:1");
    equal(read_frame(first), "bcast=2");
    // Push bounds: per-frame cap, unknown ids and the backlog report, not close.
    send_frame(first, "burst 64");
    std::string burst;
    std::size_t pushed = 0;
    for (;;) {
        burst = read_frame(first);
        if (burst.starts_with("burst=")) break;
        require(burst.size() == 64*1024 && burst.find_first_not_of('B') == std::string::npos,
            "pushed frame corrupted");
        ++pushed;
        require(pushed <= 64, "more push frames queued than attempts");
    }
    const auto colon = burst.find(':');
    const auto queued = std::stoul(burst.substr(6, colon-6));
    const auto refused = std::stoul(burst.substr(colon+1));
    require(queued == pushed && refused > 0 && queued+refused == 64, "push bounds not enforced: "+burst);
    // The same contract through a gateway: pushes relay unsolicited.
    RawSocket via(gateway);
    send_frame(via, "self");
    equal(read_frame(via), "session=3");
    send_frame(via, "push");
    equal(read_frame(via), "pushed");
    equal(read_frame(via), "done");
    // Pushes and broadcast also work on the ws game-session leg.
    {
        WsRaw over(ws_port);
        over.text("self");
        equal(over.read_text(), "session=4");
        over.text("push");
        equal(over.read_text(), "pushed");
        equal(over.read_text(), "done");
        over.text("bcast");
        equal(over.read_text(), "bc:4");
        equal(over.read_text(), "bcast=4");
    }
    require(node.output().find("callback failed") == std::string::npos, "push path logged Lua failures");
}
}
