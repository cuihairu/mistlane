// SPDX-License-Identifier: Apache-2.0
#include "mistlane/cli.hpp"
#include "mistlane/transport.hpp"
#include "cli_client.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <optional>
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
                "Serial request/response per connection. Errors produce a nonzero exit code.\n";
            return 0;
        }
        if (key == "--expect-echo") { echo = true; continue; }
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
        else throw std::invalid_argument("unknown bot option: " + key);
    }
    if (options.method != "GET" && options.method != "POST")
        throw std::invalid_argument("bot HTTP method must be GET or POST");
    if (!join.empty() && !options.url.starts_with("tcp://"))
        throw std::invalid_argument("--join currently applies only to internal TCP/cross");
    if (clients*count > 100000) throw std::invalid_argument("bot allows at most 100000 total requests");
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
    const auto started = std::chrono::steady_clock::now();
    for (unsigned c = 0; c < clients; ++c) threads.emplace_back([&] {
        try {
            auto client = cli::make_client(options);
            if (!join.empty() && client->request(join) != "MLC1 OK 1")
                throw std::runtime_error("cross handshake rejected (bot expects route epoch 1)");
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
    if (clients == 1 && count == 1 && !failures) std::cout << "response=" << std::quoted(first_response) << '\n';
    if (!first_error.empty()) std::cerr << "bot error: " << first_error << '\n';
    return failures ? 1 : 0;
}
}
