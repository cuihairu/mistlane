// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mistlane {
// probe_port carries the shard's internal probe listener for cross upstreams; 0 disables probing.
struct Endpoint { std::string address; std::uint16_t port; std::uint16_t probe_port = 0; };
// One process listener. protocol is tcp|ws|wss|http|https; TLS listeners use the
// global cert/key unless they carry their own. --listen/--transport configure the
// first entry; --listener adds more.
struct Listener {
    std::string protocol = "tcp";
    Endpoint endpoint{"127.0.0.1", 7102};
    std::string certificate, private_key;
};
struct Config {
    std::string role = "gameServer";
    std::string routing = "session"; // gateway policy: session or cross
    Endpoint listen{"127.0.0.1", 7102};
    std::vector<Endpoint> upstreams;
    std::string script;
    Endpoint redis{"127.0.0.1", 6379};
    Endpoint metrics_listen{"127.0.0.1", 9090};
    Endpoint probe_listen{"127.0.0.1", 7103};
    bool redis_enabled = false;
    bool metrics_enabled = false;
    bool probe_enabled = false;
    std::string crashpad_handler, crashpad_database;
    std::string transport = "tcp";
    std::string certificate, private_key;
    std::vector<Listener> listeners; // the authoritative listener set, filled by the CLI
    std::size_t max_connections = 1024;
    std::size_t memory_budget_mib = 256;
    std::size_t max_room_sessions = 64, max_group_sessions = 128, max_shard_sessions = 256;
    bool is_cross_gateway() const { return role == "gateway" && routing == "cross"; }
    unsigned route_epoch = 1;
    double overload_ratio = 0.9;   // readiness flips not-ready at this share of max_connections
    unsigned ready_hysteresis = 3; // consecutive over/under-threshold ticks before flipping
};
int run_node(Config config);
}
