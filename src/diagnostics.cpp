// SPDX-License-Identifier: Apache-2.0
#include "mistlane/diagnostics.hpp"
#include "mistlane/node.hpp"

#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>

#include <crashpad/client/crash_report_database.h>
#include <crashpad/client/crashpad_client.h>
#include <crashpad/client/settings.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <cstdint>
#include <unistd.h>

namespace mistlane {
Diagnostics::Diagnostics(const Config& config) {
    if (config.metrics_enabled) {
        registry_ = std::make_shared<prometheus::Registry>();
        const auto labels = prometheus::Labels{{"role", config.role}};
        ready_ = &prometheus::BuildGauge().Name("mistlane_ready").Help("Whether this process is ready.")
            .Labels(labels).Register(*registry_).Add({});
        active_sessions_ = &prometheus::BuildGauge().Name("mistlane_active_sessions")
            .Help("Current accepted client sessions.").Labels(labels).Register(*registry_).Add({});
        registry_members_ = &prometheus::BuildGauge().Name("mistlane_registry_members")
            .Help("Live registered members of this coordinator.").Labels(labels).Register(*registry_).Add({});
        event_loop_lag_max_ = &prometheus::BuildGauge().Name("mistlane_event_loop_lag_max_seconds")
            .Help("Largest observed delay between scheduled event-loop ticks.")
            .Labels(labels).Register(*registry_).Add({});
        process_rss_ = &prometheus::BuildGauge().Name("mistlane_process_resident_memory_bytes")
            .Help("Resident memory reported by procfs.").Labels(labels).Register(*registry_).Add({});
        rejected_ = &prometheus::BuildCounter().Name("mistlane_admission_rejections_total")
            .Help("Sessions rejected by admission control.").Labels(labels).Register(*registry_).Add({});
        auto& messages = prometheus::BuildCounter().Name("mistlane_messages_total")
            .Help("Messages read from or written to clients.").Labels(labels).Register(*registry_);
        messages_rx_ = &messages.Add({{"direction", "rx"}});
        messages_tx_ = &messages.Add({{"direction", "tx"}});
        auto& payload = prometheus::BuildCounter().Name("mistlane_payload_bytes_total")
            .Help("Payload bytes read from or written to clients.").Labels(labels).Register(*registry_);
        rx_bytes_ = &payload.Add({{"direction", "rx"}});
        tx_bytes_ = &payload.Add({{"direction", "tx"}});
        upstream_errors_ = &prometheus::BuildCounter().Name("mistlane_upstream_errors_total")
            .Help("Upstream connect, write, read or handshake failures.")
            .Labels(labels).Register(*registry_).Add({});
        upstream_timeouts_ = &prometheus::BuildCounter().Name("mistlane_upstream_timeouts_total")
            .Help("Session deadlines expired while waiting on an upstream.")
            .Labels(labels).Register(*registry_).Add({});
        lua_callback_seconds_ = &prometheus::BuildHistogram().Name("mistlane_lua_callback_seconds")
            .Help("Lua callback duration in seconds.").Labels(labels).Register(*registry_)
            .Add({}, prometheus::Histogram::BucketBoundaries{0.0001, 0.0005, 0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1.0});
        request_seconds_ = &prometheus::BuildHistogram().Name("mistlane_request_seconds")
            .Help("Client message to reply latency in seconds.").Labels(labels).Register(*registry_)
            .Add({}, prometheus::Histogram::BucketBoundaries{0.0001, 0.0005, 0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1.0, 5.0, 10.0});
        event_loop_lag_ = &prometheus::BuildHistogram().Name("mistlane_event_loop_lag_seconds")
            .Help("Delay between scheduled and fired event-loop ticks.").Labels(labels).Register(*registry_)
            .Add({}, prometheus::Histogram::BucketBoundaries{0.0005, 0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0});
        exposer_ = std::make_unique<prometheus::Exposer>(config.metrics_listen.address + ":" +
            std::to_string(config.metrics_listen.port));
        exposer_->RegisterCollectable(registry_);
    }

    if (!config.crashpad_handler.empty()) {
        namespace fs = std::filesystem;
        const fs::path database_path(config.crashpad_database);
        const auto metrics_path = database_path / "handler-metrics";
        fs::create_directories(database_path);
        fs::create_directories(metrics_path);
        crashpad_database_ = crashpad::CrashReportDatabase::Initialize(
            base::FilePath(database_path.string()));
        if (!crashpad_database_ || !crashpad_database_->GetSettings()->SetUploadsEnabled(false))
            throw std::runtime_error("cannot initialize local Crashpad database with uploads disabled");
        crashpad_client_ = std::make_unique<crashpad::CrashpadClient>();
        const bool started = crashpad_client_->StartHandler(
            base::FilePath(config.crashpad_handler), base::FilePath(database_path.string()),
            base::FilePath(metrics_path.string()), "", {{"role", config.role}, {"product", "mistlane"}},
            {}, true, false);
        if (!started) throw std::runtime_error("cannot start Crashpad handler");
    }
}

Diagnostics::~Diagnostics() = default;
void Diagnostics::set_ready(bool ready) { if (ready_) ready_->Set(ready ? 1.0 : 0.0); }
void Diagnostics::set_active_sessions(std::size_t count) {
    if (active_sessions_) active_sessions_->Set(static_cast<double>(count));
}
void Diagnostics::set_registry_members(std::size_t count) {
    if (registry_members_) registry_members_->Set(static_cast<double>(count));
}
void Diagnostics::reject(std::string_view) { if (rejected_) rejected_->Increment(); }
void Diagnostics::add_message(std::string_view direction) {
    if (direction == "tx") { if (messages_tx_) messages_tx_->Increment(); }
    else if (messages_rx_) messages_rx_->Increment();
}
void Diagnostics::add_payload_bytes(std::string_view direction, std::size_t bytes) {
    if (direction == "tx") { if (tx_bytes_) tx_bytes_->Increment(static_cast<double>(bytes)); }
    else if (rx_bytes_) rx_bytes_->Increment(static_cast<double>(bytes));
}
void Diagnostics::observe_lua_callback(std::string_view, std::chrono::steady_clock::duration elapsed) {
    if (lua_callback_seconds_)
        lua_callback_seconds_->Observe(std::chrono::duration<double>(elapsed).count());
}
void Diagnostics::observe_request(std::chrono::steady_clock::duration elapsed) {
    if (request_seconds_)
        request_seconds_->Observe(std::chrono::duration<double>(elapsed).count());
}
void Diagnostics::upstream_error() { if (upstream_errors_) upstream_errors_->Increment(); }
void Diagnostics::upstream_timeout() { if (upstream_timeouts_) upstream_timeouts_->Increment(); }
void Diagnostics::observe_event_loop_lag(std::chrono::steady_clock::duration lag) {
    const double seconds = std::chrono::duration<double>(lag).count();
    if (event_loop_lag_) event_loop_lag_->Observe(seconds);
    if (event_loop_lag_max_) event_loop_lag_max_->Set(std::max(event_loop_lag_max_->Value(), seconds));
}
void Diagnostics::refresh_process_memory() {
    if (!process_rss_) return;
    std::ifstream statm("/proc/self/statm");
    std::uint64_t total = 0, resident = 0;
    if (statm >> total >> resident)
        process_rss_->Set(static_cast<double>(resident) * static_cast<double>(::sysconf(_SC_PAGESIZE)));
}
}
