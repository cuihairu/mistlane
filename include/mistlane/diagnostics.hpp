#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string_view>

namespace crashpad { class CrashpadClient; class CrashReportDatabase; }
namespace prometheus {
class Counter;
class Exposer;
class Gauge;
class Histogram;
class Registry;
}

namespace mistlane {
struct Config;
class Diagnostics {
public:
    explicit Diagnostics(const Config& config);
    ~Diagnostics();
    Diagnostics(const Diagnostics&) = delete;
    Diagnostics& operator=(const Diagnostics&) = delete;

    void set_ready(bool ready);
    void set_active_sessions(std::size_t count);
    void reject(std::string_view reason);
    void add_payload_bytes(std::string_view direction, std::size_t bytes);
    void observe_lua_callback(std::string_view hook, std::chrono::steady_clock::duration elapsed);
    void set_event_loop_lag(std::chrono::steady_clock::duration lag);
    void refresh_process_memory();

private:
    std::unique_ptr<crashpad::CrashpadClient> crashpad_client_;
    std::unique_ptr<crashpad::CrashReportDatabase> crashpad_database_;
    std::shared_ptr<prometheus::Registry> registry_;
    std::unique_ptr<prometheus::Exposer> exposer_;
    prometheus::Gauge* ready_ = nullptr;
    prometheus::Gauge* active_sessions_ = nullptr;
    prometheus::Gauge* event_loop_lag_ = nullptr;
    prometheus::Gauge* process_rss_ = nullptr;
    prometheus::Counter* rejected_ = nullptr;
    prometheus::Counter* rx_bytes_ = nullptr;
    prometheus::Counter* tx_bytes_ = nullptr;
    prometheus::Histogram* lua_callback_seconds_ = nullptr;
};
}
