// SPDX-License-Identifier: Apache-2.0
// tools/bots: standalone load and fault-injection driver for mistlane test
// nodes. It rides the real session plane like the embedded `mistlane bot`:
// framed TCP, MLS1 identity, spawn, MLE1 entity steps and MLD1 record legs
// (src/session_plane.hpp, src/tcp_channel.hpp, include/mistlane/record.hpp).
// Scope is research & learning: it finds problems in this repository's own
// nodes, it is not a general-purpose product.
//
//   mistlane-bots --plan plan.toml [--url tcp://IP:PORT] [--mode load|inject]
//
// Plan files are a small TOML subset ([table] headers, key = value with
// string/integer/boolean scalars, # comments). Unknown keys fail loudly so a
// typo can never silently disable a scenario, mirroring the native config
// reader in src/main.cpp.
#include "business_plan.hpp"
#include "mistlane/http_client.hpp"
#include "mistlane/transport.hpp"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <netinet/in.h>
#include <sys/socket.h>

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace mistlane {
namespace {

// ---------------------------------------------------------------------------
// Plan file: TOML subset reader with per-line errors and a strict key list.
// ---------------------------------------------------------------------------
class PlanFile {
public:
    struct Entry { std::string value; std::size_t line = 0; };
    explicit PlanFile(const std::string& file) {
        std::ifstream in(file);
        if (!in) throw std::invalid_argument("cannot read plan: " + file);
        const auto trim = [](std::string_view text) {
            const auto from = text.find_first_not_of(" \t");
            const auto to = text.find_last_not_of(" \t");
            return from == std::string_view::npos ? std::string()
                : std::string(text.substr(from, to - from + 1));
        };
        std::string raw, table;
        std::size_t line_no = 0;
        while (std::getline(in, raw)) {
            ++line_no;
            if (!raw.empty() && raw.back() == '\r') raw.pop_back();
            const auto text = trim(raw);
            if (text.empty() || text[0] == '#') continue;
            if (text.front() == '[') {
                if (text.back() != ']')
                    throw std::invalid_argument(file + ":" + std::to_string(line_no)
                        + ": expected [table]");
                table = trim(std::string_view(text).substr(1, text.size() - 2));
                if (table.empty())
                    throw std::invalid_argument(file + ":" + std::to_string(line_no)
                        + ": empty table name");
                continue;
            }
            const auto equals = text.find('=');
            if (equals == std::string::npos || equals == 0)
                throw std::invalid_argument(file + ":" + std::to_string(line_no)
                    + ": expected key = value");
            const auto key = trim(std::string_view(text).substr(0, equals));
            auto value = trim(std::string_view(text).substr(equals + 1));
            if (key.empty() || value.empty())
                throw std::invalid_argument(file + ":" + std::to_string(line_no)
                    + ": empty key or value");
            if (value.front() == '"') {
                if (value.size() < 2 || value.back() != '"')
                    throw std::invalid_argument(file + ":" + std::to_string(line_no)
                        + ": unterminated string");
                value = value.substr(1, value.size() - 2);
            }
            const auto at = tables_[table].find(key);
            if (at != tables_[table].end())
                throw std::invalid_argument(file + ":" + std::to_string(line_no)
                    + ": duplicate key " + key + " (first set on line "
                    + std::to_string(at->second.line) + ")");
            tables_[table][key] = {value, line_no};
        }
    }
    // Known-key gate: a key the tool never reads is a mistake, not dead
    // weight, so refuse it loudly instead of silently ignoring it.
    void allow(const std::string& table, std::initializer_list<const char*> keys) const {
        const auto found = tables_.find(table);
        if (found == tables_.end()) return;
        for (const auto& [key, entry] : found->second) {
            bool known = false;
            for (const char* candidate : keys) known = known || key == candidate;
            if (!known)
                throw std::invalid_argument("plan line " + std::to_string(entry.line)
                    + ": unknown key [" + table + "] " + key);
        }
    }
    bool has(const std::string& table, const std::string& key) const {
        const auto found = tables_.find(table);
        return found != tables_.end() && found->second.count(key);
    }
    std::string str(const std::string& table, const std::string& key) const {
        return tables_.at(table).at(key).value;
    }
    std::string opt_str(const std::string& table, const std::string& key,
                        const std::string& fallback = {}) const {
        return has(table, key) ? str(table, key) : fallback;
    }
    std::uint64_t num(const std::string& table, const std::string& key,
                      std::uint64_t low, std::uint64_t high) const {
        const auto& entry = tables_.at(table).at(key);
        std::uint64_t value = 0;
        const auto& text = entry.value;
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (ec != std::errc{} || end != text.data() + text.size() || value < low || value > high)
            throw std::invalid_argument("plan line " + std::to_string(entry.line)
                + ": [" + table + "] " + key + " must be an integer in ["
                + std::to_string(low) + "," + std::to_string(high) + "]: " + text);
        return value;
    }
    bool boolean(const std::string& table, const std::string& key, bool fallback) const {
        if (!has(table, key)) return fallback;
        const auto& text = tables_.at(table).at(key).value;
        if (text == "true") return true;
        if (text == "false") return false;
        throw std::invalid_argument("plan line " + std::to_string(tables_.at(table).at(key).line)
            + ": [" + table + "] " + key + " must be true or false: " + text);
    }
private:
    std::map<std::string, std::map<std::string, Entry>> tables_;
};

struct LoadPlan {
    std::string url, contract_file, service_token, token_prefix = "bot-";
    unsigned concurrency = 1, requests = 0, timeout_ms = 5000, ramp_ms = 0;
    std::uint64_t duration_s = 0, rate = 0;
    bool spawn = false;
    std::uint32_t scope = 1, incarnation = 1, generation = 1, entity_type = 2;
    std::uint64_t entity_base = 1000;
    unsigned rooms = 1, heartbeat_every = 0;
};

struct InjectPlan {
    std::string url, url_data, url_metrics, service_token, contract_file;
    bool bad_frames = true, late_acks = true, slow_clients = true, disconnect_storm = true;
    unsigned storm_rounds = 3, storm_burst = 8, slow_wait_ms = 300;
    std::uint64_t storm_entity_base = 500000, entity_base = 1000, timeout_ms = 5000;
    bool spawn = true;
    std::uint32_t scope = 1, incarnation = 1, generation = 1, entity_type = 2;
};

std::uint64_t opt_num(const PlanFile& plan, const std::string& table, const std::string& key,
                      std::uint64_t low, std::uint64_t high, std::uint64_t fallback) {
    return plan.has(table, key) ? plan.num(table, key, low, high) : fallback;
}
// The scenario table is shared with load plans: [inject] wins, [scenario]
// backs it up, and the declared default is last.
std::uint64_t plan_num(const PlanFile& plan, const std::string& key,
                       std::uint64_t low, std::uint64_t high, std::uint64_t fallback) {
    if (plan.has("inject", key)) return plan.num("inject", key, low, high);
    if (plan.has("scenario", key)) return plan.num("scenario", key, low, high);
    return fallback;
}

LoadPlan parse_load(const PlanFile& plan, const std::string& url_override) {
    plan.allow("run", {"url", "concurrency", "ramp_ms", "requests", "duration_s", "rate",
                       "timeout_ms", "mode"});
    plan.allow("scenario", {"contract", "service_token", "token_prefix", "spawn", "scope",
                            "entity_type", "incarnation", "generation", "entity_base",
                            "rooms", "heartbeat_every"});
    LoadPlan out;
    out.url = !url_override.empty() ? url_override : plan.opt_str("run", "url");
    if (out.url.empty()) throw std::invalid_argument("plan needs [run] url (or --url)");
    if (!out.url.starts_with("tcp://"))
        throw std::invalid_argument("bots drive the framed TCP entity path only: " + out.url);
    out.concurrency = static_cast<unsigned>(opt_num(plan, "run", "concurrency", 1, 512, 1));
    out.requests = static_cast<unsigned>(opt_num(plan, "run", "requests", 0, 100000, 0));
    out.duration_s = opt_num(plan, "run", "duration_s", 0, 3600, 0);
    out.rate = opt_num(plan, "run", "rate", 0, 1000000, 0);
    out.ramp_ms = static_cast<unsigned>(opt_num(plan, "run", "ramp_ms", 0, 600000, 0));
    out.timeout_ms = static_cast<unsigned>(opt_num(plan, "run", "timeout_ms", 1, 60000, 5000));
    if (!out.requests && !out.duration_s)
        throw std::invalid_argument("plan needs [run] requests or duration_s");
    out.contract_file = plan.opt_str("scenario", "contract");
    if (out.contract_file.empty()) throw std::invalid_argument("plan needs [scenario] contract");
    out.service_token = plan.opt_str("scenario", "service_token");
    out.token_prefix = plan.opt_str("scenario", "token_prefix", "bot-");
    out.spawn = plan.boolean("scenario", "spawn", false);
    out.scope = static_cast<std::uint32_t>(opt_num(plan, "scenario", "scope", 1, 1000000, 1));
    out.entity_type = static_cast<std::uint32_t>(opt_num(plan, "scenario", "entity_type", 1, 63, 2));
    out.incarnation = static_cast<std::uint32_t>(opt_num(plan, "scenario", "incarnation", 1, 1000000, 1));
    out.generation = static_cast<std::uint32_t>(opt_num(plan, "scenario", "generation", 1, 1000000, 1));
    out.entity_base = opt_num(plan, "scenario", "entity_base", 1, 1000000000, 1000);
    out.rooms = static_cast<unsigned>(opt_num(plan, "scenario", "rooms", 1, 1024, 1));
    out.heartbeat_every = static_cast<unsigned>(opt_num(plan, "scenario", "heartbeat_every", 0, 1000000, 0));
    return out;
}

InjectPlan parse_inject(const PlanFile& plan, const std::string& url_override) {
    plan.allow("run", {"url", "timeout_ms", "mode"});
    plan.allow("inject", {"bad_frames", "late_acks", "slow_clients", "disconnect_storm",
                          "url_data", "url_metrics", "service_token", "contract",
                          "storm_rounds", "storm_burst", "slow_wait_ms",
                          "storm_entity_base", "entity_base", "spawn", "scope",
                          "entity_type", "incarnation", "generation"});
    plan.allow("scenario", {"contract", "service_token", "spawn", "scope", "entity_type",
                            "incarnation", "generation", "entity_base"});
    InjectPlan out;
    out.url = !url_override.empty() ? url_override : plan.opt_str("run", "url");
    if (out.url.empty()) throw std::invalid_argument("plan needs [run] url (or --url)");
    if (!out.url.starts_with("tcp://"))
        throw std::invalid_argument("bots drive the framed TCP entity path only: " + out.url);
    out.timeout_ms = opt_num(plan, "run", "timeout_ms", 1, 60000, 5000);
    // The scenario table is shared with load plans; [inject] overrides win.
    out.contract_file = plan.opt_str("inject", "contract", plan.opt_str("scenario", "contract"));
    if (out.contract_file.empty()) throw std::invalid_argument("plan needs [inject] contract");
    out.service_token = plan.opt_str("inject", "service_token",
        plan.opt_str("scenario", "service_token"));
    out.spawn = plan.boolean("inject", "spawn", plan.boolean("scenario", "spawn", true));
    out.scope = static_cast<std::uint32_t>(plan_num(plan, "scope", 1, 1000000, 1));
    out.entity_type = static_cast<std::uint32_t>(plan_num(plan, "entity_type", 1, 63, 2));
    out.incarnation = static_cast<std::uint32_t>(plan_num(plan, "incarnation", 1, 1000000, 1));
    out.generation = static_cast<std::uint32_t>(plan_num(plan, "generation", 1, 1000000, 1));
    out.entity_base = plan_num(plan, "entity_base", 1, 1000000000, 1000);
    out.url_data = plan.opt_str("inject", "url_data");
    if (!out.url_data.empty() && !out.url_data.starts_with("tcp://"))
        throw std::invalid_argument("url_data must be a tcp:// endpoint: " + out.url_data);
    out.url_metrics = plan.opt_str("inject", "url_metrics");
    if (!out.url_metrics.empty() && !out.url_metrics.starts_with("http://")
            && !out.url_metrics.starts_with("https://"))
        throw std::invalid_argument("url_metrics must be an http(s):// endpoint: " + out.url_metrics);
    out.bad_frames = plan.boolean("inject", "bad_frames", true);
    out.late_acks = plan.boolean("inject", "late_acks", true);
    out.slow_clients = plan.boolean("inject", "slow_clients", true);
    out.disconnect_storm = plan.boolean("inject", "disconnect_storm", true);
    out.storm_rounds = static_cast<unsigned>(opt_num(plan, "inject", "storm_rounds", 1, 100, 3));
    out.storm_burst = static_cast<unsigned>(opt_num(plan, "inject", "storm_burst", 1, 256, 8));
    out.slow_wait_ms = static_cast<unsigned>(opt_num(plan, "inject", "slow_wait_ms", 1, 30000, 300));
    out.storm_entity_base = opt_num(plan, "inject", "storm_entity_base", 1, 1000000000, 500000);
    return out;
}

std::string read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::invalid_argument("cannot read file: " + path);
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

// ---------------------------------------------------------------------------
// Raw framed leg (src/tcp_channel.hpp wire: u32 BE length + payload) for the
// scenarios that need mid-stream control: send without reading, oversize
// declarations, or an abrupt RST teardown.
// ---------------------------------------------------------------------------
class RawLeg {
public:
    explicit RawLeg(const std::string& url) {
        const auto colon = url.rfind(':');
        if (colon == std::string::npos || !url.starts_with("tcp://"))
            throw std::invalid_argument("raw leg expects tcp://IP:PORT");
        const auto host = url.substr(6, colon - 6), port = url.substr(colon + 1);
        tcp::resolver resolver(io_);
        boost::asio::connect(socket_, resolver.resolve(host, port));
        socket_.set_option(tcp::no_delay(true));
    }
    void send(std::string_view payload) {
        std::string frame(4, '\0');
        auto length = static_cast<std::uint32_t>(payload.size());
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(length & 255); length >>= 8; }
        frame += payload;
        boost::asio::write(socket_, boost::asio::buffer(frame));
    }
    // A declared length past max_frame is refused by the peer before the body
    // is read, so the carried payload stays tiny on purpose.
    void send_oversize(std::string_view payload, std::size_t declared) {
        std::string frame(4, '\0');
        auto length = static_cast<std::uint32_t>(declared);
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(length & 255); length >>= 8; }
        frame += payload;
        boost::asio::write(socket_, boost::asio::buffer(frame));
    }
    std::string receive() {
        std::string header(4, '\0');
        boost::asio::read(socket_, boost::asio::buffer(header.data(), header.size()));
        std::size_t length = 0;
        for (unsigned char ch : header) length = (length << 8) | ch;
        if (length > max_frame) throw std::runtime_error("peer announced oversized frame");
        std::string payload(length, '\0');
        boost::asio::read(socket_, boost::asio::buffer(payload.data(), payload.size()));
        return payload;
    }
    // Close with SO_LINGER 0 so the peer sees a reset, not a graceful FIN.
    void reset() {
        ::linger option{1, 0};
        ::setsockopt(socket_.native_handle(), SOL_SOCKET, SO_LINGER, &option, sizeof option);
        boost::system::error_code ignored;
        socket_.close(ignored);
    }
private:
    asio::io_context io_;
    tcp::socket socket_{io_};
};

// A service-identified leg on the entity host; hostile frames ride an
// identified session so they reach the entity endpoint, not the token gate.
class EntityLeg : public RawLeg {
public:
    EntityLeg(const InjectPlan& plan, unsigned connection) : RawLeg(plan.url) {
        if (!plan.service_token.empty()) {
            send("MLS1 bots inject-" + std::to_string(connection) + " " + plan.service_token);
            if (receive() != "MLS1 OK") throw std::runtime_error("MLS1 handshake rejected");
        }
    }
};

// One healthy business round-trip (MLS1, spawn, login, join, move) on a
// request/response client; the after-storm health probes reuse it. A failing
// step names itself in `why` so a broken guard reads back where it stopped.
bool healthy_probe(const InjectPlan& plan, const BusinessPlan& business, unsigned connection,
                   std::string* why = nullptr) {
    const auto step = [&](bool ok, const char* name, const std::string& reply) {
        if (!ok && why) *why = std::string(name) + " -> " + reply;
        return ok;
    };
    cli::ClientOptions options;
    options.url = plan.url;
    options.timeout_ms = plan.timeout_ms;
    auto client = cli::make_client(options);
    if (!plan.service_token.empty()) {
        const auto identified = client->request("MLS1 bots probe-" + std::to_string(connection)
            + " " + plan.service_token);
        if (!step(identified == "MLS1 OK", "identify", identified)) return false;
    }
    const auto entity_id = plan.entity_base + connection;
    // A repeated probe re-spawns its entity: both receipts mean the entity
    // runtime answered, so either keeps the probe healthy.
    if (plan.spawn) {
        const auto spawned = client->request("spawn " + std::to_string(entity_id));
        if (!step(spawned == "spawned" || spawned == "duplicate", "spawn", spawned)) return false;
    }
    std::uint64_t correlation = 0;
    const auto check = [&](const EntityValues& values, const ContractMethod& method,
                           const char* name) {
        const auto request = business.frame(entity_id, method, values,
            ++correlation, plan.timeout_ms);
        const auto reply = client->request(request);
        const auto verdict = reply.size() > 12
            ? decode_entity_reply(std::string_view(reply).substr(12)) : std::nullopt;
        const bool ok = verdict && !verdict->is_error;
        if (!ok && why) {
            static const char* hex = "0123456789abcdef";
            std::string bytes = " frame=";
            for (unsigned char ch : request) { bytes += hex[ch >> 4]; bytes += hex[ch & 15]; }
            *why = std::string(name) + " -> " + reply + bytes;
        }
        return ok;
    };
    return check({{"token", "probe-" + std::to_string(connection)}}, *business.login_method, "login")
        && check({{"room", std::int64_t(1)}}, *business.join_method, "joinRoom")
        && check({{"x", std::int64_t(connection)}, {"y", std::int64_t(1)}}, *business.move_method, "move");
}

double percentile(std::vector<double>& sorted, double p) {
    return sorted.empty() ? 0.0
        : sorted[std::min(sorted.size() - 1, static_cast<std::size_t>(p * (sorted.size() - 1)))];
}

// ---------------------------------------------------------------------------
// Load mode: the business scenario under a plan (concurrency, ramp, global
// rate, duration or per-connection requests), with per-message-type stats.
// ---------------------------------------------------------------------------
struct LoadStats {
    std::mutex mutex;
    std::array<unsigned long, 4> counters{}, failures{};
    StepLatencies latencies;
    unsigned failed_connections = 0;
    unsigned long total_ok = 0;
    std::string first_error;
};

std::unique_ptr<cli::Client> connect_bot(const LoadPlan& plan, unsigned connection) {
    cli::ClientOptions options;
    options.url = plan.url;
    options.timeout_ms = plan.timeout_ms;
    auto client = cli::make_client(options);
    if (!plan.service_token.empty()) {
        const auto verdict = client->request("MLS1 bots bots-" + std::to_string(connection)
            + " " + plan.service_token);
        if (verdict != "MLS1 OK") throw std::runtime_error("MLS1 handshake rejected: " + verdict);
    }
    if (plan.spawn && client->request("spawn " + std::to_string(plan.entity_base + connection)) != "spawned")
        throw std::runtime_error("entity spawn rejected (wanted 'spawned')");
    return client;
}

int run_load(const LoadPlan& plan) {
    EntityContract contract(read_file(plan.contract_file));
    BusinessPlan business(contract, plan.entity_type, plan.scope, plan.incarnation,
        plan.generation);
    business.token_prefix = plan.token_prefix;

    LoadStats stats;
    std::atomic<std::uint64_t> ticket{0};
    const auto started = std::chrono::steady_clock::now();
    // Duration mode: connections stop after the deadline; requests mode:
    // exactly `requests + 2` steps per connection (login + join + business).
    const auto deadline = started + std::chrono::seconds(plan.duration_s);
    const unsigned long steps = plan.duration_s
        ? std::numeric_limits<unsigned long>::max() : plan.requests + 2;
    std::vector<std::thread> threads;
    threads.reserve(plan.concurrency);
    for (unsigned c = 0; c < plan.concurrency; ++c) threads.emplace_back([&, c] {
        try {
            if (plan.ramp_ms) // spread the joins across the ramp window
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    static_cast<unsigned long long>(plan.ramp_ms) * c / plan.concurrency));
            auto client = connect_bot(plan, c);
            const auto entity_id = plan.entity_base + c;
            const auto room = 1 + c % plan.rooms;
            std::uint64_t correlation = 0;
            for (unsigned long step = 0; step < steps; ++step) {
                if (plan.duration_s && std::chrono::steady_clock::now() >= deadline) break;
                if (plan.rate) { // global pacing: ticket n goes out at start + n/rate
                    const auto slot = ticket.fetch_add(1);
                    const auto target = started + std::chrono::nanoseconds(
                        slot * 1000000000ULL / plan.rate);
                    if (std::chrono::steady_clock::now() < target)
                        std::this_thread::sleep_until(target);
                }
                try {
                    business_step(business, *client, entity_id, correlation, plan.timeout_ms,
                        static_cast<unsigned>(step), plan.heartbeat_every, room, c,
                        stats.counters, stats.mutex, &stats.latencies);
                    std::lock_guard lock(stats.mutex);
                    ++stats.total_ok;
                } catch (const std::exception& e) {
                    std::lock_guard lock(stats.mutex);
                    const unsigned slot = step == 0 ? 0 : step == 1 ? 1
                        : (plan.heartbeat_every
                            && step % plan.heartbeat_every == plan.heartbeat_every - 1) ? 3 : 2;
                    ++stats.failures[slot];
                    if (stats.first_error.empty()) stats.first_error = e.what();
                    break; // the connection is dead; a fresh one would skew stats
                }
            }
        } catch (const std::exception& e) {
            std::lock_guard lock(stats.mutex);
            ++stats.failed_connections;
            if (stats.first_error.empty()) stats.first_error = e.what();
        }
    });
    for (auto& thread : threads) thread.join();
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();

    std::array<std::vector<double>, 4> sorted;
    std::vector<double> all;
    for (unsigned slot = 0; slot < 4; ++slot) {
        sorted[slot] = stats.latencies[slot];
        std::sort(sorted[slot].begin(), sorted[slot].end());
        all.insert(all.end(), sorted[slot].begin(), sorted[slot].end());
    }
    std::sort(all.begin(), all.end());
    std::cout << std::fixed << std::setprecision(3)
        << "mode=load"
        << " planned=" << (plan.duration_s ? 0 : plan.concurrency * (plan.requests + 2))
        << " completed=" << stats.total_ok
        << " failed_connections=" << stats.failed_connections
        << " elapsed_seconds=" << seconds
        << " requests_per_second=" << (seconds ? stats.total_ok / seconds : 0)
        << " p50_ms=" << percentile(all, .50)
        << " p95_ms=" << percentile(all, .95)
        << " p99_ms=" << percentile(all, .99) << '\n';
    static const char* names[]{"login", "joinRoom", "move", "heartbeat"};
    for (unsigned slot = 0; slot < 4; ++slot)
        std::cout << "type=" << names[slot]
            << " ok=" << stats.counters[slot]
            << " fail=" << stats.failures[slot]
            << " p50_ms=" << percentile(sorted[slot], .50)
            << " p95_ms=" << percentile(sorted[slot], .95)
            << " p99_ms=" << percentile(sorted[slot], .99)
            << " max_ms=" << percentile(sorted[slot], 1) << '\n';
    if (!stats.first_error.empty()) std::cerr << "bots error: " << stats.first_error << '\n';
    return (stats.failed_connections || stats.total_ok == 0) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Inject mode: guards under hostile traffic. Every probe encodes the exact
// behaviour the implementation owes (the record fences of record.hpp, the
// malformed-envelope verdict of entity_endpoint.hpp, the frame bound of
// tcp_channel.hpp), and a guard only holds when the wire shows it.
// ---------------------------------------------------------------------------
struct Guard { std::string name; bool held = false; std::string detail; };
struct InjectReport {
    std::vector<Guard> guards;
    bool broken() const {
        for (const auto& guard : guards) if (!guard.held) return true;
        return false;
    }
};
void report(const InjectReport& outcome) {
    for (const auto& guard : outcome.guards)
        std::cout << "guard=" << guard.name
            << " verdict=" << (guard.held ? "held" : "BROKEN")
            << (guard.detail.empty() ? "" : " detail=" + guard.detail) << '\n';
}

// The endpoint owes "malformed envelope" INVALID for header bytes that do not
// decode (entity_endpoint.hpp): short frames, an out-of-range side byte.
bool malformed_envelope_held(EntityLeg& leg, const std::string& frame, std::string& detail) {
    leg.send(frame);
    const auto reply = leg.receive();
    if (!reply.starts_with("MLE1")) { detail = "reply missing MLE1 marker"; return false; }
    const auto verdict = decode_entity_reply(std::string_view(reply).substr(12));
    if (!verdict) { detail = "reply did not decode as EntityReply"; return false; }
    if (!verdict->is_error || verdict->error != EntityError::INVALID) {
        detail = std::string("expected INVALID, got ") + (verdict->is_error ? "another error" : "ok");
        return false;
    }
    if (verdict->reason != "malformed envelope") { detail = "reason: " + verdict->reason; return false; }
    return true;
}

void inject_bad_frames(const InjectPlan& plan, const BusinessPlan& business, InjectReport& out) {
    // Truncated envelope: the marker but fewer bytes than the 39-byte header.
    {
        EntityLeg leg(plan, 1);
        std::string detail;
        const bool held = malformed_envelope_held(leg, "MLE1bad", detail);
        out.guards.push_back({"bad-frames.truncated-envelope", held, detail});
    }
    // Full-size header with side=2: the decoder refuses side > 1.
    {
        EntityLeg leg(plan, 2);
        std::string bytes(39, '\0');
        bytes[20] = 2; // side sits after scope|type|entity|incarnation (4+4+8+4)
        std::string detail;
        const bool held = malformed_envelope_held(leg, "MLE1" + bytes, detail);
        out.guards.push_back({"bad-frames.bad-side", held, detail});
    }
    // A declared frame longer than max_frame must hit the transport bound
    // (src/tcp_channel.hpp) and tear the session down, never answer.
    {
        bool held = false;
        std::string detail;
        try {
            EntityLeg leg(plan, 3);
            leg.send_oversize("x", max_frame + 1);
            const auto reply = leg.receive(); // any reply at all is a broken bound
            detail = "answered " + std::to_string(reply.size()) + " bytes instead of refusing";
        } catch (const std::exception& e) {
            held = true; // EOF/reset/refusal: the frame never became traffic
            detail = e.what();
        }
        // The node keeps serving identified peers after the refusal: a full
        // healthy round-trip on a fresh leg.
        std::string why;
        const bool serving = healthy_probe(plan, business, 7, &why);
        held = held && serving;
        if (!serving) detail += " node stopped serving after refusal: " + why;
        out.guards.push_back({"bad-frames.oversize-refused", held, detail});
    }
}

void inject_late_acks(const InjectPlan& plan, InjectReport& out) {
    if (plan.url_data.empty()) {
        out.guards.push_back({"late-acks.durable-dedup-gap-stale", true,
            "skipped: no [inject] url_data"});
        return;
    }
    cli::ClientOptions options;
    options.url = plan.url_data;
    options.timeout_ms = plan.timeout_ms;
    auto client = cli::make_client(options);
    const auto ask = [&](const std::string& frame) { return client->request(frame); };
    std::string detail;
    bool held = true;
    // Fresh key: LOAD then the first accepted change, whose receipt is a
    // durable ACK because the injected data node runs with --journal.
    const auto load = ask("MLD1 LOAD bots late-1");
    if (load != "MLD1 EMPTY" && !load.starts_with("MLD1 STATE")) {
        detail += " load: " + load + "; ";
        held = false;
    }
    const auto first = ask("MLD1 SUBMIT bots late-1 gen 1 seq 1 storm-1");
    const bool durable = first == "MLD1 ACK durable bots late-1 gen 1 seq 1";
    held = held && durable;
    if (!durable) detail += " submit 1: " + first + "; ";
    // Advance inside the generation, then to a newer one: a cursor that a
    // late writer can arrive behind. (gen 0 itself is refused as INVALID
    // at parse — generations count from 1.)
    const std::pair<const char*, const char*> steps[] = {
        {"MLD1 SUBMIT bots late-1 gen 1 seq 2 storm-2", "MLD1 ACK durable bots late-1 gen 1 seq 2"},
        {"MLD1 SUBMIT bots late-1 gen 2 seq 3 handover", "MLD1 ACK durable bots late-1 gen 2 seq 3"},
    };
    for (const auto& [request, expect] : steps) {
        const auto advance = ask(request);
        if (advance != expect) { detail += std::string(" advance: ") + advance + "; "; held = false; }
    }
    // A late writer of the retired generation must hit the generation fence.
    const auto stale = ask("MLD1 SUBMIT bots late-1 gen 1 seq 4 late-writer");
    const bool stale_held = stale == "MLD1 ERR STALE gen 2";
    held = held && stale_held;
    if (!stale_held) detail += " stale: " + stale + "; ";
    // The home side fills no gaps: the skipped sequence is refused by name.
    const auto gap = ask("MLD1 SUBMIT bots late-1 gen 2 seq 5 gap");
    const bool gap_held = gap == "MLD1 ERR SEQ seq 4";
    held = held && gap_held;
    if (!gap_held) detail += " gap: " + gap + "; ";
    // At-least-once resends dedup to a second durable ACK of the same cursor.
    const auto dup = ask("MLD1 SUBMIT bots late-1 gen 2 seq 3 handover");
    const bool dup_held = dup == "MLD1 ACK durable bots late-1 gen 2 seq 3";
    held = held && dup_held;
    if (!dup_held) detail += " dedup: " + dup + "; ";
    // Concurrent submitters under storm: every key advances 1..N in order.
    constexpr unsigned storm_keys = 16, storm_steps = 40;
    std::atomic<unsigned> storm_bad{0};
    std::vector<std::thread> workers;
    for (unsigned k = 0; k < storm_keys; ++k) workers.emplace_back([&, k] {
        try {
            cli::ClientOptions worker_options;
            worker_options.url = plan.url_data;
            worker_options.timeout_ms = plan.timeout_ms;
            auto worker = cli::make_client(worker_options);
            for (unsigned seq = 1; seq <= storm_steps; ++seq)
                if (worker->request("MLD1 SUBMIT bots storm-" + std::to_string(k)
                        + " gen 1 seq " + std::to_string(seq) + " s" + std::to_string(seq))
                        != "MLD1 ACK durable bots storm-" + std::to_string(k)
                        + " gen 1 seq " + std::to_string(seq))
                    ++storm_bad;
        } catch (const std::exception&) { ++storm_bad; }
    });
    for (auto& worker : workers) worker.join();
    const bool storm_held = storm_bad == 0;
    held = held && storm_held;
    if (!storm_held) detail += " storm rejected " + std::to_string(storm_bad) + " changes; ";
    // The accepted cursors never regressed: every storm key ends at N. (A
    // stored cursor answers STATE with the payload appended, so the check is
    // a prefix match on the cursor itself.)
    for (unsigned k = 0; k < storm_keys && held; ++k) {
        const auto state = ask("MLD1 LOAD bots storm-" + std::to_string(k));
        if (state.rfind("MLD1 STATE gen 1 seq " + std::to_string(storm_steps), 0) != 0) {
            detail += " final cursor: " + state;
            held = false;
        }
    }
    out.guards.push_back({"late-acks.durable-dedup-gap-stale", held, detail});
}

void inject_slow_clients(const InjectPlan& plan, const BusinessPlan& business, InjectReport& out) {
    // Slow readers hold their replies for slow_wait_ms; the session plane owes
    // back pressure (one frame in flight per relay leg), not dropped traffic:
    // the reply still arrives and decodes OK after the stall.
    constexpr unsigned slow = 6, hot_steps = 10;
    std::atomic<unsigned> slow_ok{0}, slow_bad{0}, hot_ok{0};
    std::vector<std::thread> readers;
    for (unsigned c = 0; c < slow; ++c) readers.emplace_back([&, c] {
        try {
            EntityLeg leg(plan, 100 + c);
            const auto entity_id = plan.entity_base + 1000 + c;
            if (plan.spawn) {
                leg.send("spawn " + std::to_string(entity_id));
                if (leg.receive() != "spawned") throw std::runtime_error("spawn rejected");
            }
            const auto request = business.frame(entity_id, *business.move_method,
                {{"x", std::int64_t(c)}, {"y", std::int64_t(1)}}, c + 1, plan.timeout_ms);
            leg.send(request);
            std::this_thread::sleep_for(std::chrono::milliseconds(plan.slow_wait_ms));
            const auto reply = leg.receive();
            const auto verdict = decode_entity_reply(std::string_view(reply).substr(12));
            (verdict && !verdict->is_error) ? ++slow_ok : ++slow_bad;
        } catch (const std::exception&) { ++slow_bad; }
    });
    // Healthy probes keep serving while the readers stall. Each probe rides
    // its own entity: the correlation fence is at-most-once per entity, so a
    // repeated probe of one entity would CONFLICT on its own past frames.
    std::mutex detail_mutex;
    std::string hot_why, slow_why;
    readers.emplace_back([&] {
        try {
            std::string why;
            for (unsigned step = 0; step < hot_steps; ++step)
                if (!healthy_probe(plan, business, 200 + step, &why)) {
                    const std::lock_guard<std::mutex> guard(detail_mutex);
                    hot_why = why;
                    return;
                } else ++hot_ok;
        } catch (const std::exception& e) {
            const std::lock_guard<std::mutex> guard(detail_mutex);
            hot_why = e.what();
        }
    });
    for (auto& reader : readers) reader.join();
    const bool held = slow_ok == slow && hot_ok == hot_steps;
    const std::lock_guard<std::mutex> guard(detail_mutex);
    out.guards.push_back({"slow-clients.backpressure-reply-after-stall", held,
        "slow_ok=" + std::to_string(slow_ok) + " slow_bad=" + std::to_string(slow_bad)
        + " hot_ok=" + std::to_string(hot_ok)
        + (hot_why.empty() ? "" : " hot_stopped: " + hot_why)});
}

void inject_disconnect_storm(const InjectPlan& plan, const BusinessPlan& business,
                             InjectReport& out) {
    // Bursts of connect -> identify -> spawn -> first entity frame -> RST:
    // the node retires every half-done session and keeps serving afterwards.
    const auto storms = plan.storm_rounds * plan.storm_burst;
    std::atomic<unsigned> launched{0};
    std::vector<std::thread> bursts;
    for (unsigned r = 0; r < plan.storm_rounds; ++r) bursts.emplace_back([&, r] {
        for (unsigned i = 0; i < plan.storm_burst; ++i) {
            const auto entity_id = plan.storm_entity_base + r * plan.storm_burst + i;
            try {
                EntityLeg leg(plan, 1000 + r * plan.storm_burst + i);
                if (plan.spawn) {
                    leg.send("spawn " + std::to_string(entity_id));
                    if (leg.receive() != "spawned") continue;
                }
                leg.send(business.frame(entity_id, *business.move_method,
                    {{"x", std::int64_t(r)}, {"y", std::int64_t(i)}}, entity_id,
                    plan.timeout_ms));
                leg.reset(); // abandon the in-flight move mid-request
                ++launched;
            } catch (const std::exception&) {}
        }
    });
    for (auto& burst : bursts) burst.join();
    std::string detail = "storms=" + std::to_string(launched) + "/" + std::to_string(storms);
    bool held = launched > 0;
    // After the storm a full healthy round-trip must pass...
    std::string why;
    const bool serving = healthy_probe(plan, business, 400, &why);
    held = held && serving;
    if (!serving) detail += " node stopped serving after storm: " + why;
    // ...and the metrics exposer must still answer with a ready node.
    if (held && !plan.url_metrics.empty()) {
        try {
            HttpRequest probe;
            probe.url = plan.url_metrics + "/metrics";
            const auto page = http_request(probe);
            const bool ready = page.status == 200
                && page.body.find("mistlane_ready") != std::string::npos;
            held = held && ready;
            if (!ready) detail += " metrics status=" + std::to_string(page.status);
        } catch (const std::exception& e) {
            held = false;
            detail += " metrics unreachable: ";
            detail += e.what();
        }
    }
    out.guards.push_back({"disconnect-storm.survives-and-serves", held, detail});
}

int run_inject(const InjectPlan& plan) {
    EntityContract contract(read_file(plan.contract_file));
    BusinessPlan business(contract, plan.entity_type, plan.scope, plan.incarnation,
        plan.generation);
    InjectReport outcome;
    if (plan.bad_frames) inject_bad_frames(plan, business, outcome);
    if (plan.late_acks) inject_late_acks(plan, outcome);
    if (plan.slow_clients) inject_slow_clients(plan, business, outcome);
    if (plan.disconnect_storm) inject_disconnect_storm(plan, business, outcome);
    report(outcome);
    return outcome.broken() ? 1 : 0;
}

}
}

int main(int argc, char** argv) {
    try {
        std::string plan_file, url_override, mode_override;
        for (int i = 1; i < argc; ++i) {
            const std::string key = argv[i];
            if (key == "--help") {
                std::cout << "mistlane-bots --plan FILE [--url tcp://IP:PORT] [--mode load|inject]\n"
                    "Standalone load and fault-injection driver for mistlane test nodes.\n"
                    "Plan files are a small TOML subset; see tools/bots/README.md.\n";
                return 0;
            }
            if (++i >= argc) throw std::invalid_argument("missing value for " + key);
            const std::string value = argv[i];
            if (key == "--plan") plan_file = value;
            else if (key == "--url") url_override = value;
            else if (key == "--mode") mode_override = value;
            else throw std::invalid_argument("unknown bots option: " + key);
        }
        if (plan_file.empty()) throw std::invalid_argument("--plan FILE is required");
        const mistlane::PlanFile plan(plan_file);
        const auto mode = !mode_override.empty() ? mode_override : plan.opt_str("run", "mode", "load");
        if (mode == "load") return mistlane::run_load(mistlane::parse_load(plan, url_override));
        if (mode == "inject") return mistlane::run_inject(mistlane::parse_inject(plan, url_override));
        throw std::invalid_argument("unknown mode " + mode + " (load or inject)");
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << '\n';
        return 1;
    }
}
