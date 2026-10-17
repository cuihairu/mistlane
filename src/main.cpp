#include "mistlane/node.hpp"
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
        mistlane::Config config;
        for (int i = 1; i < argc; ++i) {
            std::string key = argv[i];
            if (key == "--help") {
                std::cout << "mistlane --role worker|router|gateway --listen IP:PORT\n"
                             "  worker: --script FILE\n"
                             "  router/gateway: --upstream IP:PORT (repeat for session round robin)\n"
                             "  --transport tcp|ws|wss (wss requires --cert FILE --key FILE)\n"
                             "  --max-connections N (default 1024)\n"
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
            else if (key == "--max-connections") config.max_connections = number(value, 100000);
            else throw std::invalid_argument("unknown option: "+key);
        }
        if (config.transport != "tcp" && config.transport != "ws" && config.transport != "wss")
            throw std::invalid_argument("unknown transport");
        if (config.transport == "wss") {
            if (config.certificate.empty() || config.private_key.empty())
                throw std::invalid_argument("wss requires --cert and --key");
        } else if (!config.certificate.empty() || !config.private_key.empty())
            throw std::invalid_argument("cert/key only apply to wss");
        if (config.role == "worker") {
            if (config.script.empty() || !config.upstreams.empty())
                throw std::invalid_argument("worker requires --script and no upstream");
        } else if (config.role == "router" || config.role == "gateway") {
            if (config.upstreams.empty() || !config.script.empty())
                throw std::invalid_argument("router/gateway requires upstream and no script");
        } else throw std::invalid_argument("unknown role: "+config.role);
        return mistlane::run_node(std::move(config));
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << '\n';
        return 1;
    }
}
