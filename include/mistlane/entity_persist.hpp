// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "mistlane/entity_property.hpp"
#include <set>
#include <string>
#include <vector>

namespace mistlane {
// One forced save: the fence values it asserted plus the persistent snapshot
// it captured. save_id identifies the in-flight save for confirm().
struct SaveReceipt {
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    std::uint64_t save_id = 0;
};

struct SaveOutcome {
    bool ok = false;
    EntityError error = EntityError::INVALID;
    std::string reason;  // "generation" | "revision" | "in-flight" | ""
    SaveReceipt receipt;
    EntityValues snapshot;
};

struct RestoreReport {
    bool ok = false;
    EntityError error = EntityError::INVALID;
    std::string refused;                  // the property that forced a whole-load refusal
    std::vector<std::string> defaulted;   // contract grew; these took descriptor defaults
};

// Home-data persistence model over one controlled store (the in-process half
// of the persistence item): writes keep a revision that advances only when a
// change lands on a persistent property, so transient churn never marks a
// record dirty and refused writes never do. The single forced-save entry
// captures the descriptor's persistent properties under the writer's hosting
// generation and the revision it observed; sending never clears dirty — only
// confirm() of the in-flight receipt does, and only up to the revision the
// snapshot actually captured. A save under a foreign generation is STALE, one
// against a moved-on revision is CONFLICT (another writer landed first), and
// a second save may not overlap an unconfirmed one. restore() is the load
// path with explicit version conversion: keys the contract no longer declares
// or never persisted refuse the whole load, missing persistent properties
// keep their descriptor defaults, and a generation may only move forward.
class PersistentStore {
public:
    explicit PersistentStore(PropertyStore& store) : store_(store) {}

    // A fresh or strictly higher hosting assignment becomes current. Lower or
    // equal is refused — an aborted generation is never reused.
    bool assume(std::uint64_t generation) {
        error_.clear();
        if (!generation || (generation_ && generation <= generation_)) {
            error_ = "generation "+std::to_string(generation)+" does not advance "
                     +std::to_string(generation_);
            return false;
        }
        generation_ = generation;
        return true;
    }

    // Pass-through writes; the revision advances only when the change lands on
    // a persistent property.
    bool set(const std::string& name, EntityValue value) {
        if (!store_.set(name, std::move(value))) return false;
        bump(name);
        return true;
    }
    bool set_path(const std::string& name, TreePath path, EntityValue value) {
        if (!store_.set_path(name, std::move(path), std::move(value))) return false;
        bump(name);
        return true;
    }
    bool insert_at(const std::string& name, const TreePath& path, std::size_t position, EntityValue value) {
        if (!store_.insert_at(name, path, position, std::move(value))) return false;
        bump(name);
        return true;
    }
    bool remove_at(const std::string& name, const TreePath& path, std::size_t position) {
        if (!store_.remove_at(name, path, position)) return false;
        bump(name);
        return true;
    }

    // The one forced-save entry. expected_revision is the revision the caller
    // observed before encoding; a save against anything else is refused
    // instead of capturing a snapshot that is already obsolete.
    SaveOutcome save(std::uint64_t generation, std::uint64_t expected_revision) {
        SaveOutcome out;
        if (!generation_) { out.error = EntityError::NOT_READY; out.reason = "no hosting assignment"; return out; }
        if (generation != generation_) { out.error = EntityError::STALE; out.reason = "generation"; return out; }
        if (in_flight_.save_id) { out.error = EntityError::CONFLICT; out.reason = "in-flight"; return out; }
        if (expected_revision != revision_) { out.error = EntityError::CONFLICT; out.reason = "revision"; return out; }
        out.snapshot = persistent_snapshot();
        out.receipt = SaveReceipt{generation_, revision_, ++save_counter_};
        in_flight_ = out.receipt;
        out.ok = true;
        return out;
    }

    // The durability receipt. Only the in-flight save id confirms; the
    // committed revision moves to the snapshot's revision, so writes that
    // landed after the save stay dirty.
    bool confirm(std::uint64_t save_id) {
        error_.clear();
        if (!in_flight_.save_id || save_id != in_flight_.save_id) {
            error_ = "save id "+std::to_string(save_id)+" is not the in-flight save";
            return false;
        }
        committed_revision_ = in_flight_.revision;
        in_flight_ = SaveReceipt{};
        return true;
    }

    // Load path with explicit version conversion. Any snapshot key the
    // contract no longer declares, or that was never persistent, refuses the
    // whole load — never a silent merge; declared persistent keys absent from
    // the snapshot keep the defaults loaded at construction; the generation
    // only moves forward.
    RestoreReport restore(std::uint64_t generation, std::uint64_t committed_revision, const EntityValues& snapshot) {
        RestoreReport out;
        if (!generation) { out.error = EntityError::INVALID; out.refused = "generation"; return out; }
        if (generation_ && generation < generation_) {
            out.error = EntityError::STALE;
            out.refused = "generation "+std::to_string(generation);
            return out;
        }
        const auto& props = store_.entity().properties;
        for (const auto& [name, value] : snapshot) {
            (void)value;
            const auto it = std::find_if(props.begin(), props.end(),
                                         [&](const ContractProperty& p) { return p.name == name; });
            if (it == props.end() || !it->persistent) { out.refused = name; return out; }
        }
        for (const auto& [name, value] : snapshot) {
            if (!store_.set(name, EntityValue(value))) { out.refused = name; return out; }
        }
        for (const auto& p : props)
            if (p.persistent && !snapshot.count(p.name)) out.defaulted.push_back(p.name);
        generation_ = generation;
        revision_ = committed_revision;
        committed_revision_ = committed_revision;
        in_flight_ = SaveReceipt{};
        out.ok = true;
        return out;
    }

    // The persistent subset of the current values — what save() captures and
    // what a data peer would hold for this record.
    EntityValues persistent_snapshot() const {
        EntityValues out;
        const auto& props = store_.entity().properties;
        for (const auto& p : props) {
            if (!p.persistent) continue;
            const auto* value = store_.get(p.name);
            if (value) out.emplace(p.name, EntityValue(*value));
        }
        return out;
    }

    bool dirty() const { return revision_ != committed_revision_; }
    std::uint64_t revision() const { return revision_; }
    std::uint64_t committed_revision() const { return committed_revision_; }
    std::uint64_t generation() const { return generation_; }
    const std::string& error() const { return error_; }
private:
    void bump(const std::string& name) {
        const auto& props = store_.entity().properties;
        const auto it = std::find_if(props.begin(), props.end(),
                                     [&](const ContractProperty& p) { return p.name == name; });
        if (it != props.end() && it->persistent) ++revision_;
    }

    PropertyStore& store_;
    std::uint64_t generation_ = 0, revision_ = 0, committed_revision_ = 0, save_counter_ = 0;
    SaveReceipt in_flight_;
    std::string error_;
};
}
