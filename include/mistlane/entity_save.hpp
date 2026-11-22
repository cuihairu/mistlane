// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "mistlane/entity_state.hpp"
#include <chrono>
#include <functional>
#include <vector>

namespace mistlane {
// The periodic-save driver (docs/entity-contract.md, "Loading and persistence"):
// the one component that turns "a record is dirty" into the single forced-save
// entry, on a period, under an injected clock. It owns no entity state and no
// backend — each registered store is already bound to its own backend and
// carries its own fences — and it never re-saves a record on its own initiative:
// a tick touches a store only when it is dirty, and a save that is refused
// (unconfirmed prior save, a moved-on revision, a changed generation, or no
// hosting assignment yet) is reported and retried on a later period, while a
// persist that never landed aborts its own receipt so the record stays dirty.
// This is the driver half of "Periodic, transfer and shutdown saves use the
// same mechanism"; the runtime wiring landed too — an entity-hosting node owns
// one chain per live declared entity and drives the periodic pass from its
// tick plus the shutdown pass from stop() (see EntityRuntime).
class PeriodicSaveDriver {
public:
    using Clock = std::function<std::uint64_t()>;
    explicit PeriodicSaveDriver(std::uint64_t period_ms, Clock now_ms = default_clock())
        : period_(period_ms), now_ms_(std::move(now_ms)) {}

    // Register a store to periodically save. Not owned: the caller keeps the
    // store (and the backend it points at) alive for the driver's lifetime.
    void add(EntityStateStore* store) { stores_.push_back(store); }
    void remove(EntityStateStore* store) { std::erase(stores_, store); }
    std::size_t registered() const { return stores_.size(); }

    // What one tick did. `attempted` is the number of dirty stores the tick
    // tried to save; the rest of the counts are the ways a save was refused or
    // failed, which the caller may want to report.
    struct Report {
        std::size_t attempted = 0;  // dirty stores given a save this tick
        std::size_t landed = 0;     // a save confirmed
        std::size_t in_flight = 0;  // skipped: an unconfirmed save is pending
        std::size_t conflict = 0;   // skipped: the observed revision moved on
        std::size_t stale = 0;      // skipped: the record's generation changed
        std::size_t not_ready = 0;  // skipped: no hosting assignment yet
        std::size_t failed = 0;     // persist threw, or the snapshot was not encodable
        std::size_t skipped() const { return in_flight + conflict + stale + not_ready; }
        std::size_t refused() const { return skipped() + failed; }
    };

    // One period. The first tick after construction fires immediately; later
    // ticks fire only once `period_ms` has elapsed, so a slow tick never saves
    // twice. The result is per tick, so a caller logs or exports it.
    Report tick() {
        Report report;
        const auto now = now_ms_();
        if (has_ticked_ && now - last_ < period_) return report;
        last_ = now;
        has_ticked_ = true;
        return attempt(report);
    }

    // The shutdown half of the same mechanism: every dirty record goes through
    // the single forced-save entry once, ignoring the period. Same
    // classification, so a stop never silently drops a refused save — the
    // report names what did not land.
    Report flush() { return attempt({}); }

private:
    Report attempt(Report report) {
        for (auto* store : stores_) {
            if (!store->dirty()) continue;
            ++report.attempted;
            try {
                const auto outcome = store->save(store->generation(), store->revision());
                if (outcome.ok) { ++report.landed; continue; }
                if (outcome.reason == "in-flight") { ++report.in_flight; continue; }
                if (outcome.reason == "revision") { ++report.conflict; continue; }
                if (outcome.error == EntityError::STALE) { ++report.stale; continue; }
                if (outcome.error == EntityError::NOT_READY) { ++report.not_ready; continue; }
                ++report.failed;
            } catch (...) {
                // A backend failure is counted, not rethrown: the store already
                // aborted its own receipt, so the record stays dirty and the
                // next period retries it. The count keeps the failure visible.
                ++report.failed;
            }
        }
        return report;
    }

    static Clock default_clock() {
        return [] {
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        };
    }
    std::uint64_t period_ = 0;
    Clock now_ms_;
    std::uint64_t last_ = 0;
    bool has_ticked_ = false;
    std::vector<EntityStateStore*> stores_;
};
}
