// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "mistlane/entity_property.hpp"
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mistlane {
// Wire-shaped delta (the property design's representation rule): the top-level
// property id, a path of struct field ids and zero-based array indexes, the
// operation and the new value, plus the receiver's replication sequence number.
// INSERT and REMOVE are ordered slice operations addressing the array at
// (property_id, path) at a zero-based position; SET carries the leaf value at
// the path. Ids and indexes are the only stable addresses on the wire, so no
// traversal order can drift between languages.
struct SyncStep {
    bool by_index = false;
    std::uint32_t field_id = 0;
    std::size_t index = 0;
};
inline bool operator==(const SyncStep& a, const SyncStep& b) {
    return a.by_index == b.by_index && (a.by_index ? a.index == b.index : a.field_id == b.field_id);
}
using SyncPath = std::vector<SyncStep>;

struct SyncDelta {
    enum class Kind { SET, INSERT, REMOVE };
    std::uint32_t property_id = 0;
    SyncPath path;
    Kind kind = Kind::SET;
    std::size_t position = 0;
    EntityValue value{std::int64_t{0}};
    std::uint64_t sequence = 0;
};

// One message on an observer's stream: the full visible baseline established by
// entry, re-entry or resync, or an ordered run of deltas. Sequences are
// per-observer — invisible changes are not numbered for an observer who will
// never see them, so every stream the receiver gets is gapless.
struct SyncMessage {
    enum class Kind { BASELINE, DELTAS };
    Kind kind = Kind::BASELINE;
    std::uint64_t sequence = 0;
    EntityValues baseline;
    std::vector<SyncDelta> deltas;
};

// One receiver's replica: it replays the deltas that reached it into its own
// controlled store, so a delta that could never ride the wire can never be
// stored. Batches apply atomically — any sequence gap, unknown id or
// impossible position refuses the whole batch, marks the replica stale and
// demands a fresh baseline.
class SyncReceiver {
public:
    SyncReceiver(const EntityContract& contract, std::uint32_t type_id) : replica_(contract, type_id) {}

    // A baseline message: the visible snapshot replaces the replica and the
    // replication sequence restarts at zero.
    void rebaseline(EntityValues baseline) {
        restore(std::move(baseline));
        sequence_ = 0;
        stale_ = false;
    }

    bool apply(const std::vector<SyncDelta>& batch) {
        if (batch.empty()) return true;
        if (stale_) return false;
        // The batch head must continue the stream and every step inside the
        // batch must be consecutive; a torn batch is refused as a whole.
        std::uint64_t expect = sequence_ + 1;
        for (const auto& delta : batch) {
            if (delta.sequence != expect) { stale_ = true; return false; }
            ++expect;
        }
        const auto backup = replica_.snapshot();
        for (const auto& delta : batch) {
            if (!resolve(delta) || !perform(delta)) {
                restore(backup);
                stale_ = true;
                return false;
            }
        }
        replica_.rebaseline();
        sequence_ = batch.back().sequence;
        return true;
    }

    const PropertyStore& replica() const { return replica_; }
    std::uint64_t sequence() const { return sequence_; }
    bool stale() const { return stale_; }
private:
    const ContractProperty* descriptor(std::uint32_t property_id) const {
        const auto& props = replica_.entity().properties;
        const auto it = std::find_if(props.begin(), props.end(),
                                     [&](const ContractProperty& p) { return p.id == property_id; });
        return it == props.end() ? nullptr : &*it;
    }

    // Resolve the wire path against the descriptor into a store path. Unknown
    // ids, a field on a non-struct, an index into a non-array and a struct
    // field on an indexed element are all refused here, before any value is
    // touched.
    bool resolve(const SyncDelta& delta) {
        const auto* prop = descriptor(delta.property_id);
        if (!prop) return false;
        property_ = prop->name;
        ValueShape shape{true, prop->type, prop->of};
        const auto& structs = replica_.contract().structs();
        path_.clear();
        for (const auto& step : delta.path) {
            if (step.by_index) {
                if (shape.type != "ARRAY") return false;
                path_.push_back(TreeStep{true, "", step.index});
                shape = ValueShape{true, shape.of, ""};
            } else {
                const auto spec = structs.find(shape.type);
                if (spec == structs.end()) return false;
                const auto field = std::find_if(spec->second.begin(), spec->second.end(),
                                                [&](const ContractField& f) { return f.id == step.field_id; });
                if (field == spec->second.end()) return false;
                path_.push_back(TreeStep{false, field->name, 0});
                shape = ValueShape{true, field->type, field->of};
            }
        }
        return true;
    }

    bool perform(const SyncDelta& delta) {
        if (delta.kind == SyncDelta::Kind::SET)
            return replica_.set_path(property_, path_, EntityValue(delta.value));
        if (delta.kind == SyncDelta::Kind::INSERT)
            return replica_.insert_at(property_, path_, delta.position, EntityValue(delta.value));
        return replica_.remove_at(property_, path_, delta.position);
    }

    // Restore one full snapshot, clearing the replica's own journal either
    // way: the replica is the end of the chain, it never feeds forward.
    void restore(EntityValues snapshot) {
        replica_.rebaseline();
        for (const auto& [name, value] : snapshot) replica_.set(name, EntityValue(value));
        replica_.rebaseline();
    }

    PropertyStore replica_;
    std::string property_;
    TreePath path_;
    std::uint64_t sequence_ = 0;
    bool stale_ = false;
};

// One observer stream over one controlled store: entry delivers a visible
// baseline, later drains travel as filtered delta batches, departure removes
// the stream. Visibility is per property — SERVER_ONLY never leaves the server
// and OWN_CLIENT reaches owners only.
class PropertyChannel {
public:
    explicit PropertyChannel(PropertyStore* store) : store_(store) {}

    // Entry and re-entry are distinct calls: a stream that already exists is
    // not reset silently, so a duplicate registration is refused instead of
    // swallowing the observer's history.
    bool observe(const std::string& id, bool is_owner) {
        if (audience_.count(id)) return false;
        audience_.emplace(id, Audience{is_owner, 1, taken_ + store_->pending_changes()});
        pending_.emplace(id, 1);
        return true;
    }
    bool depart(const std::string& id) {
        const auto it = audience_.find(id);
        if (it == audience_.end()) return false;
        audience_.erase(it);
        pending_.erase(id);
        return true;
    }
    bool listening(const std::string& id) const { return audience_.count(id) != 0; }

    // Drain the store journal once: pending baselines first, then one delta
    // batch per observer carrying only what that audience may see. Changes
    // from before an observer's entry are history their baseline already
    // covers, so they are never re-delivered.
    std::vector<SyncMessage> deliver() {
        std::vector<SyncMessage> out;
        const auto changes = store_->take_changes();
        const std::uint64_t first = taken_;
        taken_ += changes.size();
        for (auto it = audience_.begin(); it != audience_.end(); ++it) {
            const std::size_t waiting = pending_.count(it->first) ? pending_[it->first] : 0;
            pending_.erase(it->first);
            for (std::size_t i = 0; i < waiting; ++i) {
                SyncMessage baseline;
                baseline.baseline = store_->client_view(it->second.is_owner);
                out.push_back(std::move(baseline));
            }
            SyncMessage batch;
            batch.kind = SyncMessage::Kind::DELTAS;
            for (std::size_t i = 0; i < changes.size(); ++i) {
                if (first + i < it->second.entry) continue;
                if (!visible(changes[i].property, it->second.is_owner)) continue;
                if (batch.deltas.empty()) batch.sequence = it->second.next;
                batch.deltas.push_back(to_delta(changes[i], it->second.next++));
            }
            if (!batch.deltas.empty()) out.push_back(std::move(batch));
        }
        return out;
    }

    // Missing or stale baseline, or a sequence gap: a fresh visible snapshot
    // and the observer's sequence starts over at one. An id with no stream
    // gets the guest view — never more.
    SyncMessage resync(const std::string& id) {
        const auto it = audience_.find(id);
        bool is_owner = false;
        if (it != audience_.end()) {
            it->second.next = 1;
            it->second.entry = taken_ + store_->pending_changes();
            is_owner = it->second.is_owner;
        }
        SyncMessage baseline;
        baseline.baseline = store_->client_view(is_owner);
        return baseline;
    }
private:
    struct Audience {
        bool is_owner = false;
        std::uint64_t next = 1;
        std::uint64_t entry = 0;  // journal position the baseline already covers
    };

    bool visible(const std::string& name, bool is_owner) const {
        const auto& props = store_->entity().properties;
        const auto it = std::find_if(props.begin(), props.end(),
                                     [&](const ContractProperty& p) { return p.name == name; });
        return it != props.end() && PropertyStore::client_visible(*it, is_owner);
    }

    SyncDelta to_delta(const TreeChange& change, std::uint64_t sequence) const {
        const auto& props = store_->entity().properties;
        const auto found = std::find_if(props.begin(), props.end(),
                                        [&](const ContractProperty& p) { return p.name == change.property; });
        if (found == props.end()) return {};  // unreachable: the journal only records declared properties
        const auto& prop = *found;
        SyncDelta delta;
        delta.property_id = prop.id;
        delta.kind = change.kind == TreeChange::Kind::SET      ? SyncDelta::Kind::SET
                     : change.kind == TreeChange::Kind::INSERT ? SyncDelta::Kind::INSERT
                                                                : SyncDelta::Kind::REMOVE;
        delta.position = change.position;
        delta.value = change.value;
        delta.sequence = sequence;
        ValueShape shape{true, prop.type, prop.of};
        const auto& structs = store_->contract().structs();
        for (const auto& step : change.path) {
            if (step.by_index) {
                delta.path.push_back(SyncStep{true, 0, step.index});
                shape = ValueShape{true, shape.of, ""};
            } else {
                const auto spec = structs.find(shape.type);
                const auto field = std::find_if(spec->second.begin(), spec->second.end(),
                                                [&](const ContractField& f) { return f.name == step.field; });
                if (field == spec->second.end()) break;  // unreachable for a validated journal entry
                delta.path.push_back(SyncStep{false, field->id, 0});
                shape = ValueShape{true, field->type, field->of};
            }
        }
        return delta;
    }

    PropertyStore* store_;
    std::uint64_t taken_ = 0;
    std::map<std::string, Audience> audience_;
    std::map<std::string, std::size_t> pending_;
};
}
