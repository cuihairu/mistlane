// SPDX-License-Identifier: Apache-2.0
#include "test_common.hpp"
namespace mistlane::test {
void coordinator_registry() {
    Context c;
    write(c.path("echo.lua"), correlate_lua);
    const auto registry = port(), metrics = port(), tokened = port(),
              first = port(), second = port(), third = port();
    auto& coord = c.node("coordinator", registry, {"--metrics-listen", endpoint(metrics)});
    c.node("coordinator", tokened, {"--service-token", "s3cret"});
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
    const auto expect_close = [](RawSocket& raw) {
        for (int i = 0; i < 40 && !raw.closed(); ++i) std::this_thread::sleep_for(50ms);
        require(raw.closed(), "connection stayed open after a refusal");
    };
    // The registry is not a forwarding role: unknown traffic is refused.
    RawSocket stranger(registry);
    send_frame(stranger, "hello");
    equal(read_frame(stranger), "MLR1 ERR UNEXPECTED");
    expect_close(stranger);
    // Malformed registrations report the shape.
    for (const auto& bad : {"MLR1 REGISTER", "MLR1 REGISTER data NS ns gs-x alpha 127.0.0.1:1 0",
                            "MLR1 REGISTER gameServer NS ns gs-x alpha not-an-endpoint 0"}) {
        RawSocket malformed(registry);
        send_frame(malformed, bad);
        equal(read_frame(malformed), "MLR1 ERR INVALID");
        expect_close(malformed);
    }
    // Members join two scopes; the first saturates under real load.
    auto& gs_a = c.node("gameServer", first, {"--script", c.path("echo.lua").string(),
        "--coordinator", endpoint(registry), "--scope", "alpha", "--service-name", "gs-a",
        "--max-connections", "4", "--overload-ratio", "0.5", "--ready-hysteresis", "1"});
    auto& gs_b = c.node("gameServer", second, {"--script", c.path("echo.lua").string(),
        "--coordinator", endpoint(registry), "--scope", "alpha", "--service-name", "gs-b"});
    auto& gs_c = c.node("gameServer", third, {"--script", c.path("echo.lua").string(),
        "--coordinator", endpoint(registry), "--scope", "beta", "--service-name", "gs-c"});
    std::string body;
    const auto query = [&](const std::string& line) {
        RawSocket raw(registry);
        send_frame(raw, line);
        body = read_frame(raw);
        expect_close(raw);
        return body;
    };
    const auto place = [&](const std::string& line) {
        const auto response = query(line);
        std::cout << "registry ask " << line << " => " << response << '\n';
        return response;
    };
    eventually([&] { return query("MLR1 QUERY -").find("MLR1 MEMBERS 3") != std::string::npos; },
        5s, "members never reached the registry");
    require(body.find("member gameServer gs-a - alpha") != std::string::npos &&
            body.find("member gameServer gs-b - alpha") != std::string::npos &&
            body.find("member gameServer gs-c - beta") != std::string::npos,
            "registry listing lost a member: "+body);
    // Scope filters narrow the listing and placement.
    query("MLR1 QUERY gameServer - beta");
    require(body.find("gs-c") != std::string::npos && body.find("gs-a") == std::string::npos,
            "scope filter leaked another scope: "+body);
    const auto beta_place = "MLR1 PLACE gs-c 127.0.0.1:"+std::to_string(third)+" 0 ready 0/42";
    eventually([&] { return place("MLR1 PLACE gameServer - beta") == beta_place; },
        5s, "gs-c never became placeable");
    equal(place("MLR1 PLACE gameServer - beta"), beta_place);
    equal(place("MLR1 PLACE gameServer - gamma"), "MLR1 EMPTY");
    // Placement prefers the less loaded member once real load differs.
    {
        RawSocket load(first);
        send_frame(load, "ping"); equal(read_frame(load), "ping");
        eventually([&] { return place("MLR1 PLACE gameServer - alpha") ==
            "MLR1 PLACE gs-b 127.0.0.1:"+std::to_string(second)+" 0 ready 0/42"; },
            5s, "placement ignored member load");
        // Saturation flips readiness, and the registry follows the heartbeats.
        RawSocket full(first);
        send_frame(full, "ping"); equal(read_frame(full), "ping");
        eventually([&] { return query("MLR1 QUERY gameServer - alpha").find(
            "member gameServer gs-a - alpha 127.0.0.1:"+std::to_string(first)+" 0 notready 2/4")
            != std::string::npos; }, 5s, "readiness did not follow the heartbeat");
    }
    std::this_thread::sleep_for(600ms);
    eventually([&] { return query("MLR1 QUERY gameServer - alpha").find(
        "member gameServer gs-a - alpha 127.0.0.1:"+std::to_string(first)+" 0 ready 0/4")
        != std::string::npos; }, 5s, "readiness did not recover");
    // A disconnect drops the member from the registry.
    gs_c.signal(SIGTERM);
    auto live = http("http://"+endpoint(metrics)+"/metrics").body;
    eventually([&] {
        live = http("http://"+endpoint(metrics)+"/metrics").body;
        return live.find("mistlane_registry_members{role=\"coordinator\"} 2") != std::string::npos;
    }, 5s, "a dead member stayed registered");
    // The registry gauge follows the live membership.
    require(live.find("mistlane_registry_members{role=\"coordinator\"} 2") != std::string::npos,
            "registry gauge missing from the exposition");
    // Identity stacks on the registry: an unnamed member is refused, an
    // identified one registers normally.
    RawSocket anonymous(tokened);
    send_frame(anonymous, "MLR1 REGISTER gameServer gs-x NS ns alpha 127.0.0.1:1 0");
    equal(read_frame(anonymous), "MLS1 ERR TOKEN");
    expect_close(anonymous);
    auto& gs_d = c.node("gameServer", port(), {"--script", c.path("echo.lua").string(),
        "--coordinator", endpoint(tokened), "--upstream-token", "s3cret", "--service-name", "gs-d"});
    const auto identified = [&]() {
        RawSocket raw(tokened);
        send_frame(raw, "MLS1 registryClient query-1 s3cret");
        equal(read_frame(raw), "MLS1 OK");
        send_frame(raw, "MLR1 QUERY -");
        body = read_frame(raw);
        expect_close(raw);
        return body;
    };
    eventually([&] { return identified().find("member gameServer gs-d - main") != std::string::npos; },
        5s, "an identified member never registered");
    require(gs_a.output().find("registered") != std::string::npos &&
            gs_d.output().find("registered") != std::string::npos,
            "member registration not logged");
    // The coordinator is not a renamed router: it takes neither script nor
    // upstreams, and the member flags do not leak into other roles.
    {
        auto& scripted = c.spawn({self().string(), "--role", "coordinator", "--listen", endpoint(port()),
            "--script", c.path("echo.lua").string()});
        require(scripted.wait() != 0 && scripted.output().find("requires no script and no upstream") != std::string::npos,
                "coordinator accepted a script");
        auto& routed = c.spawn({self().string(), "--role", "gateway", "--listen", endpoint(port()),
            "--upstream", endpoint(registry), "--coordinator", endpoint(registry)});
        require(routed.wait() != 0 && routed.output().find("--coordinator applies to gameServer, crossServer and login") != std::string::npos,
                "gateway accepted a coordinator");
        auto& scoped = c.spawn({self().string(), "--role", "gameServer", "--listen", endpoint(port()),
            "--script", c.path("echo.lua").string(), "--scope", "alpha"});
        require(scoped.wait() != 0 && scoped.output().find("--namespace and --scope require --coordinator") != std::string::npos,
                "scope accepted without a coordinator");
    }
    // A crossServer token can identify the registry leg, the MLD1 data leg,
    // or both; only an endpoint-less token is silent and must refuse to start.
    // The service name is concatenated into the MLS1 frame, so whitespace or
    // any other non-identifier character would corrupt the handshake.
    {
        auto& orphan = c.spawn({self().string(), "--role", "crossServer", "--listen", endpoint(port()),
            "--script", c.path("echo.lua").string(), "--upstream-token", "s3cret"});
        require(orphan.wait() != 0 && orphan.output().find("--coordinator or --data") != std::string::npos,
                "crossServer token accepted without a registry or data endpoint");
        write(c.path("space.lua"), "return {message=function(p)return p end}");
        auto& spaced = c.spawn({self().string(), "--role", "crossServer", "--listen", endpoint(port()),
            "--script", c.path("space.lua").string(), "--service-name", "a b"});
        require(spaced.wait() != 0 && spaced.output().find("--service-name must be a single word") != std::string::npos,
                "service name whitespace was accepted");
        // A registering member's service name becomes the registry instance,
        // so a non-record value would pass the single-word check above and
        // then be rejected forever by the coordinator; fail it here instead.
        auto& colon_named = c.spawn({self().string(), "--role", "crossServer", "--listen", endpoint(port()),
            "--script", c.path("echo.lua").string(), "--coordinator", endpoint(tokened),
            "--upstream-token", "s3cret", "--service-name", "xs:1"});
        require(colon_named.wait() != 0
                && colon_named.output().find("registering member must be a record identifier") != std::string::npos,
                "a non-record member service name was accepted");
        auto& bad_scope = c.spawn({self().string(), "--role", "crossServer", "--listen", endpoint(port()),
            "--script", c.path("echo.lua").string(), "--coordinator", endpoint(tokened),
            "--upstream-token", "s3cret", "--scope", "a:b"});
        require(bad_scope.wait() != 0
                && bad_scope.output().find("--scope on a registering member must be a record identifier") != std::string::npos,
                "a non-record member scope was accepted");
        auto& default_scope = c.spawn({self().string(), "--role", "crossServer", "--listen", endpoint(port()),
            "--script", c.path("echo.lua").string(), "--coordinator", endpoint(tokened),
            "--upstream-token", "s3cret", "--scope", "-"});
        require(default_scope.wait() != 0
                && default_scope.output().find("--scope on a registering member must be a record identifier") != std::string::npos,
                "a default-group member scope was accepted");
        auto& bad_ns = c.spawn({self().string(), "--role", "crossServer", "--listen", endpoint(port()),
            "--script", c.path("echo.lua").string(), "--coordinator", endpoint(tokened),
            "--upstream-token", "s3cret", "--namespace", "n:m"});
        require(bad_ns.wait() != 0
                && bad_ns.output().find("--namespace on a registering member must be a record identifier") != std::string::npos,
                "a non-record member namespace was accepted");
        c.node("crossServer", port(), {"--script", c.path("echo.lua").string(),
            "--coordinator", endpoint(tokened), "--upstream-token", "s3cret", "--service-name", "cross1"});
        eventually([&] { return identified().find("member crossServer cross1") != std::string::npos; },
            5s, "crossServer did not register with a coordinator-only token");
    }
    for (const auto* member : {&gs_a, &gs_b})
        require(member->output().find("callback failed") == std::string::npos, "registry member logged Lua failures");
    require(coord.output().find("unexpected frame") == std::string::npos, "coordinator log noise");
}
// login is a registry client, not a member: each placement ask owns one
// connection, and the callback answers through mistlane.session.send because
// the answer arrives after the handler returned.
void coordinator_placement() {
    Context c;
    write(c.path("echo.lua"), echo);
    write(c.path("place.lua"),
        "return {message=function(p,c)"
        " local role, namespace_, scope = p:match('place (%S+) ?(%S*) ?(%S*)')\n"
        " if not role then return 'usage' end\n"
        " namespace_ = namespace_ ~= '' and namespace_ ~= '-' and namespace_ or nil\n"
        " mistlane.place(role, namespace_,"
        " scope ~= '' and scope or nil, function(err, address)\n"
        "   mistlane.session.send(c.session, err and ('ERR '..err) or ('OK '..address))\n"
        " end)\n"
        " return nil end}");
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
    const auto place = [&](unsigned listen, std::string_view request) {
        RawSocket raw(listen);
        send_frame(raw, request);
        return read_frame(raw);
    };
    const auto members = [&](unsigned metrics) {
        const auto body = http("http://"+endpoint(metrics)+"/metrics").body;
        const auto needle = std::string("mistlane_registry_members{role=\"coordinator\"} ");
        const auto at = body.find(needle);
        return at == std::string::npos ? -1 : static_cast<int>(std::strtod(body.c_str()+at+needle.size(), nullptr));
    };
    const auto registry = port(), member_listen = port(), login = port(), metrics = port(),
             token_registry = port(), token_member = port(), token_login = port(),
             wrong_login = port(), dead_login = port();
    auto& coord = c.node("coordinator", registry, {"--metrics-listen", endpoint(metrics)});
    auto& member = c.spawn({self().string(), "--role", "gameServer", "--listen", endpoint(member_listen),
             "--script", c.path("echo.lua").string(), "--coordinator", endpoint(registry)});
    member.ready("ready gameServer " + endpoint(member_listen));
    eventually([&] { return members(metrics) == 1; }, 5s, "the member never registered");
    c.node("login", login, {"--script", c.path("place.lua").string(), "--transport", "tcp",
        "--coordinator", endpoint(registry)});
    // Registration precedes the first readiness heartbeat, so the ask waits
    // for the ready verdict instead of racing it.
    eventually([&] { return place(login, "place gameServer")
        == "OK 127.0.0.1:"+std::to_string(member_listen); }, 5s, "placement never saw the ready member");
    equal(place(login, "place gameServer - nobody"), "ERR EMPTY");
    equal(place(login, "place gameServer - shop"), "ERR EMPTY");
    equal(place(login, "place gameServer shop nobody"), "ERR EMPTY");
    equal(place(login, "place crossServer"), "ERR EMPTY");
    equal(place(login, "where"), "usage");
    require(members(metrics) == 1, "login registered itself as a member");
    // A tokened coordinator gates the lookup behind MLS1.
    auto& token_coord = c.node("coordinator", token_registry, {"--service-token", "s3cret"});
    auto& token_member_node = c.spawn({self().string(), "--role", "gameServer", "--listen", endpoint(token_member),
             "--script", c.path("echo.lua").string(), "--coordinator", endpoint(token_registry),
             "--upstream-token", "s3cret", "--service-name", "gs-token"});
    token_member_node.ready("ready gameServer " + endpoint(token_member));
    c.node("login", token_login, {"--script", c.path("place.lua").string(), "--transport", "tcp",
        "--coordinator", endpoint(token_registry), "--upstream-token", "s3cret", "--service-name", "login-1"});
    c.node("login", wrong_login, {"--script", c.path("place.lua").string(), "--transport", "tcp",
        "--coordinator", endpoint(token_registry), "--upstream-token", "nope"});
    eventually([&] { return place(token_login, "place gameServer")
        == "OK 127.0.0.1:"+std::to_string(token_member); }, 5s, "identity did not reach the tokened registry");
    equal(place(wrong_login, "place gameServer"), "ERR TOKEN");
    // A dead coordinator reports a reason instead of holding the session.
    c.node("login", dead_login, {"--script", c.path("place.lua").string(), "--transport", "tcp",
        "--coordinator", endpoint(port())});
    equal(place(dead_login, "place gameServer"), "ERR unreachable");
    // The registry stays the coordinator's job: data never presents itself.
    auto& data = c.spawn({self().string(), "--role", "data", "--listen", endpoint(port()),
        "--script", c.path("place.lua").string(), "--coordinator", endpoint(registry)});
    require(data.wait() != 0 && data.output().find("--coordinator applies to gameServer, crossServer and login")
        != std::string::npos, "data accepted registry placement");
    require(coord.output().find("member registered login") == std::string::npos, "login registered as a member");
    require(coord.output().find("unexpected frame") == std::string::npos, "coordinator log noise");
    require(token_coord.output().find("unexpected frame") == std::string::npos, "coordinator log noise");
}
void coordinator_routes() {
    Context c;
    write(c.path("echo.lua"), echo);
    const auto registry = port(), metrics = port(), tokened = port(),
              first = port(), second = port(), third = port();
    auto& coord = c.node("coordinator", registry, {"--metrics-listen", endpoint(metrics)});
    c.node("coordinator", tokened, {"--service-token", "s3cret"});
    auto& gs_a = c.node("gameServer", first, {"--script", c.path("echo.lua").string(),
        "--coordinator", endpoint(registry), "--scope", "alpha", "--service-name", "gs-a"});
    auto& gs_b = c.node("gameServer", second, {"--script", c.path("echo.lua").string(),
        "--coordinator", endpoint(registry), "--scope", "alpha", "--service-name", "gs-b"});
    auto& gs_ns = c.node("gameServer", third, {"--script", c.path("echo.lua").string(),
        "--coordinator", endpoint(registry), "--namespace", "ns", "--scope", "alpha",
        "--service-name", "gs-n"});
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
    const auto expect_close = [](RawSocket& raw) {
        for (int i = 0; i < 40 && !raw.closed(); ++i) std::this_thread::sleep_for(50ms);
        require(raw.closed(), "connection stayed open after a refusal");
    };
    std::string body;
    // One-shot legs: one frame in, one verdict out, then the coordinator closes.
    const auto ask = [&](const std::string& line) {
        RawSocket raw(registry);
        send_frame(raw, line);
        body = read_frame(raw);
        expect_close(raw);
        return body;
    };
    eventually([&] { return ask("MLR1 QUERY -").find("MLR1 MEMBERS 2") != std::string::npos; },
        5s, "members never reached the registry");
    // GET before any SET is an honest empty, not an error.
    equal(ask("MLR1 ROUTE GET NS - mist eve"), "MLR1 ROUTE EMPTY");
    // Malformed shapes and unknown verbs report the right word.
    for (const auto& [line, word] : std::vector<std::pair<std::string, std::string>>{
            {"MLR1 ROUTE SET NS ns alpha mist eve gameServer", "MLR1 ERR INVALID"},
            {"MLR1 ROUTE SET NS ns alpha mist eve gameServer gs-a extra", "MLR1 ERR INVALID"},
            {"MLR1 ROUTE SET NS ns alpha mi!t eve gameServer gs-a", "MLR1 ERR INVALID"},
            {"MLR1 ROUTE SET NS ns alpha mist eve data gs-a", "MLR1 ERR INVALID"},
            {"MLR1 ROUTE GET ns mist", "MLR1 ERR INVALID"},
            {"MLR1 ROUTE GET ns mist ev!", "MLR1 ERR INVALID"},
            {"MLR1 ROUTE LIST", "MLR1 ERR UNEXPECTED"},
            {"MLR1 ROUTE", "MLR1 ERR UNEXPECTED"}}) {
        RawSocket raw(registry);
        send_frame(raw, line);
        equal(read_frame(raw), word);
        expect_close(raw);
    }
    // A route needs a live host: an unregistered instance is refused by name.
    equal(ask("MLR1 ROUTE SET NS - alpha mist eve gameServer gs-z"), "MLR1 ERR UNKNOWN");
    equal(ask("MLR1 ROUTE SET NS - alpha mist eve gameServer gs-a"), "MLR1 ROUTE OK");
    auto live = http("http://"+endpoint(metrics)+"/metrics").body;
    require(live.find("mistlane_registry_routes{role=\"coordinator\"} 1") != std::string::npos,
            "route gauge did not follow the record count");
    const auto eve_at = "MLR1 ROUTE AT alpha gameServer gs-a 127.0.0.1:"+
        std::to_string(first)+" 0 ready 0/42 - 0";
    eventually([&] { return ask("MLR1 ROUTE GET NS - mist eve") == eve_at; },
        5s, "route did not follow the live member state");
    // A second SET for the same character overwrites; the count stays at one.
    equal(ask("MLR1 ROUTE SET NS - alpha mist eve gameServer gs-b - town-square"), "MLR1 ROUTE OK");
    const auto eve_moved = "MLR1 ROUTE AT alpha gameServer gs-b 127.0.0.1:"+
        std::to_string(second)+" 0 ready 0/42 town-square 0";
    eventually([&] { return ask("MLR1 ROUTE GET NS - mist eve") == eve_moved; },
        5s, "route overwrite did not stick");
    // Two namespaces may reuse one scope name: members and route records stay
    // apart, and a character route in each answers its own host.
    eventually([&] { return ask("MLR1 QUERY - ns alpha").find(
        "member gameServer gs-n ns alpha 127.0.0.1:"+std::to_string(third)) != std::string::npos; },
        5s, "a namespaced member never reached the registry");
    const auto default_listing = ask("MLR1 QUERY - - alpha");
    require(default_listing.find("MLR1 MEMBERS 2") != std::string::npos &&
            default_listing.find("gs-a") != std::string::npos &&
            default_listing.find("gs-b") != std::string::npos &&
            default_listing.find("gs-n") == std::string::npos,
            "a same-scope member leaked across namespaces: "+default_listing);
    equal(ask("MLR1 ROUTE SET NS ns alpha mist eve gameServer gs-n"), "MLR1 ROUTE OK");
    eventually([&] { return ask("MLR1 ROUTE GET NS ns mist eve") ==
        "MLR1 ROUTE AT alpha gameServer gs-n 127.0.0.1:"+std::to_string(third)+" 0 ready 0/42 - 0"; },
        5s, "a namespaced route did not answer its own host");
    equal(ask("MLR1 ROUTE GET NS - mist eve"), eve_moved);
    equal(ask("MLR1 PLACE gameServer NS ns alpha"),
        "MLR1 PLACE gs-n 127.0.0.1:"+std::to_string(third)+" 0 ready 0/42");
    // A registered member publishes routes on its own control leg and the leg
    // keeps heartbeating; GET mirrors the heartbeat state it announces.
    {
        RawSocket host(registry);
        send_frame(host, "MLR1 REGISTER gameServer gs-r NS - alpha 127.0.0.1:1 0");
        equal(read_frame(host), "MLR1 OK");
        send_frame(host, "MLR1 ROUTE SET NS - alpha mist rogue gameServer gs-r");
        equal(read_frame(host), "MLR1 ROUTE OK");
        send_frame(host, "MLR1 notready 0 42");
        equal(read_frame(host), "MLR1 OK");
        equal(ask("MLR1 ROUTE GET NS - mist rogue"),
              "MLR1 ROUTE AT alpha gameServer gs-r 127.0.0.1:1 0 notready 0/42 - 0");
        send_frame(host, "MLR1 ready 1 42");
        equal(read_frame(host), "MLR1 OK");
        equal(ask("MLR1 ROUTE GET NS - mist rogue"),
              "MLR1 ROUTE AT alpha gameServer gs-r 127.0.0.1:1 0 ready 1/42 - 0");
        live = http("http://"+endpoint(metrics)+"/metrics").body;
        require(live.find("mistlane_registry_routes{role=\"coordinator\"} 3") != std::string::npos,
                "route gauge missed the second record");
    } // the host retires here
    eventually([&] { return ask("MLR1 ROUTE GET NS - mist rogue") == "MLR1 ROUTE EMPTY"; },
        5s, "a retired host kept its route");
    live = http("http://"+endpoint(metrics)+"/metrics").body;
    require(live.find("mistlane_registry_routes{role=\"coordinator\"} 2") != std::string::npos,
            "route gauge did not follow the retirement");
    // Placement beside the route verbs stays member placement.
    eventually([&] { return ask("MLR1 PLACE gameServer - alpha") ==
        "MLR1 PLACE gs-a 127.0.0.1:"+std::to_string(first)+" 0 ready 0/42"; },
        5s, "placement broke beside the route verbs");
    // Identity gates routes like everything else on a tokened coordinator.
    RawSocket anonymous(tokened);
    send_frame(anonymous, "MLR1 ROUTE GET NS - mist eve");
    equal(read_frame(anonymous), "MLS1 ERR TOKEN");
    expect_close(anonymous);
    c.node("gameServer", port(), {"--script", c.path("echo.lua").string(),
        "--coordinator", endpoint(tokened), "--upstream-token", "s3cret", "--service-name", "gs-d"});
    const auto identified = [&](const std::string& line) {
        RawSocket raw(tokened);
        send_frame(raw, "MLS1 registryClient routes-1 s3cret");
        equal(read_frame(raw), "MLS1 OK");
        send_frame(raw, line);
        body = read_frame(raw);
        expect_close(raw);
        return body;
    };
    eventually([&] { return identified("MLR1 ROUTE SET NS - main mist kate gameServer gs-d") ==
        "MLR1 ROUTE OK"; }, 5s, "an identified route publish never landed");
    body = identified("MLR1 ROUTE GET NS - mist kate");
    require(body.starts_with("MLR1 ROUTE AT main gameServer gs-d "), "route answer wrong shape: "+body);
    require(gs_a.output().find("callback failed") == std::string::npos &&
            gs_b.output().find("callback failed") == std::string::npos,
            "registry member logged Lua failures");
    require(coord.output().find("unexpected frame") == std::string::npos, "coordinator log noise");
}
namespace {
struct MigrationFixture {
    MigrationKey key{"home", "hero"};
    MigrationOwner source{{MigrationScope::Kind::Realm, "home", ""}, "gameServer", "gs-a", "town"};
    MigrationOwner target{{MigrationScope::Kind::CrossGroup, "", "arena"}, "crossServer", "xs-a", "room-1"};
    HomeGenerationAuthority home;
    MigrationFixture() { require(home.seed(key, 8), "home authority seed failed"); }
    MigrationTransaction transaction() { return MigrationTransaction(home, key, {source, 8}); }
    MigrationRequest request(std::string id = "move-1") { return {key, std::move(id), {source, 8}, target}; }
};
void migration_step(MigrationTransaction& tx, const MigrationToken& token, MigrationEvent event) {
    require(tx.apply(token, event) == MigrationResult::Applied, "migration step failed: " + std::to_string(static_cast<int>(event)));
}
void migration_ready(MigrationTransaction& tx, const MigrationToken& token) {
    for (auto event : {MigrationEvent::SourceDrained, MigrationEvent::HomeFence, MigrationEvent::DestinationReady})
        migration_step(tx, token, event);
}
}
void coordinator_migration_commit() {
    MigrationFixture f;
    auto tx = f.transaction();
    require(tx.state() == MigrationState::Idle && tx.begin_request(), "idle source not active");
    const auto request = f.request();
    require(tx.prepare(request) == MigrationResult::Applied, "prepare failed");
    const auto token = tx.entry(request.id)->token;
    require(token.generation == 9 && !tx.timers_running(), "home generation or source timer gate failed");
    require(!tx.write_allowed(f.source, 8) && !tx.write_allowed(f.target, 9), "prepare admitted a writer");
    require(tx.apply(token, MigrationEvent::SourceDrained) == MigrationResult::Order && tx.finish_request(),
        "source drain ignored in-flight handler");
    migration_ready(tx, token);
    require(tx.route() == request.expected && tx.write_allowed(f.target, 9), "target ACK prematurely published route");
    migration_step(tx, token, MigrationEvent::Commit);
    require(tx.state() == MigrationState::Committed && tx.route() == MigrationRoute{f.target, 9}
            && !tx.write_allowed(f.source, 8) && tx.timers_running(), "commit did not publish sole target owner");
    require(tx.apply(token, MigrationEvent::Commit) == MigrationResult::Duplicate
            && tx.prepare(request) == MigrationResult::Duplicate && f.home.issued(f.key) == 9, "commit/prepare replay changed generation");
    // Explicit scope/role/scene checks, and identical character ids in two realms.
    auto bad = MigrationFixture{}.request();
    bad.id = "bad-scope"; bad.destination.scope.realm = "home";
    require(tx.prepare(bad) == MigrationResult::Invalid, "mixed realm/group scope accepted");
    bad.destination = f.source; bad.destination.role = "crossServer";
    require(tx.prepare(bad) == MigrationResult::Invalid, "role incompatible with scope accepted");
    bad.destination = f.target; bad.destination.scene.clear();
    require(tx.prepare(bad) == MigrationResult::Invalid, "missing scene ownership accepted");
    const MigrationKey other_key{"other", "hero"};
    require(f.home.seed(other_key, 1), "realm-isolated seed failed");
    MigrationTransaction other(f.home, other_key, {f.source, 1});
    require(other.prepare({other_key, "move-1", {f.source, 1}, f.target}) == MigrationResult::Applied
            && other.entry("move-1")->token.generation == 2 && f.home.issued(f.key) == 9, "realm generation keys collided");
    std::cout << "migration.commit: drained -> fenced -> destination ACK -> route gen=9; scoped ownership [PASS]\n";
}
void coordinator_migration_abort() {
    for (bool drained : {false, true}) {
        MigrationFixture f;
        auto tx = f.transaction();
        require(tx.begin_request(), "abort fixture handler missing");
        require(tx.prepare(f.request()) == MigrationResult::Applied, "abort prepare failed");
        const auto token = tx.entry("move-1")->token;
        if (drained) {
            require(tx.finish_request(), "drain failed");
            migration_step(tx, token, MigrationEvent::SourceDrained);
        }
        migration_step(tx, token, MigrationEvent::Abort);
        require(tx.state() == MigrationState::Aborting && !tx.write_allowed(f.source, 8), "abort resumed before cleanup");
        require(tx.apply(token, MigrationEvent::HomeFence) == MigrationResult::Order, "abort raced into fence");
        if (!drained) {
            require(tx.apply(token, MigrationEvent::Cleanup) == MigrationResult::Pending && tx.finish_request(),
                "abort cleanup ignored executing handler");
        }
        migration_step(tx, token, MigrationEvent::Cleanup);
        require(tx.state() == MigrationState::Aborted && tx.route() == f.request().expected
                && tx.write_allowed(f.source, 8) && !tx.write_allowed(f.target, 9)
                && tx.timers_running() && !tx.entry("move-1")->destination_staged, "abort left staged target or frozen source");
        require(tx.apply(token, MigrationEvent::Abort) == MigrationResult::Duplicate, "abort replay failed");
        require(tx.prepare(f.request("move-2")) == MigrationResult::Applied
                && tx.entry("move-2")->token.generation == 10, "aborted generation was reused");
    }
    std::cout << "migration.abort: half-built target discarded, source gen=8 restored, next gen=10 [PASS]\n";
}
void coordinator_migration_recover() {
    // Crash cuts: prepare, source drain, fence, target ACK, committed,
    // abort requested, aborted. Restore trusted protocol/home checkpoints;
    // no test claims that the current data endpoint persists these facts.
    for (unsigned cut = 0; cut < 7; ++cut) {
        MigrationFixture f;
        std::optional<MigrationTransaction::Snapshot> checkpoint;
        MigrationToken token;
        {
            auto crashed = f.transaction();
            require(crashed.prepare(f.request()) == MigrationResult::Applied, "recover prepare failed");
            token = crashed.entry("move-1")->token;
            if (cut >= 1 && cut <= 4) migration_step(crashed, token, MigrationEvent::SourceDrained);
            if (cut >= 2 && cut <= 4) migration_step(crashed, token, MigrationEvent::HomeFence);
            if (cut >= 3 && cut <= 4) migration_step(crashed, token, MigrationEvent::DestinationReady);
            if (cut == 4) migration_step(crashed, token, MigrationEvent::Commit);
            if (cut >= 5) migration_step(crashed, token, MigrationEvent::Abort);
            if (cut == 6) migration_step(crashed, token, MigrationEvent::Cleanup);
            checkpoint = crashed.snapshot();
        }
        auto restored_home = f.home;
        MigrationTransaction restored(restored_home, *checkpoint);
        auto result = restored.apply(token, MigrationEvent::Recover);
        if (cut == 2) {
            require(result == MigrationResult::Pending && restored.state() == MigrationState::Recovering
                    && !restored.write_allowed(f.source, 8) && !restored.write_allowed(f.target, 9), "recovery invented destination ACK");
            require(restored.apply(token, MigrationEvent::Abort) == MigrationResult::Order, "post-fence recovery revived source");
            // A second crash while recovering remains pending until the target ACK.
            MigrationTransaction again(restored_home, restored.snapshot());
            require(again.apply(token, MigrationEvent::Recover) == MigrationResult::Pending, "recover replay changed decision");
            migration_step(again, token, MigrationEvent::DestinationReady);
            migration_step(again, token, MigrationEvent::Recover);
            require(again.state() == MigrationState::Committed && again.route() == MigrationRoute{f.target, 9}, "forward recovery did not commit");
        } else {
            require(result == MigrationResult::Applied || result == MigrationResult::Duplicate, "recovery failed");
            const bool forward = cut == 3 || cut == 4;
            require(restored.state() == (forward ? MigrationState::Committed : MigrationState::Aborted)
                    && restored.route() == (forward ? MigrationRoute{f.target, 9} : f.request().expected), "wrong crash terminal state");
            require(restored.apply(token, MigrationEvent::Recover) == MigrationResult::Duplicate, "terminal recovery not idempotent");
        }
        require(restored_home.issued(f.key) == 9, "recovery lost reserved generation");
    }
    std::cout << "migration.recover: 7 checkpoint cuts -> aborted/committed; fenced recovery waits for ACK [PASS]\n";
}
void coordinator_migration_stale() {
    MigrationFixture f;
    auto tx = f.transaction();
    require(tx.prepare(f.request()) == MigrationResult::Applied, "stale prepare failed");
    const auto first = tx.entry("move-1")->token;
    auto stale = first; stale.generation = 8;
    for (auto event : {MigrationEvent::HomeFence, MigrationEvent::DestinationReady, MigrationEvent::Commit,
                       MigrationEvent::Abort, MigrationEvent::Cleanup, MigrationEvent::Recover})
        require(tx.apply(stale, event) == MigrationResult::Stale, "late old-generation control event accepted");
    migration_ready(tx, first);
    migration_step(tx, first, MigrationEvent::Commit);
    require(!tx.write_allowed(f.source, 8) && !tx.write_allowed(f.target, 10), "old or unissued writer accepted");
    require(tx.prepare(f.request("stale-owner")) == MigrationResult::Stale, "stale expected owner/route accepted");
    require(tx.prepare({f.key, "move-2", tx.route(), f.source}) == MigrationResult::Applied, "return prepare failed");
    const auto second = tx.entry("move-2")->token;
    migration_ready(tx, second);
    migration_step(tx, second, MigrationEvent::Commit);
    require(tx.apply(first, MigrationEvent::Commit) == MigrationResult::Duplicate
            && tx.route() == MigrationRoute{f.source, 10} && !tx.write_allowed(f.target, 9), "late commit republished obsolete route");
    auto wrong_key = second; wrong_key.key.first = "other";
    require(tx.apply(wrong_key, MigrationEvent::Recover) == MigrationResult::Invalid, "cross-realm event accepted");
    std::cout << "migration.stale: old generation/control/owner rejected; old commit cannot republish [PASS]\n";
}
void coordinator_migration_order() {
    MigrationFixture f;
    auto tx = f.transaction();
    require(tx.prepare(f.request()) == MigrationResult::Applied, "order prepare failed");
    const auto token = tx.entry("move-1")->token;
    require(tx.prepare(f.request("concurrent")) == MigrationResult::Conflict, "concurrent migration accepted");
    auto changed = f.request(); changed.destination.instance = "xs-b";
    require(tx.prepare(changed) == MigrationResult::Conflict, "request id reused with another target");
    require(tx.apply(token, MigrationEvent::HomeFence) == MigrationResult::Order
            && tx.apply(token, MigrationEvent::DestinationReady) == MigrationResult::Order
            && tx.apply(token, MigrationEvent::Commit) == MigrationResult::Order, "pre-drain publication/grant accepted");
    migration_step(tx, token, MigrationEvent::SourceDrained);
    require(tx.apply(token, MigrationEvent::Commit) == MigrationResult::Order, "route published before home fence");
    migration_step(tx, token, MigrationEvent::HomeFence);
    require(tx.apply(token, MigrationEvent::Commit) == MigrationResult::Order
            && tx.apply(token, MigrationEvent::Abort) == MigrationResult::Order
            && !tx.write_allowed(f.source, 8), "fenced source revived or route published before target ACK");
    migration_step(tx, token, MigrationEvent::DestinationReady);
    require(tx.route() == f.request().expected, "route changed before commit");
    // Simulate a competing directory edit in a trusted checkpoint: CAS fails.
    auto changed_route = tx.snapshot(); changed_route.route.owner.instance = "gs-other";
    MigrationTransaction competing(f.home, changed_route);
    require(competing.apply(token, MigrationEvent::Commit) == MigrationResult::Order, "expected-route CAS bypassed");
    migration_step(tx, token, MigrationEvent::Commit);
    require(tx.route() == MigrationRoute{f.target, 9}, "ordered commit did not publish");
    std::cout << "migration.order: premature fence/activation/publication and route CAS conflict rejected [PASS]\n";
}
void coordinator_migration_retire() {
    // A member that leaves the registry cannot finish the transaction it was
    // moving a participant to or from, so the transaction has to end instead
    // of blocking every later prepare forever. The source's handler is dead
    // with the process, so it is force-drained here; a destination retirement
    // leaves a live source, whose handler still owns admission and is let
    // finish first, exactly like any other abort.
    {
        MigrationFixture f;
        auto tx = f.transaction();
        require(tx.begin_request(), "retire fixture handler missing");
        require(tx.prepare(f.request()) == MigrationResult::Applied, "retire prepare failed");
        const auto token = tx.entry("move-1")->token;
        require(tx.retire_member(token, f.source) == MigrationResult::Applied
                && tx.state() == MigrationState::Aborted,
                "a retired source did not drain its own handler and abort");
        auto owner = tx.lookup_ownership(f.key);
        require(tx.route() == f.request().expected && tx.write_allowed(f.source, 8)
                && !tx.write_allowed(f.target, 9) && tx.timers_running()
                && !tx.entry("move-1")->destination_staged
                && owner && *owner == HomeOwnership{f.source, 8, HomeOwnershipState::Active}
                && f.home.issued(f.key) == 9,
                "source retirement left the staged target or froze the source");

        // Takeover: the aborted generation is consumed and never reused, but
        // the route still names the participant, so a strictly newer
        // generation may move it onto a living member.
        MigrationOwner alive = f.target; alive.instance = "xs-b";
        MigrationRequest takeover{f.key, "takeover", f.request().expected, alive};
        require(tx.prepare(takeover) == MigrationResult::Applied
                && tx.entry("takeover")->token.generation == 10,
                "takeover did not reserve a newer generation");
        const auto next = tx.entry("takeover")->token;
        migration_ready(tx, next);
        migration_step(tx, next, MigrationEvent::Commit);
        require(tx.state() == MigrationState::Committed && tx.route() == MigrationRoute{alive, 10}
                && tx.write_allowed(alive, 10) && !tx.write_allowed(f.source, 8)
                && !tx.write_allowed(f.target, 9), "takeover did not publish the living destination");
        MigrationTransaction checkpoint(f.home, tx.snapshot());
        require(checkpoint.state() == MigrationState::Committed && checkpoint.route() == MigrationRoute{alive, 10}
                && checkpoint.apply(token, MigrationEvent::Recover) == MigrationResult::Duplicate
                && f.home.issued(f.key) == 10, "retirement checkpoint did not restore its journal");
    }
    {
        MigrationFixture f;
        auto tx = f.transaction();
        require(tx.begin_request(), "destination-retire fixture handler missing");
        require(tx.prepare(f.request()) == MigrationResult::Applied, "destination-retire prepare failed");
        const auto token = tx.entry("move-1")->token;
        require(tx.retire_member(token, f.target) == MigrationResult::Pending
                && tx.state() == MigrationState::Preparing && !tx.timers_running()
                && !tx.write_allowed(f.source, 8),
                "a retired destination advanced the transaction or resumed the source's writes");
        require(tx.finish_request(), "the live source handler did not finish");
        require(tx.retire_member(token, f.target) == MigrationResult::Applied
                && tx.state() == MigrationState::Aborted && tx.write_allowed(f.source, 8)
                && tx.timers_running() && !tx.entry("move-1")->destination_staged,
                "destination retirement did not abort with the source restored");
        require(tx.apply(token, MigrationEvent::Abort) == MigrationResult::Duplicate,
                "abort replay after retirement changed the outcome");
    }
    {
        MigrationFixture f;
        auto tx = f.transaction();
        require(tx.prepare(f.request()) == MigrationResult::Applied, "fenced-retire prepare failed");
        const auto token = tx.entry("move-1")->token;
        migration_step(tx, token, MigrationEvent::SourceDrained);
        migration_step(tx, token, MigrationEvent::HomeFence);
        auto fenced = tx.lookup_ownership(f.key);
        require(fenced && fenced->state == HomeOwnershipState::Fenced && !tx.write_allowed(f.source, 8),
                "fence fixture did not fence the source");
        require(tx.retire_member(token, f.target) == MigrationResult::Pending
                && tx.retire_member(token, f.source) == MigrationResult::Pending
                && tx.state() == MigrationState::Preparing && !tx.write_allowed(f.source, 8)
                && !tx.timers_running(),
                "post-fence retirement mutated the fenced transaction");
        auto still = tx.lookup_ownership(f.key);
        require(still && still->state == HomeOwnershipState::Fenced
                && tx.apply(token, MigrationEvent::Recover) == MigrationResult::Pending,
                "fence retirement invented an owner or a destination acknowledgement");
    }
    {
        // Post-fence takeover: the destination leaves after the home fence,
        // leaving a tombstoned fenced gap with no owner. The coordinator must
        // not infer an owner from the dead member or revive the source; the
        // only way forward is an explicit takeover onto a living destination
        // with a strictly newer generation (the durable authority never reuses
        // a burned generation). The source is already drained and fenced, so
        // the new transaction starts at the destination ACK.
        MigrationFixture f;
        auto tx = f.transaction();
        require(tx.prepare(f.request()) == MigrationResult::Applied, "takeover prepare failed");
        const auto token = tx.entry("move-1")->token;
        migration_step(tx, token, MigrationEvent::SourceDrained);
        migration_step(tx, token, MigrationEvent::HomeFence);
        require(tx.retire_member(token, f.target) == MigrationResult::Pending
                && tx.retire_member(token, f.source) == MigrationResult::Pending,
                "fenced retirement was not acknowledged without mutation");
        auto gap = tx.lookup_ownership(f.key);
        require(gap && gap->state == HomeOwnershipState::Fenced && !gap->owner
                && !tx.write_allowed(f.source, 8) && !tx.write_allowed(f.target, 9),
                "takeover fixture did not leave a fenced gap with no owner");

        MigrationOwner alive = f.target; alive.instance = "xs-b";
        MigrationRequest takeover{f.key, "takeover", f.request().expected, alive};
        require(tx.takeover(takeover) == MigrationResult::Applied
                && tx.entry("takeover")->token.generation == 10
                && tx.entry("takeover")->source_drained && tx.entry("takeover")->home_fenced,
                "takeover did not reserve a newer generation in the fenced gap");
        require(tx.entry("move-1")->state == MigrationState::Aborted
                && !tx.entry("move-1")->destination_staged,
                "the superseded fenced transaction was not burned");
        auto now = tx.lookup_ownership(f.key);
        require(now && now->state == HomeOwnershipState::Fenced && !now->owner
                && f.home.issued(f.key) == 10 && !tx.write_allowed(alive, 10),
                "takeover prematurely granted an owner or reissued the old generation");
        const auto next = tx.entry("takeover")->token;
        require(tx.apply(next, MigrationEvent::DestinationReady) == MigrationResult::Applied
                && tx.write_allowed(alive, 10) && !tx.write_allowed(f.source, 8)
                && !tx.write_allowed(f.target, 9),
                "takeover destination ACK did not activate the strictly newer grant");
        migration_step(tx, next, MigrationEvent::Commit);
        require(tx.state() == MigrationState::Committed && tx.route() == MigrationRoute{alive, 10}
                && tx.apply(token, MigrationEvent::Recover) == MigrationResult::Duplicate,
                "takeover did not publish the living destination and burn the old transaction");
        MigrationTransaction checkpoint(f.home, tx.snapshot());
        require(checkpoint.state() == MigrationState::Committed
                && checkpoint.route() == MigrationRoute{alive, 10}
                && f.home.issued(f.key) == 10, "takeover checkpoint did not restore");

        // Refusals: only the live fenced gap is takeable; a replay is
        // idempotent, a committed transaction is a conflict, a dead or same
        // destination is invalid, and a never-fenced transaction is a conflict.
        require(tx.takeover(takeover) == MigrationResult::Duplicate,
                "a replayed takeover was not idempotent");
        MigrationRequest late = takeover; late.id = "takeover-2";
        require(tx.takeover(late) == MigrationResult::Conflict,
                "a takeover ran against a committed transaction");
        MigrationFixture pre;
        auto before = pre.transaction();
        require(before.prepare(pre.request()) == MigrationResult::Applied, "pre-fence takeover prepare failed");
        MigrationRequest unfenced = takeover; unfenced.id = "unfenced";
        require(before.takeover(unfenced) == MigrationResult::Conflict,
                "a takeover ran against a transaction that never fenced");
        MigrationFixture pre2;
        auto gap2 = pre2.transaction();
        require(gap2.prepare(pre2.request()) == MigrationResult::Applied, "invalid takeover prepare failed");
        const auto token2 = gap2.entry("move-1")->token;
        migration_step(gap2, token2, MigrationEvent::SourceDrained);
        migration_step(gap2, token2, MigrationEvent::HomeFence);
        MigrationRequest dead = takeover; dead.id = "dead-target"; dead.destination = pre2.target;
        require(gap2.takeover(dead) == MigrationResult::Invalid,
                "the dead destination was accepted for takeover");
        MigrationRequest same = takeover; same.id = "same-target"; same.destination = pre2.source;
        require(gap2.takeover(same) == MigrationResult::Invalid,
                "the source itself was accepted as the takeover destination");
        MigrationRequest stale = takeover; stale.id = "stale-route";
        stale.expected.owner.scene = "elsewhere";
        require(gap2.takeover(stale) == MigrationResult::Stale,
                "a takeover continued a route that was never the fenced one");
    }
    {
        MigrationFixture f;
        auto tx = f.transaction();
        require(tx.prepare(f.request()) == MigrationResult::Applied, "refusal prepare failed");
        const auto token = tx.entry("move-1")->token;
        MigrationOwner stranger = f.target; stranger.instance = "xs-b";
        require(tx.retire_member(token, stranger) == MigrationResult::Invalid,
                "retirement of a member the transaction does not depend on was accepted");
        MigrationOwner malformed = f.source; malformed.scene.clear();
        require(tx.retire_member(token, malformed) == MigrationResult::Invalid,
                "an unidentifiable member claim was accepted");
        MigrationToken old_generation = token; old_generation.generation = 8;
        require(tx.retire_member(old_generation, f.source) == MigrationResult::Stale,
                "a stale generation claimed a retirement");
        MigrationToken wrong_key = token; wrong_key.key.first = "other";
        require(tx.retire_member(wrong_key, f.source) == MigrationResult::Invalid,
                "a cross-realm retirement claim was accepted");
        migration_step(tx, token, MigrationEvent::Abort);
        migration_step(tx, token, MigrationEvent::Cleanup);
        require(tx.retire_member(token, f.source) == MigrationResult::Conflict
                && tx.state() == MigrationState::Aborted, "retirement changed a terminal transaction");
    }
    std::cout << "migration.retire: pre-fence retirement aborts, fenced retire waits, "
                 "post-fence takeover resumes at gen 10 [PASS]\n";
}
// The migration model on the wire: MLR1 MIG carries the same protocol the
// coordinator keeps in memory, over real connections, behind MLS1 on a tokened
// coordinator, idempotent by request id.
void coordinator_migration_wire() {
    Context c;
    write(c.path("echo.lua"), echo);
    const auto registry = port(), metrics = port(), tokened = port(),
              first = port(), second = port(), third = port(), cross = port();
    c.node("coordinator", registry, {"--metrics-listen", endpoint(metrics)});
    c.node("coordinator", tokened, {"--service-token", "s3cret"});
    c.node("gameServer", first, {"--script", c.path("echo.lua").string(),
        "--coordinator", endpoint(registry), "--scope", "alpha", "--service-name", "gs-a"});
    auto& doomed = c.node("gameServer", second, {"--script", c.path("echo.lua").string(),
        "--coordinator", endpoint(registry), "--scope", "beta", "--service-name", "gs-b"});
    c.node("gameServer", third, {"--script", c.path("echo.lua").string(),
        "--coordinator", endpoint(registry), "--scope", "gamma", "--service-name", "gs-c"});
    auto& cross_member = c.node("crossServer", cross, {"--script", c.path("echo.lua").string(),
        "--coordinator", endpoint(registry), "--scope", "arena", "--service-name", "xs-a"});
    auto& doomed_cross = c.node("crossServer", port(), {"--script", c.path("echo.lua").string(),
        "--coordinator", endpoint(registry), "--scope", "arena", "--service-name", "xs-z"});
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
    // One-shot legs: the coordinator answers and closes, so each question owns
    // a fresh connection; the tokened leg identifies first.
    const auto ask = [&](const std::string& line,
                         const std::source_location& loc = std::source_location::current()) {
        try {
            RawSocket raw(registry);
            send_frame(raw, line);
            return read_frame(raw);
        } catch (const std::exception& error) {
            throw std::runtime_error("L"+std::to_string(loc.line())+" "+line+": "+error.what());
        }
    };
    const auto ask_tokened = [&](const std::string& line,
                                 const std::source_location& loc = std::source_location::current()) {
        try {
            RawSocket raw(tokened);
            send_frame(raw, "MLS1 gameServer gs-a s3cret");
            equal(read_frame(raw), "MLS1 OK");
            send_frame(raw, line);
            return read_frame(raw);
        } catch (const std::exception& error) {
            throw std::runtime_error("L"+std::to_string(loc.line())+" "+line+": "+error.what());
        }
    };
    eventually([&] { return ask("MLR1 QUERY -").find("MLR1 MEMBERS 5") != std::string::npos; },
        5s, "members never reached the registry");
    const std::string source = "realm NS - alpha gameServer gs-a town";
    const std::string target = "cross NS - arena crossServer xs-a room-1";
    // A participant key with no published home facts is EMPTY, not an error.
    equal(ask("MLR1 MIG STATUS NS - home hero"), "MLR1 EMPTY");
    equal(ask("MLR1 MIG PREPARE NS - home hero move-1 8 "+source+" "+target), "MLR1 ERR EMPTY");
    equal(ask("MLR1 MIG OWNER NS - home hero 8 "+source), "MLR1 MIG OWNER OK");
    equal(ask("MLR1 MIG OWNER NS - home hero 8 "+source), "MLR1 MIG OWNER OK"); // idempotent
    equal(ask("MLR1 MIG OWNER NS - home hero 9 "+source), "MLR1 ERR CONFLICT"); // never adopts facts
    equal(ask("MLR1 MIG OWNER NS - home hero 8 realm NS - home crossServer gs-a town"), "MLR1 ERR INVALID");
    equal(ask("MLR1 MIG STATUS NS - home hero"), "MLR1 MIG STATE idle "+source+" 8");
    // A request naming a generation the coordinator never published is STALE
    // while the transaction is idle, and it reserves nothing.
    equal(ask("MLR1 MIG PREPARE NS - home hero stale-1 7 "+source+" "+target), "MLR1 ERR STALE");
    // Preparing reserves the next generation; the same PREPARE replays as the
    // same verdict and never allocates again.
    equal(ask("MLR1 MIG PREPARE NS - home hero move-1 8 "+source+" "+target), "MLR1 MIG PREPARED move-1 9");
    equal(ask("MLR1 MIG PREPARE NS - home hero move-1 8 "+source+" "+target), "MLR1 MIG PREPARED move-1 9");
    equal(ask("MLR1 MIG PREPARE NS - home hero move-1 8 "+source+" realm NS - alpha gameServer gs-a hall"),
        "MLR1 ERR CONFLICT");
    equal(ask("MLR1 MIG PREPARE NS - home hero concurrent-1 9 "+source+" realm NS - alpha gameServer gs-a hall"),
        "MLR1 ERR CONFLICT");
    equal(ask("MLR1 MIG PREPARE NS - home hero junk-1 8 realm NS - alpha gameServer "+source+" "+target),
        "MLR1 ERR INVALID");
    equal(ask("MLR1 MIG STATUS NS - home hero"), "MLR1 MIG STATE move-1 PREPARING open unfenced notready 9");
    // The order gates still hold over the wire, and a skipped step is an error
    // rather than a silent retry.
    equal(ask("MLR1 MIG COMMIT NS - home hero move-1 9"), "MLR1 ERR ORDER");
    equal(ask("MLR1 MIG EVENT NS - home hero move-1 9 READY"), "MLR1 ERR ORDER");
    equal(ask("MLR1 MIG EVENT NS - home hero move-1 9 FENCE"), "MLR1 ERR ORDER");
    equal(ask("MLR1 MIG EVENT NS - home hero move-1 9 DRAINED"), "MLR1 MIG STEP move-1 PREPARING");
    equal(ask("MLR1 MIG EVENT NS - home hero move-1 9 DRAINED"), "MLR1 MIG STEP move-1 PREPARING");
    equal(ask("MLR1 MIG EVENT NS - home hero move-1 9 FENCE"), "MLR1 MIG STEP move-1 PREPARING");
    equal(ask("MLR1 MIG EVENT NS - home hero move-1 9 READY"), "MLR1 MIG STEP move-1 PREPARING");
    equal(ask("MLR1 MIG COMMIT NS - home hero move-1 9"), "MLR1 MIG COMMITTED move-1 "+target+" 9");
    equal(ask("MLR1 MIG COMMIT NS - home hero move-1 9"), "MLR1 MIG COMMITTED move-1 "+target+" 9");
    equal(ask("MLR1 MIG STATUS NS - home hero"), "MLR1 MIG STATE move-1 COMMITTED drained fenced ready 9");
    // The committed route points at the live member, whose ready state is
    // reported by its own heartbeat; wait for it to settle before asserting.
    eventually([&] { return ask("MLR1 ROUTE GET NS - home hero") ==
        "MLR1 ROUTE AT arena crossServer xs-a 127.0.0.1:"+std::to_string(cross)
        +" 0 ready 0/42 room-1 9"; }, 5s, "committed route never reached ready");
    // A terminal replay of the same request re-answers the recorded verdict
    // instead of reserving a newer generation.
    equal(ask("MLR1 MIG PREPARE NS - home hero move-1 8 "+source+" "+target), "MLR1 MIG PREPARED move-1 9");
    // The published route is the new expectation, and the next candidate is
    // strictly newer.
    equal(ask("MLR1 MIG PREPARE NS - home hero move-2 9 "+target+" realm NS - alpha gameServer gs-a hall"),
        "MLR1 MIG PREPARED move-2 10");
    // Abort only schedules the rollback; CLEANUP finishes it, and the replies
    // never claim a terminal word that has not happened yet.
    equal(ask("MLR1 MIG ABORT NS - home hero move-2 10"), "MLR1 MIG ABORTING move-2");
    equal(ask("MLR1 MIG ABORT NS - home hero move-2 10"), "MLR1 MIG ABORTING move-2");
    equal(ask("MLR1 MIG EVENT NS - home hero move-2 10 CLEANUP"), "MLR1 MIG STEP move-2 ABORTED");
    equal(ask("MLR1 MIG ABORT NS - home hero move-2 10"), "MLR1 MIG ABORTED move-2");
    equal(ask("MLR1 MIG STATUS NS - home hero"), "MLR1 MIG STATE move-2 ABORTED open unfenced notready 10");
    eventually([&] { return ask("MLR1 ROUTE GET NS - home hero") ==
        "MLR1 ROUTE AT arena crossServer xs-a 127.0.0.1:"+std::to_string(cross)
        +" 0 ready 0/42 room-1 9"; }, 5s, "aborted route never settled ready");
    // Aborted generations are burned: the next PREPARE gets 11, not 10.
    equal(ask("MLR1 MIG PREPARE NS - home hero move-3 9 "+target+" realm NS - alpha gameServer gs-a hall"),
        "MLR1 MIG PREPARED move-3 11");
    // A retiring member retires the live transaction that depends on it; the
    // coordinator sees the leave from the registry, no message is needed.
    const std::string doomed_owner = "realm NS - beta gameServer gs-b town";
    equal(ask("MLR1 MIG OWNER NS - home hero-2 8 "+doomed_owner), "MLR1 MIG OWNER OK");
    equal(ask("MLR1 MIG PREPARE NS - home hero-2 move-4 8 "+doomed_owner+" realm NS - alpha gameServer gs-a hall"),
        "MLR1 MIG PREPARED move-4 9");
    doomed.signal(SIGTERM);
    eventually([&] { return ask("MLR1 MIG STATUS NS - home hero-2")
        == "MLR1 MIG STATE move-4 ABORTED open unfenced notready 9"; }, 5s,
        "member retirement did not abort the transaction that depended on it");
    equal(ask("MLR1 MIG STATUS NS - home hero-2"), "MLR1 MIG STATE move-4 ABORTED open unfenced notready 9");
    // RETIRE validates the member identity against the migration's own
    // source/destination, not against the registry.
    equal(ask("MLR1 MIG PREPARE NS - home hero-2 move-5 8 "+doomed_owner+" realm NS - alpha gameServer gs-a hall"),
        "MLR1 MIG PREPARED move-5 10");
    equal(ask("MLR1 MIG RETIRE NS - home hero-2 move-5 10 realm NS - alpha gameServer gs-z hall"),
        "MLR1 ERR INVALID");
    // RETIRE matches the transaction's own dependency, so a process that is
    // already gone still retires the migration that claimed it.
    equal(ask("MLR1 MIG RETIRE NS - home hero-2 move-5 10 "+doomed_owner), "MLR1 MIG ABORTED move-5");
    equal(ask("MLR1 MIG PREPARE NS - home hero-2 move-6 8 "+doomed_owner+" realm NS - alpha gameServer gs-a hall"),
        "MLR1 MIG PREPARED move-6 11");
    // A member the transaction does not depend on is not a retirement claim.
    equal(ask("MLR1 MIG RETIRE NS - home hero-2 move-6 11 realm NS - gamma gameServer gs-c hall"),
        "MLR1 ERR INVALID");
    equal(ask("MLR1 MIG RETIRE NS - home hero-2 move-6 11 realm NS - alpha gameServer gs-a hall"),
        "MLR1 MIG ABORTED move-6");
    equal(ask("MLR1 MIG STATUS NS - home hero-2"),
        "MLR1 MIG STATE move-6 ABORTED open unfenced notready 11");
    // After the home fence nothing rolls back: retirement and abort only
    // acknowledge, and recovery keeps waiting for the destination. Every
    // candidate stays newer than the last even though the published route is
    // still the one the aborted migrations never replaced.
    equal(ask("MLR1 MIG PREPARE NS - home hero-2 move-7 8 "+doomed_owner+" realm NS - alpha gameServer gs-a hall"),
        "MLR1 MIG PREPARED move-7 12");
    equal(ask("MLR1 MIG EVENT NS - home hero-2 move-7 12 DRAINED"), "MLR1 MIG STEP move-7 PREPARING");
    equal(ask("MLR1 MIG EVENT NS - home hero-2 move-7 12 FENCE"), "MLR1 MIG STEP move-7 PREPARING");
    equal(ask("MLR1 MIG RETIRE NS - home hero-2 move-7 12 realm NS - alpha gameServer gs-a hall"),
        "MLR1 MIG PENDING move-7");
    equal(ask("MLR1 MIG ABORT NS - home hero-2 move-7 12"), "MLR1 ERR ORDER");
    equal(ask("MLR1 MIG STATUS NS - home hero-2"),
        "MLR1 MIG STATE move-7 PREPARING drained fenced notready 12");
    // The fenced gap has no live owner: an explicit TAKEOVER names the fenced
    // route and a living destination, burns the superseded transaction, and
    // reserves a strictly newer generation. It never revives the old source or
    // the dead destination, and only the new destination's ACK authorizes it.
    const std::string alive = "realm NS - gamma gameServer gs-c town";
    // Refusals against the fenced gap before any takeover: a route other than
    // the fenced one is STALE, the source or move-7's dead destination is
    // INVALID, and a shape miss is INVALID.
    equal(ask("MLR1 MIG TAKEOVER NS - home hero-2 take-2 9 "+doomed_owner+" "+alive),
        "MLR1 ERR STALE");
    equal(ask("MLR1 MIG TAKEOVER NS - home hero-2 take-3 8 "+alive+" "+alive),
        "MLR1 ERR STALE");
    equal(ask("MLR1 MIG TAKEOVER NS - home hero-2 take-4 8 "+doomed_owner+" "+doomed_owner),
        "MLR1 ERR INVALID");
    equal(ask("MLR1 MIG TAKEOVER NS - home hero-2 take-5 8 "+doomed_owner
        +" realm NS - alpha gameServer gs-a hall"), "MLR1 ERR INVALID");
    equal(ask("MLR1 MIG TAKEOVER NS - home hero-2 take-6 8 "+doomed_owner),
        "MLR1 ERR INVALID");
    // The fenced gap has no live owner: an explicit TAKEOVER names the fenced
    // route and a living destination, burns the superseded transaction, and
    // reserves a strictly newer generation. It never revives the old source
    // and only the new destination's ACK authorizes it.
    equal(ask("MLR1 MIG TAKEOVER NS - home hero-2 take-1 8 "+doomed_owner+" "+alive),
        "MLR1 MIG TAKEOVER take-1 13");
    equal(ask("MLR1 MIG TAKEOVER NS - home hero-2 take-1 8 "+doomed_owner+" "+alive),
        "MLR1 MIG TAKEOVER take-1 13"); // idempotent by request id
    equal(ask("MLR1 MIG STATUS NS - home hero-2"),
        "MLR1 MIG STATE take-1 PREPARING drained fenced notready 13");
    // The takeover resumes at the destination ACK and commits onto the living
    // target; the superseded fenced transaction is terminal and cannot win.
    equal(ask("MLR1 MIG EVENT NS - home hero-2 take-1 13 READY"), "MLR1 MIG STEP take-1 PREPARING");
    equal(ask("MLR1 MIG COMMIT NS - home hero-2 take-1 13"), "MLR1 MIG COMMITTED take-1 "+alive+" 13");
    equal(ask("MLR1 MIG STATUS NS - home hero-2"),
        "MLR1 MIG STATE take-1 COMMITTED drained fenced ready 13");
    equal(ask("MLR1 MIG COMMIT NS - home hero-2 move-7 12"), "MLR1 ERR CONFLICT");
    equal(ask("MLR1 MIG TAKEOVER NS - home hero-2 take-7 8 "+doomed_owner+" "+alive),
        "MLR1 ERR CONFLICT");
    // The destination member is live membership, not just a request field: if
    // it leaves between PREPARE and COMMIT the commit cannot publish its route,
    // and the pending verdict leaves the fenced gap explicit.
    const std::string dead_target = "cross NS - arena crossServer xs-z room-1";
    equal(ask("MLR1 MIG OWNER NS - home hero-3 8 "+source), "MLR1 MIG OWNER OK");
    equal(ask("MLR1 MIG PREPARE NS - home hero-3 move-7 8 "+source+" "+dead_target),
        "MLR1 MIG PREPARED move-7 9");
    equal(ask("MLR1 MIG ABORT NS - home hero-3 move-7 9"), "MLR1 MIG ABORTING move-7");
    equal(ask("MLR1 MIG EVENT NS - home hero-3 move-7 9 CLEANUP"),
        "MLR1 MIG STEP move-7 ABORTED");
    equal(ask("MLR1 MIG PREPARE NS - home hero-3 move-8 8 "+source+" "+dead_target),
        "MLR1 MIG PREPARED move-8 10");
    equal(ask("MLR1 MIG EVENT NS - home hero-3 move-8 10 DRAINED"),
        "MLR1 MIG STEP move-8 PREPARING");
    equal(ask("MLR1 MIG EVENT NS - home hero-3 move-8 10 FENCE"),
        "MLR1 MIG STEP move-8 PREPARING");
    equal(ask("MLR1 MIG EVENT NS - home hero-3 move-8 10 READY"),
        "MLR1 MIG STEP move-8 PREPARING");
    doomed_cross.signal(SIGTERM);
    eventually([&] { return ask("MLR1 QUERY crossServer - arena").find("xs-z") == std::string::npos; },
        5s, "retired destination stayed in the registry");
    equal(ask("MLR1 MIG COMMIT NS - home hero-3 move-8 9"), "MLR1 ERR STALE");
    equal(ask("MLR1 MIG COMMIT NS - home hero-3 move-7 9"), "MLR1 ERR CONFLICT");
    equal(ask("MLR1 MIG COMMIT NS - home hero-3 move-8 10"), "MLR1 ERR PENDING");
    equal(ask("MLR1 MIG STATUS NS - home hero-3"),
        "MLR1 MIG STATE move-8 PREPARING drained fenced ready 10");
    equal(ask("MLR1 ROUTE GET NS - home hero-3"), "MLR1 ROUTE EMPTY");
    // The tokened coordinator keeps its own registry, so it only sees this
    // key after its own hand identifies and publishes the facts.
    {
        RawSocket raw(tokened);
        send_frame(raw, "MLR1 MIG STATUS NS - home hero-2");
        equal(read_frame(raw), "MLS1 ERR TOKEN");
    }
    equal(ask_tokened("MLR1 MIG STATUS NS - home hero-2"), "MLR1 EMPTY");
    equal(ask_tokened("MLR1 MIG OWNER NS - home hero-2 8 "+doomed_owner), "MLR1 MIG OWNER OK");
    equal(ask_tokened("MLR1 MIG OWNER NS - home hero-2 9 "+doomed_owner), "MLR1 ERR CONFLICT");
    equal(ask_tokened("MLR1 MIG PREPARE NS - home hero-2 wire-1 8 "+doomed_owner+" "+target),
        "MLR1 MIG PREPARED wire-1 9");
    equal(ask_tokened("MLR1 MIG EVENT NS - home hero-2 wire-1 9 DRAINED"),
        "MLR1 MIG STEP wire-1 PREPARING");
    equal(ask_tokened("MLR1 MIG STATUS NS - home hero-2"),
        "MLR1 MIG STATE wire-1 PREPARING drained unfenced notready 9");
    // The same participant coordinates independently in a named namespace.
    const std::string named_source = "realm NS shop alpha gameServer gs-a town";
    const std::string named_target = "cross NS shop arena crossServer xs-a room-1";
    equal(ask("MLR1 MIG STATUS NS shop home hero"), "MLR1 EMPTY");
    equal(ask("MLR1 MIG OWNER NS shop home hero 8 "+named_source), "MLR1 MIG OWNER OK");
    equal(ask("MLR1 MIG PREPARE NS shop home hero ns-1 8 "+named_source+" "+named_target),
        "MLR1 MIG PREPARED ns-1 9");
    equal(ask("MLR1 MIG STATUS NS shop home hero"),
        "MLR1 MIG STATE ns-1 PREPARING open unfenced notready 9");
    // Shape refusals close the leg with the same words as the registry verbs.
    equal(ask("MLR1 MIG"), "MLR1 ERR UNEXPECTED");
    equal(ask("MLR1 MIG OWNER NS - home hero"), "MLR1 ERR INVALID");
    equal(ask("MLR1 MIG OWNER NS - home hero 8 "+source+" junk"), "MLR1 ERR INVALID");
    equal(ask("MLR1 MIG PREPARE NS - home hero move-1 8 "+source), "MLR1 ERR INVALID");
    equal(ask("MLR1 MIG EVENT NS - home hero move-1 9 JUMP"), "MLR1 ERR INVALID");
    equal(ask("MLR1 MIG STATUS NS - home hero extra"), "MLR1 ERR INVALID");
    equal(ask("MLR1 MIG STATUS NS - "+std::string(260, 'x')+" hero"), "MLR1 ERR INVALID");
    equal(ask("MLR1 MIG PREPARE NS - home hero move-1 8 "+std::string(1000, 'x')), "MLR1 ERR INVALID");
    equal(ask("MLR1 MIG STATUS NS - no.such.key hero"), "MLR1 ERR INVALID");
    std::cout << "migration.wire: OWNER/PREPARE/STEP/COMMIT/ABORT/RETIRE/TAKEOVER/STATUS "
        "over TCP, MLS1-gated, idempotent by request id, retirement observed from "
        "the registry, committed ownership is answerable with scene/generation, "
        "and a retired owner stops being routed [PASS]\n";
}

// The SQLite journal slice: every accepted migration mutation lands in one
// snapshot before the wire answer, so a coordinator restart sees the same
// unresolved transaction, the same consumed generation, and can finish it.
void coordinator_migration_journal() {
    Context c;
    write(c.path("echo.lua"), echo);
    const auto first = port(), second = port();
    const auto journal = c.path("coordinator.sqlite");
    auto& initial = c.node("coordinator", first, {"--journal", journal.string()});
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
    const auto ask = [&](unsigned listen, const std::string& line) {
        RawSocket raw(listen);
        send_frame(raw, line);
        return read_frame(raw);
    };
    const std::string source = "realm NS - alpha gameServer gs-a town";
    const std::string target = "cross NS - arena crossServer xs-a room-1";
    equal(ask(first, "MLR1 MIG OWNER NS - home hero 8 "+source), "MLR1 MIG OWNER OK");
    equal(ask(first, "MLR1 MIG PREPARE NS - home hero move-1 8 "+source+" "+target),
        "MLR1 MIG PREPARED move-1 9");
    equal(ask(first, "MLR1 MIG EVENT NS - home hero move-1 9 DRAINED"), "MLR1 MIG STEP move-1 PREPARING");
    equal(ask(first, "MLR1 MIG EVENT NS - home hero move-1 9 FENCE"), "MLR1 MIG STEP move-1 PREPARING");
    equal(ask(first, "MLR1 MIG STATUS NS - home hero"),
        "MLR1 MIG STATE move-1 PREPARING drained fenced notready 9");
    // Abort/cleanup is a terminal journal row too: the consumed generation
    // must survive the restart even though the route never moved.
    equal(ask(first, "MLR1 MIG OWNER NS - home hero-abort 8 "+source), "MLR1 MIG OWNER OK");
    equal(ask(first, "MLR1 MIG PREPARE NS - home hero-abort abort-1 8 "+source+" "+target),
        "MLR1 MIG PREPARED abort-1 9");
    equal(ask(first, "MLR1 MIG ABORT NS - home hero-abort abort-1 9"), "MLR1 MIG ABORTING abort-1");
    equal(ask(first, "MLR1 MIG EVENT NS - home hero-abort abort-1 9 CLEANUP"),
        "MLR1 MIG STEP abort-1 ABORTED");
    equal(ask(first, "MLR1 MIG STATUS NS - home hero-abort"),
        "MLR1 MIG STATE abort-1 ABORTED open unfenced notready 9");
    initial.signal(SIGTERM);
    require(initial.wait() == 0, "journal coordinator did not stop cleanly");

    c.node("coordinator", second, {"--journal", journal.string()});
    const auto cross_listen = port();
    c.node("crossServer", cross_listen, {"--script", c.path("echo.lua").string(),
        "--coordinator", endpoint(second), "--scope", "arena", "--service-name", "xs-a"});
    // Startup replay has already driven the fenced live transaction to the
    // deterministic recovery state: pre-fence would have rolled back, this one
    // stays pending until the destination repeats its ACK.
    equal(ask(second, "MLR1 MIG STATUS NS - home hero"),
        "MLR1 MIG STATE move-1 RECOVERING drained fenced notready 9");
    // A prepare replay after restart repeats the original verdict and never
    // consumes another generation, then the fenced transaction resumes forward.
    equal(ask(second, "MLR1 MIG PREPARE NS - home hero move-1 8 "+source+" "+target),
        "MLR1 MIG PREPARED move-1 9");
    equal(ask(second, "MLR1 MIG STATUS NS - home hero-abort"),
        "MLR1 MIG STATE abort-1 ABORTED open unfenced notready 9");
    equal(ask(second, "MLR1 MIG PREPARE NS - home hero-abort abort-2 8 "+source+" "+target),
        "MLR1 MIG PREPARED abort-2 10");
    equal(ask(second, "MLR1 MIG EVENT NS - home hero move-1 9 READY"), "MLR1 MIG STEP move-1 RECOVERING");
    eventually([&] {
        return ask(second, "MLR1 QUERY crossServer - arena").find("xs-a") != std::string::npos;
    }, 5s, "restored destination never registered");
    equal(ask(second, "MLR1 MIG COMMIT NS - home hero move-1 9"),
        "MLR1 MIG COMMITTED move-1 "+target+" 9");
    equal(ask(second, "MLR1 MIG STATUS NS - home hero"),
        "MLR1 MIG STATE move-1 COMMITTED drained fenced ready 9");
    // The committed ownership keeps its scene across the restart: even while
    // the transaction is still open the route answers the published scene.
    eventually([&] {
        return ask(second, "MLR1 ROUTE GET NS - home hero") ==
            "MLR1 ROUTE AT arena crossServer xs-a 127.0.0.1:"+std::to_string(cross_listen)+
            " 0 ready 0/42 room-1 9";
    }, 5s, "restored committed route lost its scene");
    // The restored authority resumes from the consumed generation: the next
    // request against the committed route allocates 10, never 9.
    const std::string next_target = "realm NS - beta gameServer gs-c hall";
    equal(ask(second, "MLR1 MIG PREPARE NS - home hero move-2 9 "+target+" "+next_target),
        "MLR1 MIG PREPARED move-2 10");
    equal(ask(second, "MLR1 MIG STATUS NS - home hero"),
        "MLR1 MIG STATE move-2 PREPARING open unfenced notready 10");
    std::cout << "migration.journal: recoverable SQLite snapshot survives restart, "
        "startup replay is deterministic, replay is idempotent and generations stay consumed [PASS]\n";
}

void coordinator_migration_journal_recovery() {
    // Cut before the persisted row becomes a terminal commit. Startup must
    // turn every snapshot into the same state a trusted checkpoint test
    // produces: unfenced rolls back, fenced waits for destination ACK, and
    // destination ACK + recover completes forward without inventing a commit.
    for (unsigned cut = 0; cut < 5; ++cut) {
        Context c;
        const auto path = c.path("migration-recovery.sqlite");
        MigrationFixture f;
        std::optional<MigrationToken> token;
        {
            MigrationJournal journal(path.string());
            auto crashed = f.transaction();
            require(crashed.begin_request(), "crash fixture lost its source handler");
            require(crashed.prepare(f.request()) == MigrationResult::Applied, "crash prepare failed");
            token = crashed.entry("move-1")->token;
            if (cut >= 1 && cut <= 4) {
                require(crashed.finish_request(), "crash fixture could not release its handler");
                migration_step(crashed, *token, MigrationEvent::SourceDrained);
            }
            if (cut >= 2 && cut <= 4) migration_step(crashed, *token, MigrationEvent::HomeFence);
            if (cut == 3) migration_step(crashed, *token, MigrationEvent::DestinationReady);
            if (cut == 4) {
                migration_step(crashed, *token, MigrationEvent::DestinationReady);
                migration_step(crashed, *token, MigrationEvent::Commit);
            }
            journal.save({"ns", f.key, crashed.snapshot()});
        }
        MigrationJournal restored(path.string());
        const auto records = restored.restore();
        require(records.size() == 1 && records.front().namespace_ == "ns"
                && records.front().snapshot.key == f.key, "SQLite recovery lost the journal row");
        // Registry seeds its authority from the snapshot's route/journal max,
        // never from a fresh reservation: the journal already consumed the
        // generation that the crashed PREPARE allocated.
        std::uint64_t high = records.front().snapshot.route.generation;
        for (const auto& [id, row] : records.front().snapshot.journal) {
            (void)id;
            high = std::max(high, row.token.generation);
        }
        HomeGenerationAuthority authority;
        require(authority.seed(f.key, high), "recovery authority seed failed");
        require(authority.issued(f.key) == high, "recovery authority did not retain consumed generation");
        MigrationTransaction recovered(authority, records.front().snapshot);
        const auto result = recovered.recover();
        if (cut < 2) {
            require(result == MigrationResult::Applied && recovered.state() == MigrationState::Aborted
                    && recovered.route() == f.request().expected
                    && recovered.write_allowed(f.source, 8)
                    && !recovered.entry("move-1")->destination_staged, "unfenced recovery did not roll back");
            const auto* aborted = recovered.entry("move-1");
            require(aborted && aborted->state == MigrationState::Aborted && aborted->source_drained == (cut == 1)
                    && !aborted->home_fenced && !aborted->destination_ready && !aborted->destination_staged,
                "aborted recovery journal row mismatch");
            require(recovered.prepare(f.request("move-2")) == MigrationResult::Applied
                    && recovered.entry("move-2")->token.generation == 10, "aborted recovery reused a generation");
        } else if (cut == 2) {
            require(result == MigrationResult::Pending && recovered.state() == MigrationState::Recovering
                    && !recovered.write_allowed(f.source, 8)
                    && !recovered.write_allowed(f.target, 9), "fenced recovery invented a grant");
            const auto* pending = recovered.entry("move-1");
            require(pending && pending->state == MigrationState::Recovering && pending->source_drained
                    && pending->home_fenced && !pending->destination_ready && pending->destination_staged,
                "pending recovery journal row mismatch");
            require(MigrationTransaction(authority, recovered.snapshot()).recover() == MigrationResult::Pending,
                "pending recovery replay changed its decision");
            migration_step(recovered, *token, MigrationEvent::DestinationReady);
            migration_step(recovered, *token, MigrationEvent::Recover);
        } else {
            const bool fresh_commit = cut == 3;
            require((fresh_commit ? result == MigrationResult::Applied : result == MigrationResult::Duplicate)
                    && recovered.state() == MigrationState::Committed
                    && recovered.route() == MigrationRoute{f.target, 9}, "committed recovery replay changed outcome");
            const auto* committed = recovered.entry("move-1");
            require(committed && committed->state == MigrationState::Committed && committed->source_drained
                    && committed->home_fenced && committed->destination_ready && !committed->destination_staged,
                "committed recovery journal row mismatch");
            require(recovered.prepare(f.request("move-2")) == MigrationResult::Stale
                    && recovered.prepare({f.key, "move-2", recovered.route(), f.source}) == MigrationResult::Applied
                    && recovered.entry("move-2")->token.generation == 10, "committed recovery reused generation");
        }
        const bool reserved_next = cut < 2 || cut >= 3;
        require(authority.issued(f.key) == (reserved_next ? 10 : 9), "recovery lost the consumed generation");
    }

    // Journal validation stays fail-closed: a schema-version or key mismatch
    // must refuse startup instead of silently replaying unrelated bytes.
    {
        Context c;
        const auto path = c.path("migration-bad-version.sqlite");
        {
            MigrationJournal journal(path.string());
            MigrationFixture f;
            auto transaction = f.transaction();
            require(transaction.prepare(f.request()) == MigrationResult::Applied, "bad fixture prepare failed");
            journal.save({"ns", f.key, transaction.snapshot()});
        }
        sqlite3* raw = nullptr;
        require(sqlite3_open(path.string().c_str(), &raw) == SQLITE_OK, "cannot open journal for validation");
        require(sqlite3_exec(raw, "UPDATE migration_journal SET version=0", nullptr, nullptr, nullptr) == SQLITE_OK,
            "cannot corrupt journal version");
        sqlite3_close(raw);
        MigrationJournal corrupt(path.string());
        must_fail([&] { (void)corrupt.restore(); });
    }
    std::cout << "migration.journal.recovery: crash cuts replay deterministically, fenced waits, "
        "terminal/foreign generations are safe and corrupt versions fail closed [PASS]\n";
}

void coordinator_migration_authority() {
    // Model: the final cursor is a durable high-water mark; a generation at or
    // below it is consumed (the single-writer tombstone) and never reissued.
    {
        MigrationFixture f;
        require(f.home.issued(f.key) == 8 && f.home.allocated(f.key, 8) && f.home.allocated(f.key, 1)
                && !f.home.allocated(f.key, 9), "authority cursor did not mark consumed generations");
        auto tx = f.transaction();
        require(tx.prepare(f.request()) == MigrationResult::Applied, "authority prepare failed");
        require(tx.entry("move-1")->token.generation == 9, "prepare did not allocate the next generation");
        require(tx.apply(tx.entry("move-1")->token, MigrationEvent::Abort) == MigrationResult::Applied,
            "authority abort failed");
        require(tx.apply(tx.entry("move-1")->token, MigrationEvent::Cleanup) == MigrationResult::Applied,
            "authority abort cleanup failed");
        require(f.home.issued(f.key) == 9 && f.home.allocated(f.key, 9),
            "an abort rolled back the authority cursor");
        require(tx.prepare(f.request("move-2")) == MigrationResult::Applied
                && tx.entry("move-2")->token.generation == 10, "an aborted generation was reused");
    }
    // Store: snapshot + authority cursor commit together. A restart sees the
    // durable cursor even when the consumed generation only lives in a
    // terminal journal row, and the next prepare uses the strictly next slot.
    {
        Context c;
        const auto path = c.path("authority.sqlite");
        MigrationFixture f;
        std::uint64_t reserved = 0;
        {
            MigrationJournal journal(path.string());
            auto tx = f.transaction();
            require(tx.prepare(f.request()) == MigrationResult::Applied, "store prepare failed");
            reserved = tx.entry("move-1")->token.generation;
            require(tx.apply(tx.entry("move-1")->token, MigrationEvent::Abort) == MigrationResult::Applied,
                "store abort failed");
            journal.save({"ns", f.key, tx.snapshot()}, reserved);
        }
        MigrationJournal restored(path.string());
        const auto records = restored.restore();
        require(records.size() == 1, "authority store lost the transaction row");
        const auto authorities = restored.restore_authorities();
        require(authorities.size() == 1 && authorities.front().namespace_ == "ns"
                && authorities.front().key == f.key && authorities.front().cursor == reserved,
            "authority store lost or understated the durable final cursor");
        HomeGenerationAuthority authority;
        HomeGenerationAuthority::View view;
        view.cursor.emplace(f.key, authorities.front().cursor);
        authority.restore(view);
        require(authority.issued(f.key) == reserved, "authority restore did not retain the final cursor");
        MigrationTransaction recovered(authority, records.front().snapshot);
        require(recovered.recover() == MigrationResult::Applied && recovered.state() == MigrationState::Aborted
                && recovered.write_allowed(f.source, 8), "authority recovery did not restore the source");
        require(recovered.prepare(f.request("move-2")) == MigrationResult::Applied
                && recovered.entry("move-2")->token.generation == reserved + 1,
            "authority restore reused a consumed generation");
    }
    // A legacy single-row save still publishes the high-water cursor.
    {
        Context c;
        const auto path = c.path("authority-legacy.sqlite");
        MigrationFixture f;
        {
            MigrationJournal journal(path.string());
            auto tx = f.transaction();
            require(tx.prepare(f.request()) == MigrationResult::Applied, "legacy prepare failed");
            journal.save({"ns", f.key, tx.snapshot()});
        }
        MigrationJournal restored(path.string());
        const auto authorities = restored.restore_authorities();
        require(authorities.size() == 1 && authorities.front().cursor == 9,
            "legacy single-row save did not publish the high-water cursor");
    }
    // Fail-closed: a cursor trailing the snapshot rows, or a corrupted
    // authority row, must refuse rather than re-derive and reuse.
    {
        Context c;
        const auto path = c.path("authority-corrupt.sqlite");
        MigrationFixture f;
        {
            MigrationJournal journal(path.string());
            auto tx = f.transaction();
            require(tx.prepare(f.request()) == MigrationResult::Applied, "corrupt fixture prepare failed");
            must_fail([&] { journal.save({"ns", f.key, tx.snapshot()}, 8); }); // cursor 8 < reserved 9
            journal.save({"ns", f.key, tx.snapshot()}, 9);
        }
        sqlite3* raw = nullptr;
        require(sqlite3_open(path.string().c_str(), &raw) == SQLITE_OK, "cannot open journal for validation");
        require(sqlite3_exec(raw, "UPDATE home_authority SET cursor=0", nullptr, nullptr, nullptr) == SQLITE_OK,
            "cannot corrupt authority cursor");
        sqlite3_close(raw);
        MigrationJournal corrupt(path.string());
        must_fail([&] { (void)corrupt.restore_authorities(); });
    }
    std::cout << "migration.authority: durable home final cursor/tombstone survives restart, "
        "consumed generations are never reissued and corruption fails closed [PASS]\n";
}

void coordinator_placement_schema() {
    Context c;
    const auto journal = c.path("placement-v1.sqlite");
    {
        sqlite3* raw = nullptr;
        require(sqlite3_open(journal.string().c_str(), &raw) == SQLITE_OK,
                "cannot create placement v1 journal");
        require(sqlite3_exec(raw,
                "CREATE TABLE placement ("
                "version INTEGER NOT NULL, namespace TEXT NOT NULL, scope TEXT NOT NULL,"
                "realm TEXT NOT NULL, character TEXT NOT NULL, role TEXT NOT NULL,"
                "instance TEXT NOT NULL, generation INTEGER NOT NULL,"
                "PRIMARY KEY(namespace, realm, character));"
                "INSERT INTO placement VALUES (1, '', 'alpha', 'mist', 'eve',"
                " 'gameServer', 'gs-a', 7);"
                "PRAGMA user_version=1", nullptr, nullptr, nullptr) == SQLITE_OK,
                "cannot seed placement v1 journal");
        sqlite3_close(raw);
    }
    {
        MigrationJournal migrated(journal.string());
        const auto records = migrated.restore_placements();
        require(records.size() == 1, "placement migration lost the v1 row");
        require(records.front().scene == "-", "v1 placement did not default its scene");
        auto record = records.front();
        record.scene = "town";
        migrated.save_placement(record);
    }
    {
        sqlite3* raw = nullptr;
        require(sqlite3_open(journal.string().c_str(), &raw) == SQLITE_OK,
                "cannot reopen placement migration journal");
        sqlite3_stmt* statement = nullptr;
        require(sqlite3_prepare_v2(raw, "PRAGMA user_version", -1, &statement, nullptr) == SQLITE_OK,
                "cannot read placement schema version");
        require(sqlite3_step(statement) == SQLITE_ROW, "placement schema version row missing");
        require(sqlite3_column_int(statement, 0) == 2, "placement schema was not upgraded");
        sqlite3_finalize(statement);
        require(sqlite3_prepare_v2(raw, "SELECT scene FROM placement", -1, &statement, nullptr) == SQLITE_OK,
                "cannot read placement scene column");
        require(sqlite3_step(statement) == SQLITE_ROW && std::string(
                    reinterpret_cast<const char*>(sqlite3_column_text(statement, 0))) == "town",
                "placement scene did not survive the migration");
        sqlite3_finalize(statement);
        sqlite3_close(raw);
    }
    std::cout << "placement.schema: v1 journal migrates in place and preserves scene [PASS]\n";
}

// Durable placement: a coordinator restart keeps the character placement
// record, so a login re-query does not start the chain from scratch. The
// restored route answers EMPTY until the hosting member re-registers its
// control leg (routes never point at a dead host), then answers the live
// member again with the same placement.
void coordinator_placement_journal() {
    Context c;
    write(c.path("echo.lua"), echo);
    const auto first = port(), second = port(), metrics = port();
    const auto journal = c.path("coordinator.sqlite");
    auto& initial = c.node("coordinator", first, {"--journal", journal.string(),
        "--metrics-listen", endpoint(metrics)});
    c.node("gameServer", port(), {"--script", c.path("echo.lua").string(),
        "--coordinator", endpoint(first), "--scope", "alpha", "--service-name", "gs-a"});
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
    const auto ask = [&](unsigned listen, const std::string& line) {
        RawSocket raw(listen);
        send_frame(raw, line);
        return read_frame(raw);
    };
    eventually([&] {
        return ask(first, "MLR1 QUERY gameServer - alpha").find("gs-a") != std::string::npos;
    }, 5s, "member never registered");
    equal(ask(first, "MLR1 ROUTE SET NS - alpha mist eve gameServer gs-a"), "MLR1 ROUTE OK");
    const auto live = ask(first, "MLR1 ROUTE GET NS - mist eve");
    require(live.starts_with("MLR1 ROUTE AT alpha gameServer gs-a "), "live route wrong shape: "+live);
    require(live.ends_with(" - 0"), "live route lost the absent generation sentinel: "+live);
    equal(ask(first, "MLR1 ROUTE SET NS - alpha mist mal gameServer gs-a 7 town"), "MLR1 ROUTE OK");
    require(ask(first, "MLR1 ROUTE GET NS - mist mal").ends_with(" town 7"),
            "explicit generation route was not recorded");
    // Crash, not graceful stop: a coordinator restart must recover the
    // placement from the durable journal even though the member control leg
    // died with the process (no clean retirement ran).
    initial.signal(SIGKILL);
    require(initial.wait() != 0, "placement journal coordinator should not stop cleanly on a crash");

    c.node("coordinator", second, {"--journal", journal.string()});
    // Before the member re-registers, the restored placement is dormant: it
    // must not invent a live host, so GET honestly answers EMPTY.
    equal(ask(second, "MLR1 ROUTE GET NS - mist eve"), "MLR1 ROUTE EMPTY");
    equal(ask(second, "MLR1 ROUTE GET NS - mist mal"), "MLR1 ROUTE EMPTY");
    c.node("gameServer", port(), {"--script", c.path("echo.lua").string(),
        "--coordinator", endpoint(second), "--scope", "alpha", "--service-name", "gs-a"});
    eventually([&] {
        return ask(second, "MLR1 ROUTE GET NS - mist eve").starts_with(
            "MLR1 ROUTE AT alpha gameServer gs-a ");
    }, 5s, "restored placement never answered for the re-registered host");
    const auto restored = ask(second, "MLR1 ROUTE GET NS - mist eve");
    require(restored != "MLR1 ROUTE EMPTY", "restored placement lost the host");
    require(restored.ends_with(" - 0"), "restored placement lost the absent generation sentinel: "+restored);
    require(ask(second, "MLR1 ROUTE GET NS - mist mal").ends_with(" town 7"),
            "restored placement lost the explicit scene/generation baseline");
    std::cout << "placement.journal: coordinator restart retains character placement, "
        "including the no-generation sentinel, dormant until the host re-registers, "
        "then answers the live member again [PASS]\n";
}

// The complete four-step migration handoff across real processes: the source
// host stops accepting the entity, home data freezes the writer grant
// (MLD1 FENCE), the destination loads the frozen snapshot and confirms, then
// the coordinator compares the old value and publishes the new route. The
// old generation stays tombstoned at data so a late source write is refused,
// while the strictly newer generation resumes from the frozen cursor.
void coordinator_migration_fourstep() {
    Context c;
    write(c.path("echo.lua"), echo);
    const auto registry = port(), data_port = port(), gs_port = port(), xs_port = port();
    const auto journal = c.path("fourstep.sqlite");
    c.node("coordinator", registry, {"--journal", journal.string()});
    c.node("gameServer", gs_port, {"--script", c.path("echo.lua").string(),
        "--coordinator", endpoint(registry), "--scope", "alpha", "--service-name", "gs-a"});
    c.node("crossServer", xs_port, {"--script", c.path("echo.lua").string(),
        "--coordinator", endpoint(registry), "--scope", "arena", "--service-name", "xs-a"});
    c.node("data", data_port, {"--script", c.path("echo.lua").string(),
        "--journal", (c.path("records.sqlite")).string()});
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
    const auto ask = [&](unsigned listen, const std::string& line) {
        RawSocket raw(listen);
        send_frame(raw, line);
        return read_frame(raw);
    };
    const auto ask_data = [&](const std::string& line) { return ask(data_port, line); };
    // Step 0: the source host (gameServer gs-a, gen 8) is the live writer; it
    // records two committed changes at home data.
    equal(ask_data("MLD1 SUBMIT home hero gen 8 seq 1 hp=10 mp=5"),
        "MLD1 ACK durable home hero gen 8 seq 1");
    equal(ask_data("MLD1 SUBMIT home hero gen 8 seq 2 hp=12"),
        "MLD1 ACK durable home hero gen 8 seq 2");
    equal(ask_data("MLD1 LOAD home hero"), "MLD1 STATE gen 8 seq 2 hp=12");
    // The source and destination register as live members.
    eventually([&] {
        return ask(registry, "MLR1 QUERY gameServer - alpha").find("gs-a") != std::string::npos
            && ask(registry, "MLR1 QUERY crossServer - arena").find("xs-a") != std::string::npos;
    }, 5s, "migration members never registered");
    const std::string source = "realm NS - alpha gameServer gs-a town";
    const std::string target = "cross NS - arena crossServer xs-a room-1";
    // Step 1 (source stop) + step 2 (data freeze): the coordinator learns the
    // source is stopping, and home data freezes the writer grant at gen 8.
    equal(ask(registry, "MLR1 MIG OWNER NS - home hero 8 "+source), "MLR1 MIG OWNER OK");
    equal(ask(registry, "MLR1 MIG PREPARE NS - home hero move-1 8 "+source+" "+target),
        "MLR1 MIG PREPARED move-1 9");
    equal(ask(registry, "MLR1 MIG EVENT NS - home hero move-1 9 DRAINED"),
        "MLR1 MIG STEP move-1 PREPARING");
    equal(ask_data("MLD1 FENCE home hero gen 8"), "MLD1 FENCED gen 8 seq 2");
    equal(ask(registry, "MLR1 MIG EVENT NS - home hero move-1 9 FENCE"),
        "MLR1 MIG STEP move-1 PREPARING");
    // Step 3 (destination load + confirm): the target loads the frozen
    // snapshot from home data, then acknowledges it is ready.
    equal(ask_data("MLD1 LOAD home hero"), "MLD1 STATE gen 8 seq 2 hp=12");
    equal(ask(registry, "MLR1 MIG EVENT NS - home hero move-1 9 READY"),
        "MLR1 MIG STEP move-1 PREPARING");
    // Step 4 (coordinator compares old value, publishes the new route): the
    // committed route now names the destination with generation 9.
    equal(ask(registry, "MLR1 MIG COMMIT NS - home hero move-1 9"),
        "MLR1 MIG COMMITTED move-1 "+target+" 9");
    eventually([&] {
        const auto route = ask(registry, "MLR1 ROUTE GET NS - home hero");
        return route.starts_with("MLR1 ROUTE AT arena crossServer xs-a ");
    }, 5s, "committed route never published to the destination");
    const auto published = ask(registry, "MLR1 ROUTE GET NS - home hero");
    require(published.ends_with(" room-1 9"), "published route lost scene/generation: "+published);
    // The old writer stays tombstoned: a late gen-8 advance is refused, and
    // the frozen cursor resend still dedups. Only gen 9 resumes from seq 2.
    equal(ask_data("MLD1 SUBMIT home hero gen 8 seq 3 late"), "MLD1 ERR STALE gen 8");
    equal(ask_data("MLD1 SUBMIT home hero gen 9 seq 3 hp=20"),
        "MLD1 ACK durable home hero gen 9 seq 3");
    equal(ask_data("MLD1 LOAD home hero"), "MLD1 STATE gen 9 seq 3 hp=20");
    std::cout << "migration.fourstep: source stop -> data freeze -> destination load "
        "and confirm -> coordinator compare-old publish; old writer tombstoned, "
        "newer generation resumes from the frozen cursor [PASS]\n";
}

}
