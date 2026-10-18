// SPDX-License-Identifier: Apache-2.0
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
    Endpoint redis{"127.0.0.1", 6379};
    Endpoint metrics_listen{"127.0.0.1", 9090};
    bool redis_enabled = false;
    bool metrics_enabled = false;
    std::string crashpad_handler, crashpad_database;
    std::string transport = "tcp";
    std::string certificate, private_key;
    std::size_t max_connections = 1024;
    std::size_t memory_budget_mib = 256;
    std::size_t max_room_sessions = 64, max_group_sessions = 128, max_shard_sessions = 256;
    unsigned route_epoch = 1;
};
int run_node(Config config);
}
