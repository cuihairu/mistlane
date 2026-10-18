// SPDX-License-Identifier: Apache-2.0
#include "mistlane/node.hpp"
#include "mistlane/cli.hpp"
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
mistlane::Endpoint upstream(std::string_view text) {
    // "IP:PORT" or, when the peer exposes a probe listener, "IP:PORT:PROBE".
    // The probe form needs the two trailing fields to be bare ports, so bracket
    // IPv6 literals when a probe port is appended; plain "IP:PORT" still splits
    // at the last colon like endpoint().
    if (auto last = text.rfind(':'); last != text.npos && last > 0) {
        if (auto split = text.rfind(':', last-1); split != text.npos && split > 0) {
            const auto address = text.substr(0, split);
            const auto digits = [](std::string_view field) {
                return !field.empty() && field.find_first_not_of("0123456789") == field.npos; };
            if ((address.find(':') == address.npos || (address.front() == '[' && address.back() == ']'))
                && digits(text.substr(split+1, last-split-1)) && digits(text.substr(last+1)))
                return {std::string(address), static_cast<std::uint16_t>(number(text.substr(split+1, last-split-1), 65535)),
                    static_cast<std::uint16_t>(number(text.substr(last+1), 65535))};
        }
    }
    return endpoint(text);
}
double ratio(std::string_view text) {
    const std::string plain(text);
    std::size_t consumed = 0;
    const double value = std::stod(plain, &consumed);
    if (consumed != plain.size() || !(value > 0 && value <= 1))
        throw std::invalid_argument("overload ratio must be in (0,1]: "+plain);
    return value;
}
}
int main(int argc, char** argv) {
    try {
        if (argc > 1) {
            if (std::string_view(argv[1]) == "test") return mistlane::run_tests(argc, argv);
            if (std::string_view(argv[1]) == "bot") return mistlane::run_bot(argc, argv);
            const int scaffold_result = mistlane::run_scaffold(argv[1], argc, argv);
            if (scaffold_result >= 0) return scaffold_result;
        }
        mistlane::Config config;
        for (int i = 1; i < argc; ++i) {
            std::string key = argv[i];
            if (key == "--help") {
                std::cout << "mistlane --role worker|router|gateway|cross|shard --listen IP:PORT\n"
                             "mistlane init [DIRECTORY]  Create a multi-role starter project\n"
                             "mistlane test [--list|--suite NAME|--case NAME]  Run native C++ cases\n"
                             "mistlane bot --url URL [--clients N --count N]  Simulate game requests\n"
                             "  worker: --script FILE\n"
                             "  router/gateway: --upstream IP:PORT (repeat for session round robin)\n"
                             "  --transport tcp|ws|wss|http|https (wss/https require --cert FILE --key FILE)\n"
                             "  --max-connections N (default 1024)\n"
                             "  --memory-budget-mib N (default 256; conservative admission envelope)\n"
                             "  --redis IP:PORT (optional; Streams Lua API enabled)\n"
                             "    Redis TLS: MISTLANE_REDIS_TLS=on (chain+hostname verified),\n"
                             "    MISTLANE_REDIS_CA=FILE (optional trust anchor)\n"
                             "  --metrics-listen IP:PORT (optional; Prometheus /metrics endpoint)\n"
                             "  --probe-listen IP:PORT (optional; internal PROBE1 liveness/readiness)\n"
                             "    --overload-ratio X (readiness gate, default 0.9) --ready-hysteresis N (default 3)\n"
                             "    upstreams take IP:PORT[:PROBE] so cross can check shard readiness\n"
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
            else if (key == "--upstream") config.upstreams.push_back(upstream(value));
            else if (key == "--transport") config.transport = value;
            else if (key == "--cert") config.certificate = value;
            else if (key == "--key") config.private_key = value;
            else if (key == "--script") config.script = value;
            else if (key == "--redis") { config.redis = endpoint(value); config.redis_enabled = true; }
            else if (key == "--metrics-listen") { config.metrics_listen = endpoint(value); config.metrics_enabled = true; }
            else if (key == "--probe-listen") { config.probe_listen = endpoint(value); config.probe_enabled = true; }
            else if (key == "--overload-ratio") config.overload_ratio = ratio(value);
            else if (key == "--ready-hysteresis") config.ready_hysteresis = number(value, 100000);
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
