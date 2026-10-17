#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mistlane {
struct Endpoint { std::string address; std::uint16_t port; };
struct Config {
    std::string role = "worker";
    Endpoint listen{"127.0.0.1", 7102};
    std::vector<Endpoint> upstreams;
    std::string script;
    std::string transport = "tcp";
    std::string certificate, private_key;
    std::size_t max_connections = 1024;
};
int run_node(Config config);
}
