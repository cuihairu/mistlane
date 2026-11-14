// SPDX-License-Identifier: Apache-2.0
#include "mistlane/cli.hpp"
#include "mistlane/transport.hpp"
#include "business_plan.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <optional>
#include <sstream>
#include <array>
#include <thread>
#include <vector>

namespace mistlane {
namespace {
unsigned count_value(const std::string& text, unsigned max, bool zero = false) {
    unsigned value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data()+text.size(), value);
    if (ec != std::errc{} || end != text.data()+text.size() || (!zero && value == 0) || value > max)
        throw std::invalid_argument("invalid count/duration: " + text);
    return value;
}
}
int run_bot(int argc, char** argv) {
    cli::ClientOptions options{"tcp://127.0.0.1:7100", {}, 5000, "POST"};
    unsigned clients = 1, count = 1, interval = 0;
    std::string payload = "hello mistlane", file, join;
    std::optional<std::string> expected;
    bool echo = false;
    // Business scenario state (inert unless --scenario business).
    std::string scenario, contract_file, service_token;
    unsigned scope = 1, entity_type = 2, incarnation = 1, generation = 1;
    unsigned entity_base = 1000, rooms = 1, heartbeat_every = 0, ramp_ms = 0;
    bool spawn = false;
    // The contract must outlive the plan: the sproto codec borrows it.
    std::optional<EntityContract> contract_storage;
    std::optional<BusinessPlan> plan_storage;
    const BusinessPlan* plan = nullptr;
    for (int i = 2; i < argc; ++i) {
        std::string key = argv[i];
        if (key == "--help") {
            std::cout << "mistlane bot [--url tcp://HOST:PORT|ws[s]://HOST:PORT/ws|http[s]://HOST:PORT/message]\n"
                "  --clients N       Concurrent connections (default 1, maximum 128)\n"
                "  --count N         Requests per connection (default 1; at most 100000 total)\n"
                "  --payload TEXT    Message bytes (default: hello mistlane)\n"
                "  --payload-file F  Binary payload, at most 1 MiB\n"
                "  --expect-echo     Check that responses equal the payload\n"
                "  --expect TEXT     Check an exact response\n"
                "  --join TEXT       Initial TCP/cross handshake; expects MLC1 OK 1\n"
                "  --interval-ms N   Delay after each response (default 0)\n"
                "  --timeout-ms N    Connect/request timeout (default 5000, maximum 60000)\n"
                "  --ca-file FILE    Trust a PEM CA for WSS/HTTPS; TLS verification stays enabled\n"
                "  --method GET|POST HTTP method (default POST)\n"
                "Business scenario (MLC1-framed MLE1 entity traffic against a contract host):\n"
                "  --scenario business   Drive login -> joinRoom -> move/heartbeat MLE1 steps\n"
                "  --contract FILE       The entity contract .def the host serves (required)\n"
                "  --service-token T     Present MLS1 <name> <instance> <T> first; expects MLS1 OK\n"
                "  --spawn               Send 'spawn <entity-id>' and expect 'spawned'\n"
                "  --scope N --entity-type N --incarnation N --generation N  Envelope addressing\n"
                "  --entity-base N       First entity id; one per connection (default 1000)\n"
                "  --rooms N             Join room (1 + connection %% N) (default 1)\n"
                "  --heartbeat-every N   Every Nth step after login/join is a heartbeat\n"
                "  --ramp-ms N           Stagger the connection starts across N milliseconds\n"
                "Serial request/response per connection. Errors produce a nonzero exit code.\n";
            return 0;
        }
        if (key == "--expect-echo") { echo = true; continue; }
        if (key == "--spawn") { spawn = true; continue; }
        if (++i >= argc) throw std::invalid_argument("missing value for " + key);
        std::string value = argv[i];
        if (key == "--url") options.url = value;
        else if (key == "--clients") clients = count_value(value, 128);
        else if (key == "--count") count = count_value(value, 100000);
        else if (key == "--interval-ms") interval = count_value(value, 60000, true);
        else if (key == "--timeout-ms") options.timeout_ms = count_value(value, 60000);
        else if (key == "--payload") payload = value;
        else if (key == "--payload-file") file = value;
        else if (key == "--expect") expected = value;
        else if (key == "--ca-file") options.ca_file = value;
        else if (key == "--join") join = value;
        else if (key == "--method") options.method = value;
        else if (key == "--scenario") scenario = value;
        else if (key == "--contract") contract_file = value;
        else if (key == "--service-token") service_token = value;
        else if (key == "--scope") scope = count_value(value, 1000000);
        else if (key == "--entity-type") entity_type = count_value(value, 63);
        else if (key == "--incarnation") incarnation = count_value(value, 1000000);
        else if (key == "--generation") generation = count_value(value, 1000000);
        else if (key == "--entity-base") entity_base = count_value(value, 1000000000);
        else if (key == "--rooms") rooms = count_value(value, 1024);
        else if (key == "--heartbeat-every") heartbeat_every = count_value(value, 1000000, true);
        else if (key == "--ramp-ms") ramp_ms = count_value(value, 600000, true);
        else throw std::invalid_argument("unknown bot option: " + key);
    }
    if (options.method != "GET" && options.method != "POST")
        throw std::invalid_argument("bot HTTP method must be GET or POST");
    if (!join.empty() && !options.url.starts_with("tcp://"))
        throw std::invalid_argument("--join currently applies only to internal TCP/cross");
    if (clients*count > 100000) throw std::invalid_argument("bot allows at most 100000 total requests");
    if (!scenario.empty() && scenario != "business")
        throw std::invalid_argument("unknown scenario: " + scenario);
    const bool business = !scenario.empty();
    if (business) {
        if (!options.url.starts_with("tcp://"))
            throw std::invalid_argument("--scenario business drives the framed TCP entity path only");
        if (contract_file.empty())
            throw std::invalid_argument("--scenario business requires --contract FILE");
        std::ifstream input(contract_file, std::ios::binary);
        if (!input) throw std::invalid_argument("cannot read contract: " + contract_file);
        std::ostringstream text; text << input.rdbuf();
        contract_storage.emplace(text.str());
        plan_storage.emplace(*contract_storage, entity_type, scope, incarnation, generation);
        plan = &*plan_storage;
    }
    if (!file.empty()) {
        std::ifstream input(file, std::ios::binary | std::ios::ate);
        if (!input || input.tellg() < 0 || input.tellg() > static_cast<std::streamoff>(max_frame))
            throw std::invalid_argument("payload file missing or exceeds 1 MiB");
        payload.resize(static_cast<std::size_t>(input.tellg()));
        input.seekg(0);
        if (!input.read(payload.data(), static_cast<std::streamsize>(payload.size())))
            throw std::runtime_error("cannot read payload file");
    }
    if (payload.size() > max_frame) throw std::invalid_argument("payload exceeds 1 MiB");
    if (echo) {
        if (expected) throw std::invalid_argument("choose --expect or --expect-echo");
        expected = payload;
    }
    std::mutex mutex;
    std::vector<double> latencies;
    std::vector<std::thread> threads;
    std::string first_error, first_response;
    unsigned failures = 0, successes = 0;
    std::array<unsigned long, 4> counters{0, 0, 0, 0}; // login, join, move, heartbeat
    const auto started = std::chrono::steady_clock::now();
    for (unsigned c = 0; c < clients; ++c) threads.emplace_back([&, c] {
        try {
            if (ramp_ms) // spread the joins across the ramp window
                std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<unsigned long long>(ramp_ms)*c/clients));
            auto client = cli::make_client(options);
            if (!join.empty() && client->request(join) != "MLC1 OK 1")
                throw std::runtime_error("cross handshake rejected (bot expects route epoch 1)");
            const auto entity_id = entity_base + c;
            if (business) {
                if (!service_token.empty()) {
                    const auto verdict = client->request("MLS1 bot bot-" + std::to_string(c)
                        + " " + service_token);
                    if (verdict != "MLS1 OK") throw std::runtime_error("MLS1 handshake rejected");
                }
                if (spawn && client->request("spawn " + std::to_string(entity_id)) != "spawned")
                    throw std::runtime_error("entity spawn rejected (wanted 'spawned')");
                std::uint64_t correlation = 0;
                const auto room = rooms ? 1 + c % rooms : 1;
                for (unsigned step = 0; step < count + 2; ++step) {
                    business_step(*plan, *client, entity_id, correlation, options.timeout_ms,
                        step, heartbeat_every, room, c, counters, mutex);
                    if (interval && step + 1 < count + 2)
                        std::this_thread::sleep_for(std::chrono::milliseconds(interval));
                }
            } else {
                for (unsigned i = 0; i < count; ++i) {
                    const auto begin = std::chrono::steady_clock::now();
                    auto response = client->request(payload);
                    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-begin).count();
                    if (expected && response != *expected) throw std::runtime_error("response mismatch");
                    {
                        std::lock_guard lock(mutex);
                        if (successes == 0) first_response = response.substr(0, 256);
                        ++successes;
                        latencies.push_back(ms);
                    }
                    if (interval && i+1 < count) std::this_thread::sleep_for(std::chrono::milliseconds(interval));
                }
            }
        } catch (const std::exception& e) {
            std::lock_guard lock(mutex);
            ++failures;
            if (first_error.empty()) first_error = e.what();
        }
    });
    for (auto& thread : threads) thread.join();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
    std::sort(latencies.begin(), latencies.end());
    auto percentile = [&](double p) { return latencies.empty() ? 0.0 :
        latencies[std::min(latencies.size()-1, static_cast<std::size_t>(p*(latencies.size()-1)))]; };
    std::cout << std::fixed << std::setprecision(3)
        << "planned=" << clients*count << " completed=" << successes << " failed_clients=" << failures
        << " elapsed_seconds=" << seconds << " requests_per_second=" << (seconds ? successes/seconds : 0)
        << " p50_ms=" << percentile(.50) << " p95_ms=" << percentile(.95) << " p99_ms=" << percentile(.99) << '\n';
    if (business)
        std::cout << "scenario=business logins_ok=" << counters[0] << " joins_ok=" << counters[1]
            << " moves_ok=" << counters[2] << " heartbeats_ok=" << counters[3] << '\n';
    if (clients == 1 && count == 1 && !failures) std::cout << "response=" << std::quoted(first_response) << '\n';
    if (!first_error.empty()) std::cerr << "bot error: " << first_error << '\n';
    return failures ? 1 : 0;
}
}
