// SPDX-License-Identifier: Apache-2.0
#include "mistlane/node.hpp"
#include "mistlane/scaffold.hpp"
#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
unsigned number(std::string_view text, unsigned maximum) {
    unsigned n = 0;
    auto [end, error] = std::from_chars(text.data(), text.data()+text.size(), n);
    if (error != std::errc{} || end != text.data()+text.size() || n == 0 || n > maximum)
        throw std::invalid_argument("invalid positive number: "+std::string(text));
    return n;
}
mistlane::Endpoint endpoint(std::string_view text) {
    // Split at the last colon, so unbracketed numeric IPv6 also works.
    auto colon = text.rfind(':');
    if (colon == text.npos || colon == 0) throw std::invalid_argument("expected IP:PORT");
    return {std::string(text.substr(0, colon)), static_cast<std::uint16_t>(number(text.substr(colon+1), 65535))};
}
}
int main(int argc, char** argv) {
    try {
        if (argc > 1) {
            const int scaffold_result = mistlane::run_scaffold(argv[1], argc, argv);
            if (scaffold_result >= 0) return scaffold_result;
        }
        mistlane::Config config;
        for (int i = 1; i < argc; ++i) {
            std::string key = argv[i];
            if (key == "--help") {
                std::cout << "mistlane --role worker|router|gateway|cross|shard --listen IP:PORT\n"
                             "mistlane init [DIRECTORY]  Create a multi-role starter project\n"
                             "  worker: --script FILE\n"
                             "  router/gateway: --upstream IP:PORT (repeat for session round robin)\n"
                             "  --transport tcp|ws|wss|http|https (wss/https require --cert FILE --key FILE)\n"
                             "  --max-connections N (default 1024)\n"
                             "  --memory-budget-mib N (default 256; conservative admission envelope)\n"
                             "  --redis IP:PORT (optional; Streams Lua API enabled)\n"
                             "  --metrics-listen IP:PORT (optional; Prometheus /metrics endpoint)\n"
                             "  --crashpad-handler FILE --crashpad-database DIR (optional; local dumps only)\n"
                             "  cross/shard: --route-epoch N --max-room-sessions N\n"
                             "    --max-group-sessions N --max-shard-sessions N\n"
                             "SIGHUP: transactional worker script reload (state is reset)\n";
                return 0;
            }
            if (++i >= argc) throw std::invalid_argument("missing value for "+key);
            std::string value = argv[i];
            if (key == "--role") config.role = value;
            else if (key == "--listen") config.listen = endpoint(value);
            else if (key == "--upstream") config.upstreams.push_back(endpoint(value));
            else if (key == "--transport") config.transport = value;
            else if (key == "--cert") config.certificate = value;
            else if (key == "--key") config.private_key = value;
            else if (key == "--script") config.script = value;
            else if (key == "--redis") { config.redis = endpoint(value); config.redis_enabled = true; }
            else if (key == "--metrics-listen") { config.metrics_listen = endpoint(value); config.metrics_enabled = true; }
            else if (key == "--crashpad-handler") config.crashpad_handler = value;
            else if (key == "--crashpad-database") config.crashpad_database = value;
            else if (key == "--memory-budget-mib") config.memory_budget_mib = number(value, 100000);
            else if (key == "--max-room-sessions") config.max_room_sessions = number(value, 100000);
            else if (key == "--max-group-sessions") config.max_group_sessions = number(value, 100000);
            else if (key == "--max-shard-sessions") config.max_shard_sessions = number(value, 100000);
            else if (key == "--route-epoch") config.route_epoch = number(value, 1000000000);
            else if (key == "--max-connections") config.max_connections = number(value, 100000);
            else throw std::invalid_argument("unknown option: "+key);
        }
        if (config.transport != "tcp" && config.transport != "ws" && config.transport != "wss" && config.transport != "http" && config.transport != "https")
            throw std::invalid_argument("unknown transport");
        if (config.transport == "wss" || config.transport == "https") {
            if (config.certificate.empty() || config.private_key.empty())
                throw std::invalid_argument("wss/https require --cert and --key");
        } else if (!config.certificate.empty() || !config.private_key.empty())
            throw std::invalid_argument("cert/key only apply to wss/https");
        if (config.role == "worker" || config.role == "shard") {
            if (config.script.empty() || !config.upstreams.empty())
                throw std::invalid_argument("worker requires --script and no upstream");
        } else if (config.role == "router" || config.role == "gateway" || config.role == "cross") {
            if (config.upstreams.empty() || !config.script.empty())
                throw std::invalid_argument("router/gateway requires upstream and no script");
        } else throw std::invalid_argument("unknown role: "+config.role);
        if (config.crashpad_handler.empty() != config.crashpad_database.empty())
            throw std::invalid_argument("Crashpad requires both --crashpad-handler and --crashpad-database");
        if ((config.role == "cross" || config.role == "shard") && config.transport != "tcp")
            throw std::invalid_argument("cross/shard require internal TCP transport");
        return mistlane::run_node(std::move(config));
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << '\n';
        return 1;
    }
}
