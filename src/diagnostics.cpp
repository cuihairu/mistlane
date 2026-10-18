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
        event_loop_lag_ = &prometheus::BuildGauge().Name("mistlane_event_loop_lag_seconds")
            .Help("Delay between scheduled event-loop ticks.").Labels(labels).Register(*registry_).Add({});
        process_rss_ = &prometheus::BuildGauge().Name("mistlane_process_resident_memory_bytes")
            .Help("Resident memory reported by procfs.").Labels(labels).Register(*registry_).Add({});
        rejected_ = &prometheus::BuildCounter().Name("mistlane_admission_rejections_total")
            .Help("Sessions rejected by admission control.").Labels(labels).Register(*registry_).Add({});
        lua_callback_seconds_ = &prometheus::BuildHistogram().Name("mistlane_lua_callback_seconds")
            .Help("Lua callback duration in seconds.").Labels(labels).Register(*registry_)
            .Add({}, prometheus::Histogram::BucketBoundaries{0.0001, 0.0005, 0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1.0});
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
void Diagnostics::reject(std::string_view) { if (rejected_) rejected_->Increment(); }
void Diagnostics::add_payload_bytes(std::string_view, std::size_t) {}
void Diagnostics::observe_lua_callback(std::string_view, std::chrono::steady_clock::duration elapsed) {
    if (lua_callback_seconds_)
        lua_callback_seconds_->Observe(std::chrono::duration<double>(elapsed).count());
}
void Diagnostics::set_event_loop_lag(std::chrono::steady_clock::duration lag) {
    if (event_loop_lag_) event_loop_lag_->Set(std::chrono::duration<double>(lag).count());
}
void Diagnostics::refresh_process_memory() {
    if (!process_rss_) return;
    std::ifstream statm("/proc/self/statm");
    std::uint64_t total = 0, resident = 0;
    if (statm >> total >> resident)
        process_rss_->Set(static_cast<double>(resident) * static_cast<double>(::sysconf(_SC_PAGESIZE)));
}
}
