// SPDX-License-Identifier: Apache-2.0
#include "test_common.hpp"
namespace mistlane::test {
void config_file() {
    Context c;
    write(c.path("echo.lua"), echo);
    const auto config = c.path("gameServer.env");
    const auto listen = port();
    write(config, "# mistlane-config 1\nROLE=gameServer\nLISTEN=" + endpoint(listen) +
        "\nSCRIPT=echo.lua\nTRANSPORT=tcp\nMAX_CONNECTIONS=5\n");
    {
        auto& node = c.spawn({self().string(), "--config", config.string()});
        node.ready("ready gameServer " + endpoint(listen));
        require(node.output().find("session admission limit 5") != std::string::npos,
            "config max_connections missing: " + node.output());
        equal(tcp(listen)->request("hello"), "hello");
        node.signal(SIGTERM);
        require(node.wait() == 0, "config node should stop cleanly");
    }
    {
        // A version header is required so a random shell env file is not consumed silently.
        write(config, "ROLE=gameServer\nLISTEN=" + endpoint(listen) + "\n");
        auto& bad = c.spawn({self().string(), "--config", config.string()});
        require(bad.wait() == 1, "missing version line must fail");
        require(bad.output().find("mistlane-config 1") != std::string::npos,
            "version error missing: " + bad.output());
    }
    {
        write(config, "# mistlane-config 1\nNOPE=1\n");
        auto& bad = c.spawn({self().string(), "--config", config.string()});
        require(bad.wait() == 1, "unknown key must fail");
        require(bad.output().find("unknown key NOPE") != std::string::npos,
            "unknown key error missing: " + bad.output());
    }
    {
        write(config, "# mistlane-config 1\nMAX_CONNECTIONS=5\nMAX_CONNECTIONS=6\n");
        auto& bad = c.spawn({self().string(), "--config", config.string()});
        require(bad.wait() == 1, "duplicate key must fail");
        require(bad.output().find("duplicate key MAX_CONNECTIONS") != std::string::npos,
            "duplicate key error missing: " + bad.output());
    }
    {
        // Environment beats the file; an explicit flag beats the environment.
        write(config, "# mistlane-config 1\nROLE=gameServer\nLISTEN=" + endpoint(listen) +
            "\nSCRIPT=echo.lua\nTRANSPORT=tcp\nMAX_CONNECTIONS=5\n");
        ScopedEnv around("MISTLANE_MAX_CONNECTIONS", "7");
        auto& node = c.spawn({self().string(), "--config", config.string()});
        node.ready("ready gameServer " + endpoint(listen));
        require(node.output().find("session admission limit 7") != std::string::npos,
            "environment override missing: " + node.output());
        node.signal(SIGTERM);
        require(node.wait() == 0, "overridden node should stop cleanly");
    }
    {
        auto& node = c.spawn({self().string(), "--config", config.string(), "--max-connections", "3"});
        node.ready("ready gameServer " + endpoint(listen));
        require(node.output().find("session admission limit 3") != std::string::npos,
            "flag override missing: " + node.output());
        node.signal(SIGTERM);
        require(node.wait() == 0, "flag node should stop cleanly");
    }
    {
        // ROOT keeps scaffold-style project-root-relative entries readable.
        const auto root = c.path("app");
        fs::create_directory(root);
        write(root / "echo.lua", echo);
        const auto rooted_port = port();
        write(c.path("rooted.env"), "# mistlane-config 1\nROOT=app\nROLE=gameServer\nLISTEN=" +
            endpoint(rooted_port) + "\nSCRIPT=echo.lua\n");
        auto& node = c.spawn({self().string(), "--config", c.path("rooted.env").string()});
        node.ready("ready gameServer " + endpoint(rooted_port));
        equal(tcp(rooted_port)->request("ping"), "ping");
        node.signal(SIGTERM);
        require(node.wait() == 0, "rooted node should stop cleanly");
    }
    {
        const auto metrics = port(), probe = port();
        write(c.path("diag.env"), "# mistlane-config 1\nROLE=gameServer\nLISTEN=" + endpoint(port()) +
            "\nSCRIPT=echo.lua\nMETRICS_LISTEN=" + endpoint(metrics) + "\nPROBE_LISTEN=" + endpoint(probe) + "\n");
        auto& node = c.spawn({self().string(), "--config", c.path("diag.env").string()});
        node.ready("ready gameServer");
        { RawSocket raw(probe); raw.send("PROBE1 cf liveness\n"); equal(raw.receive_line(), "PROBE1 cf ok alive"); }
        const auto scrape = http("http://"+endpoint(metrics)+"/metrics");
        require(scrape.status == 200 && scrape.body.find("mistlane_messages_total") != std::string::npos,
            "config metrics listener missing");
        node.signal(SIGTERM);
        require(node.wait() == 0, "diagnostics node should stop cleanly");
    }
    {
        // A wrong version comment is refused exactly like a missing one, and
        // a value line without '=' names its line.
        write(config, "# mistlane-config 2\n");
        auto& wrong = c.spawn({self().string(), "--config", config.string()});
        require(wrong.wait() == 1, "wrong version comment must fail");
        require(wrong.output().find("mistlane-config 1") != std::string::npos,
            "wrong version error missing: " + wrong.output());
        write(config, "# mistlane-config 1\nBROKEN\n");
        auto& broken = c.spawn({self().string(), "--config", config.string()});
        require(broken.wait() == 1, "valueless key must fail");
        require(broken.output().find("expected KEY=VALUE") != std::string::npos,
            "KEY=VALUE error missing: " + broken.output());
    }
    {
        // List entries split on commas and drop empty items; the flags expand
        // fully before validation refuses the bad transport with one reason.
        write(config, "# mistlane-config 1\nROLE=gateway\nUPSTREAMS=127.0.0.1:1,,127.0.0.1:2\nTRANSPORT=quic\n");
        auto& bad = c.spawn({self().string(), "--config", config.string()});
        require(bad.wait() == 1, "expanded comma list must fail on bad transport");
        require(bad.output().find("unknown transport") != std::string::npos,
            "expanded list error missing: " + bad.output());
    }
    {
        // Every command-line misconfiguration exits nonzero with its named
        // reason before any listener opens.
        using Args = std::vector<std::string>;
        const std::vector<std::pair<Args, const char*>> probes = {
            {{"--nope", "1"}, "unknown option: --nope"},
            {{"--nope"}, "missing value for --nope"},
            {{"--role", "mapper"}, "unknown role: mapper"},
            {{"--listen"}, "missing value for --listen"},
            {{"--role", "gameServer", "--routing", "cross"}, "--routing applies to gateway only"},
            {{"--role", "gateway", "--listen", endpoint(port()), "--upstream", endpoint(port()),
                "--routing", "balanced"}, "unknown routing policy: balanced"},
            {{"--role", "gateway", "--listen", endpoint(port()), "--upstream", endpoint(port()),
                "--transport", "quic"}, "unknown transport"},
            {{"--role", "gateway", "--listen", endpoint(port()), "--upstream", endpoint(port()),
                "--transport", "wss"}, "wss/https require --cert and --key"},
            {{"--role", "gateway", "--listen", endpoint(port()), "--upstream", endpoint(port()),
                "--journal", "x.db"}, "--journal applies to coordinator, data or apiServer"},
            {{"--role", "gateway", "--listen", endpoint(port()), "--upstream", endpoint(port()),
                "--contract", "x.def"}, "--contract applies to gameServer/crossServer only"},
            {{"--role", "gateway", "--listen", endpoint(port()), "--upstream", endpoint(port()),
                "--data", endpoint(port())}, "--data applies to crossServer only"},
            {{"--role", "gateway", "--listen", endpoint(port()), "--upstream", endpoint(port()),
                "--overload-ratio", "2"}, "overload ratio must be in (0,1]"},
            {{"--role", "gateway", "--upstream-multiplex", "maybe"}, "expected on|off for maybe"},
            {{"--role", "gameServer", "--script", "x.lua", "--upstream-token", "t"},
                "--upstream-token applies to gateway or coordinator members"},
            {{"--role", "gameServer", "--script", "x.lua", "--coordinator", endpoint(port()),
                "--scope", "room1", "--upstream-token", "t", "--upstream-multiplex", "on"},
                "--upstream-multiplex applies to gateway only"},
            {{"--role", "gateway", "--listen", endpoint(port()), "--upstream", endpoint(port()),
                "--routing", "cross", "--upstream-multiplex", "on"},
                "--upstream-multiplex does not apply to cross routing"},
            {{"--role", "gateway", "--upstream", endpoint(port()),
                "--listener", "quic://127.0.0.1:" + std::to_string(port())},
                "unknown listener protocol: quic"},
            {{"--role", "gateway", "--upstream", endpoint(port()),
                "--listener", "tcp://127.0.0.1:" + std::to_string(port()) + "?bogus=1"},
                "unsupported listener query field: bogus=1"},
            {{"--role", "gateway", "--upstream", endpoint(port()),
                "--listener", "tcp://127.0.0.1:" + std::to_string(port()) + "?cert=only"},
                "listener certificates need both cert= and key="},
            {{"--role", "gateway", "--upstream", endpoint(port()),
                "--listener", "wss://127.0.0.1:" + std::to_string(port())},
                "wss/https listeners require --cert and --key"},
            {{"--role", "gateway", "--listen", endpoint(port()), "--upstream", endpoint(port()),
                "--crashpad-handler", "x"},
                "Crashpad requires both --crashpad-handler and --crashpad-database"},
            {{"--role", "gateway", "--listen", endpoint(port()), "--upstream", endpoint(port()),
                "--routing", "cross", "--transport", "ws"},
                "cross routing and crossServer require internal TCP transport"},
        };
        for (const auto& [args, reason] : probes) {
            Args full{self().string()};
            full.insert(full.end(), args.begin(), args.end());
            auto& bad = c.spawn(full);
            require(bad.wait() == 1 && bad.output().find(reason) != std::string::npos,
                std::string("validation probe lacked its reason: ") + reason + "\n" + bad.output());
        }
        // Certificates on a plaintext transport are refused in the flag chain.
        auto& plain = c.spawn({self().string(), "--role", "gateway", "--listen", endpoint(port()),
            "--upstream", endpoint(port()), "--cert", "a.pem", "--key", "b.pem"});
        require(plain.wait() == 1 && plain.output().find("cert/key only apply to wss/https") != std::string::npos,
            "plaintext cert acceptance missing:\n" + plain.output());
        // The help text exits cleanly without touching the network.
        require(c.command({self().string(), "--help"}) == 0, "help exit failed");
    }
    {
        // The off state is a value, not a typo: an anonymous gateway accepts
        // it, starts against a live upstream and stops cleanly.
        const auto game = port(), gate = port();
        c.node("gameServer", game, {"--script", c.path("echo.lua").string()});
        auto& node = c.spawn({self().string(), "--role", "gateway", "--listen", endpoint(gate),
            "--upstream", endpoint(game), "--upstream-multiplex", "off"});
        node.ready("ready gateway " + endpoint(gate));
        node.signal(SIGTERM);
        require(node.wait() == 0, "multiplex off node should stop cleanly");
    }
}
}
