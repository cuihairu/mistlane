// SPDX-License-Identifier: Apache-2.0
#include "mistlane/roles.hpp"
#include "mistlane/node.hpp"
#include "mistlane/cli.hpp"
#include "mistlane/scaffold.hpp"
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

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
// "PROTO://IP:PORT[?cert=FILE&key=FILE]" adds one independent listener. The
// query certificate/key pair overrides the global --cert/--key for that
// listener only; anything else in the query is rejected.
mistlane::Listener listener(std::string_view text) {
    const auto scheme_end = text.find("://");
    if (scheme_end == text.npos) throw std::invalid_argument("listener must be PROTO://IP:PORT: "+std::string(text));
    mistlane::Listener result;
    result.protocol = std::string(text.substr(0, scheme_end));
    if (result.protocol != "tcp" && result.protocol != "ws" && result.protocol != "wss" &&
        result.protocol != "http" && result.protocol != "https")
        throw std::invalid_argument("unknown listener protocol: "+result.protocol);
    auto rest = text.substr(scheme_end+3);
    if (auto query = rest.find('?'); query != rest.npos) {
        const auto fields = rest.substr(query+1);
        rest.remove_suffix(rest.size()-query);
        std::size_t at = 0;
        for (;;) {
            const auto next = fields.find('&', at);
            const auto item = fields.substr(at, next == fields.npos ? fields.npos : next-at);
            const auto equals = item.find('=');
            const auto name = item.substr(0, equals);
            if (equals == item.npos || item.empty() ||
                (name != "cert" && name != "key"))
                throw std::invalid_argument("unsupported listener query field: "+std::string(item));
            (name == "cert" ? result.certificate : result.private_key) = std::string(item.substr(equals+1));
            if (next == fields.npos) break;
            at = next+1;
        }
    }
    result.endpoint = endpoint(rest);
    if (result.certificate.empty() != result.private_key.empty())
        throw std::invalid_argument("listener certificates need both cert= and key=");
    return result;
}
// Native config files are line-based KEY=VALUE with a mandatory version header,
// so a random shell env file is never consumed silently. Entries become the
// same flags the command line parses, in order: file values, then MISTLANE_<KEY>
// environment overrides, then any explicit flag given after --config.
std::vector<std::pair<std::string, std::string>> config_flags(const std::filesystem::path& file) {
    struct Field { const char* key; const char* flag; bool path; bool list; bool root; };
    static constexpr Field fields[] = {
        {"ROLE","--role",0,0,0}, {"ROUTING","--routing",0,0,0}, {"LISTEN","--listen",0,0,0}, {"TRANSPORT","--transport",0,0,0},
        {"SERVICE_TOKEN","--service-token",0,0,0}, {"UPSTREAM_TOKEN","--upstream-token",0,0,0},
        {"SERVICE_NAME","--service-name",0,0,0},
        {"SCRIPT","--script",1,0,0}, {"UPSTREAMS","--upstream",0,1,0}, {"CERT","--cert",1,0,0},
        {"KEY","--key",1,0,0}, {"LISTENERS","--listener",0,1,0}, {"REDIS","--redis",0,0,0},
        {"METRICS_LISTEN","--metrics-listen",0,0,0}, {"PROBE_LISTEN","--probe-listen",0,0,0},
        {"OVERLOAD_RATIO","--overload-ratio",0,0,0}, {"READY_HYSTERESIS","--ready-hysteresis",0,0,0},
        {"MAX_CONNECTIONS","--max-connections",0,0,0}, {"MEMORY_BUDGET_MIB","--memory-budget-mib",0,0,0},
        {"MAX_ROOM_SESSIONS","--max-room-sessions",0,0,0}, {"MAX_GROUP_SESSIONS","--max-group-sessions",0,0,0},
        {"MAX_SHARD_SESSIONS","--max-shard-sessions",0,0,0}, {"ROUTE_EPOCH","--route-epoch",0,0,0},
        {"CRASHPAD_HANDLER","--crashpad-handler",1,0,0}, {"CRASHPAD_DATABASE","--crashpad-database",1,0,0},
        {"ROOT","",0,0,1},
    };
    std::ifstream in(file);
    if (!in) throw std::invalid_argument("cannot read config file: "+file.string());
    const auto trim = [](std::string_view text) {
        const auto from = text.find_first_not_of(" \t");
        const auto to = text.find_last_not_of(" \t");
        return from == std::string_view::npos ? std::string() : std::string(text.substr(from, to-from+1));
    };
    const auto version_error = [&](std::size_t line) {
        return file.string()+":"+std::to_string(line)+
            ": config must start with the '# mistlane-config 1' version line"; };
    std::map<std::string, std::string> entries;
    std::map<std::string, std::size_t> first_seen;
    bool header = false;
    std::size_t line_no = 0;
    std::string raw;
    while (std::getline(in, raw)) {
        ++line_no;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        const auto head = raw.find_first_not_of(" \t");
        if (head == std::string::npos) continue;
        if (raw[head] == '#') {
            if (!header && std::string_view(raw).substr(head) != "# mistlane-config 1")
                throw std::invalid_argument(version_error(line_no));
            header = true;
            continue;
        }
        if (!header) throw std::invalid_argument(version_error(line_no));
        const auto equals = raw.find('=');
        const auto key = trim(std::string_view(raw).substr(0, equals == std::string::npos ? raw.size() : equals));
        if (equals == std::string::npos || key.empty())
            throw std::invalid_argument(file.string()+":"+std::to_string(line_no)+": expected KEY=VALUE");
        bool known = false;
        for (const auto& field : fields) known = known || key == field.key;
        if (!known)
            throw std::invalid_argument(file.string()+":"+std::to_string(line_no)+": unknown key "+key);
        if (first_seen.count(key))
            throw std::invalid_argument(file.string()+":"+std::to_string(line_no)+
                ": duplicate key "+key+" (first set on line "+std::to_string(first_seen[key])+")");
        first_seen.emplace(key, line_no);
        auto value = trim(std::string_view(raw).substr(equals+1));
        if (const char* override_value = ::getenv(("MISTLANE_"+key).c_str()); override_value && *override_value)
            value = override_value;
        if (!value.empty()) entries[key] = std::move(value);
    }
    if (!header) throw std::invalid_argument(version_error(1));
    std::filesystem::path base = file.parent_path();
    if (const auto root = entries.find("ROOT"); root != entries.end()) base = base / root->second;
    std::vector<std::pair<std::string, std::string>> flags;
    for (const auto& field : fields) {
        if (field.root) continue;
        const auto entry = entries.find(field.key);
        if (entry == entries.end()) continue;
        if (field.path) {
            std::filesystem::path resolved(entry->second);
            if (resolved.is_relative()) resolved = base / resolved;
            flags.emplace_back(field.flag, resolved.string());
        } else if (field.list) {
            for (std::size_t at = 0; at < entry->second.size();) {
                const auto next = entry->second.find(',', at);
                auto item = entry->second.substr(at, next == std::string::npos ? std::string::npos : next-at);
                if (const auto trimmed = trim(item); !trimmed.empty()) flags.emplace_back(field.flag, trimmed);
                if (next == std::string::npos) break;
                at = next+1;
            }
        } else flags.emplace_back(field.flag, entry->second);
    }
    return flags;
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
        bool transport_selected = false;
        // --config entries expand in place: file values first, MISTLANE_<KEY>
        // environment overrides inside the reader, then any explicit flag after.
        std::vector<std::string> tokens;
        for (int i = 1; i < argc;) {
            if (std::string_view(argv[i]) == "--config") {
                if (++i >= argc) throw std::invalid_argument("missing value for --config");
                for (const auto& item : config_flags(argv[i])) {
                    tokens.emplace_back(item.first);
                    tokens.emplace_back(item.second);
                }
                ++i;
                continue;
            }
            tokens.emplace_back(argv[i++]);
        }
        for (std::size_t i = 0; i < tokens.size(); ++i) {
            std::string key = tokens[i];
            if (key == "--help") {
                std::cout << "mistlane --role gateway|gameServer|crossServer|apiServer --listen IP:PORT\n"
                             "mistlane init [DIRECTORY]  Create a multi-role starter project\n"
                             "mistlane test [--list|--suite NAME|--case NAME]  Run native C++ cases\n"
                             "mistlane bot --url URL [--clients N --count N]  Simulate game requests\n"
                             "  --config FILE  KEY=VALUE native config ('# mistlane-config 1' header;\n"
                             "    MISTLANE_<KEY> env overrides, flags after --config win, relative\n"
                             "    paths resolve against the file or its ROOT= entry)\n"
                             "  gameServer/crossServer/apiServer: --script FILE\n"
                             "  gateway: --upstream IP:PORT (repeat for session round robin)\n"
                             "  --routing session|cross (gateway only; default session)\n"
                             "  service identity (session routing only; cross keeps MLC1):\n"
                             "    --service-token TOKEN (accept only peers presenting MLS1),\n"
                             "    --upstream-token TOKEN --service-name NAME (present MLS1 to each upstream)\n"
                             "  --transport tcp|ws|wss|http|https (wss/https require --cert FILE --key FILE)\n"
                             "  --listener PROTO://IP:PORT (repeat; independent extra listener, mixed\n"
                             "    plaintext/TLS allowed; wss/https may add ?cert=FILE&key=FILE overrides)\n"
                             "  --max-connections N (default 1024)\n"
                             "  --memory-budget-mib N (default 256; conservative admission envelope)\n"
                             "  --redis IP:PORT (optional; Streams Lua API enabled)\n"
                             "    Redis TLS: MISTLANE_REDIS_TLS=on (chain+hostname verified),\n"
                             "    MISTLANE_REDIS_CA=FILE (optional trust anchor)\n"
                             "  --metrics-listen IP:PORT (optional; Prometheus /metrics endpoint)\n"
                             "  --probe-listen IP:PORT (optional; internal PROBE1 liveness/readiness)\n"
                             "    --overload-ratio X (readiness gate, default 0.9) --ready-hysteresis N (default 3)\n"
                             "    upstreams take IP:PORT[:PROBE] so cross-routing gateways can check crossServer readiness\n"
                             "  --crashpad-handler FILE --crashpad-database DIR (optional; local dumps only)\n"
                             "  cross routing/crossServer: --route-epoch N --max-room-sessions N\n"
                             "    --max-group-sessions N --max-shard-sessions N\n"
                             "SIGHUP: transactional script reload (state is reset)\n"
                             "Planned, not runnable: login, coordinator, data\n";
                return 0;
            }
            if (++i >= tokens.size()) throw std::invalid_argument("missing value for "+key);
            std::string value = tokens[i];
            if (key == "--role") config.role = value;
            else if (key == "--listen") config.listen = endpoint(value);
            else if (key == "--service-token") config.service_token = value;
            else if (key == "--upstream-token") config.upstream_token = value;
            else if (key == "--service-name") config.service_name = value;
            else if (key == "--upstream") config.upstreams.push_back(upstream(value));
            else if (key == "--routing") config.routing = value;
            else if (key == "--transport") { config.transport = value; transport_selected = true; }
            else if (key == "--cert") config.certificate = value;
            else if (key == "--key") config.private_key = value;
            else if (key == "--listener") config.listeners.push_back(listener(value));
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
        const auto* role = mistlane::find_role(config.role);
        if (!role) throw std::invalid_argument("unknown role: " + config.role);
        if (!role->runnable) throw std::invalid_argument("role " + config.role + " is planned but not implemented");
        if (config.role == "apiServer" && !transport_selected) config.transport = "http";
        if (config.routing != "session" && config.routing != "cross")
            throw std::invalid_argument("unknown routing policy: " + config.routing);
        if (config.routing != "session" && config.role != "gateway")
            throw std::invalid_argument("--routing applies to gateway only");
        if (config.transport != "tcp" && config.transport != "ws" && config.transport != "wss" && config.transport != "http" && config.transport != "https")
            throw std::invalid_argument("unknown transport");
        if (config.transport == "wss" || config.transport == "https") {
            if (config.certificate.empty() || config.private_key.empty())
                throw std::invalid_argument("wss/https require --cert and --key");
        } else if (!config.certificate.empty() || !config.private_key.empty())
            throw std::invalid_argument("cert/key only apply to wss/https");
        if (role->script) {
            if (config.script.empty() || !config.upstreams.empty())
                throw std::invalid_argument(config.role + " requires --script and no upstream");
        } else if (config.upstreams.empty() || !config.script.empty()) {
            throw std::invalid_argument("gateway requires upstream and no script");
        }
        // Cross routing keeps its own MLC1 join as the identity; accepting the
        // token flags there would silently skip the handshake instead of
        // surfacing the misconfiguration.
        if (!config.service_token.empty() && (config.routing == "cross" || config.role == "crossServer"))
            throw std::invalid_argument("--service-token does not apply to cross routing");
        if (!config.upstream_token.empty() && config.routing == "cross")
            throw std::invalid_argument("--upstream-token does not apply to cross routing");
        // The primary listener comes from --listen/--transport; --listener entries follow.
        mistlane::Listener primary;
        primary.protocol = config.transport;
        primary.endpoint = config.listen;
        config.listeners.insert(config.listeners.begin(), std::move(primary));
        for (std::size_t i = 0; i < config.listeners.size(); ++i)
            for (std::size_t j = i+1; j < config.listeners.size(); ++j)
                // Same port twice (any address) is ambiguous for websocket
                // upgrade dispatch, which reads the local port per connection.
                if (config.listeners[i].endpoint.port == config.listeners[j].endpoint.port)
                    throw std::invalid_argument("duplicate listener port "+
                        std::to_string(config.listeners[i].endpoint.port));
        bool any_tls = false, any_non_tcp = false, any_tcp = false;
        for (const auto& item : config.listeners) {
            const bool tls = item.protocol == "wss" || item.protocol == "https";
            any_tls = any_tls || tls;
            any_non_tcp = any_non_tcp || item.protocol != "tcp";
            any_tcp = any_tcp || item.protocol == "tcp";
            if (tls && (item.certificate.empty() ? config.certificate : item.certificate).empty())
                throw std::invalid_argument("wss/https listeners require --cert and --key (or listener ?cert= and ?key=)");
        }
        if (!any_tls && (!config.certificate.empty() || !config.private_key.empty()))
            throw std::invalid_argument("cert/key only apply to wss/https");
        if (config.role == "apiServer" && any_tcp)
            throw std::invalid_argument("apiServer requires http, https, ws or wss transport");
        if (config.crashpad_handler.empty() != config.crashpad_database.empty())
            throw std::invalid_argument("Crashpad requires both --crashpad-handler and --crashpad-database");
        if ((config.is_cross_gateway() || config.role == "crossServer") && any_non_tcp)
            throw std::invalid_argument("cross routing and crossServer require internal TCP transport");
        return mistlane::run_node(std::move(config));
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << '\n';
        return 1;
    }
}
