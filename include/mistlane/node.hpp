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
    // MLS1 service identity: a non-empty service_token makes the node accept
    // only peers presenting "MLS1 <role> <instance> <token>"; a non-empty
    // upstream_token makes the gateway present itself to each upstream.
    // Cross routing keeps its own MLC1 join as the identity.
    std::string service_token, upstream_token, service_name;
    // gateway multiplexes many logical sessions over one upstream connection
    // (MLM1). Internal mode: the peer must accept MLS1 for the upgrade to offer.
    bool upstream_multiplex = false;
    // Members (gameServer/crossServer) register with a coordinator over MLR1:
    // heartbeat readiness and load there, learn nothing back yet. scope names
    // the deployment slice (a region or a cross-group) the member serves.
    Endpoint coordinator{"127.0.0.1", 7200};
    bool coordinator_enabled = false;
    std::string scope = "main";
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
