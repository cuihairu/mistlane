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
}
}
