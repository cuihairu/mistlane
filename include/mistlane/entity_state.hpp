// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "mistlane/entity_codec.hpp"
#include "mistlane/entity_dispatch.hpp"
#include "mistlane/entity_persist.hpp"
#include <string>
#include <string_view>
#include <vector>

namespace mistlane {
// One durable entity-state row: the owned home snapshot for one entity
// address, fenced by the writer's hosting generation and the committed
// revision the snapshot captured. The bytes are the descriptor's persistent
// property subset in the property snapshot codec's record form.
struct EntityStateSnapshot {
    std::uint32_t scope = 0, type_id = 0, incarnation = 0;
    std::int64_t entity_id = 0;
    std::uint64_t generation = 0, committed_revision = 0;
    std::string snapshot;
};

// Storage boundary for the owning data side's entity state — the durable half
// of the persistence item that no backend carried before. The SQLite backend
// lives beside the coordinator journal and reuses its versioned-BLOB encoding;
// an in-memory-only caller passes no backend and keeps everything in process.
class EntityStateBackend {
public:
    virtual ~EntityStateBackend() = default;
    virtual void save_state(const EntityStateSnapshot&) = 0;
    virtual std::vector<EntityStateSnapshot> restore_states() const = 0;
};

// The owning-data persistence driver: one PersistentStore per entity over a
// backend that durably holds the encoded snapshot. save() stays the single
// forced-save entry — the fences live in PersistentStore::save — and the
// driver only adds the durable step: capture, persist, then confirm the
// receipt, so a confirmed save is exactly a landed one and a persist that
// never landed aborts its own in-flight receipt (dirty survives, the slot
// frees for a retry). restore() loads a persisted snapshot back with explicit
// version conversion and the same whole-load refusal.
class EntityStateStore {
public:
    EntityStateStore(EntityTarget address, PersistentStore& persist,
                     const SprotoCodec* codec, EntityStateBackend* backend)
        : address_(address), persist_(persist), codec_(codec), backend_(backend) {}

    SaveOutcome save(std::uint64_t generation, std::uint64_t expected_revision) {
        SaveOutcome outcome = persist_.save(generation, expected_revision);
        if (!outcome.ok) return outcome;
        const auto encoded = codec_->encode_persistent(persist_.entity(), outcome.snapshot);
        if (!encoded) {
            persist_.abort(outcome.receipt.save_id);
            outcome.ok = false;
            outcome.error = EntityError::INVALID;
            outcome.reason = "snapshot not encodable";
            return outcome;
        }
        try {
            backend_->save_state({address_.scope, address_.type_id, address_.incarnation,
                                  address_.entity_id, generation, expected_revision, *encoded});
        } catch (...) {
            persist_.abort(outcome.receipt.save_id);
            throw;
        }
        if (!persist_.confirm(outcome.receipt.save_id)) {
            outcome.ok = false;
            outcome.error = EntityError::CONFLICT;
            outcome.reason = "save id";
            return outcome;
        }
        return outcome;
    }

    RestoreReport restore(std::uint64_t generation, std::uint64_t committed_revision,
                          std::string_view bytes) {
        EntityValues snapshot;
        std::string reason;
        if (!codec_->decode_persistent(persist_.entity(), bytes, snapshot, reason)) {
            RestoreReport out;
            out.error = EntityError::INVALID;
            out.refused = reason;
            return out;
        }
        return persist_.restore(generation, committed_revision, snapshot);
    }

    const EntityTarget& address() const { return address_; }
    bool dirty() const { return persist_.dirty(); }
    std::uint64_t revision() const { return persist_.revision(); }
    std::uint64_t committed_revision() const { return persist_.committed_revision(); }
    std::uint64_t generation() const { return persist_.generation(); }

private:
    EntityTarget address_;
    PersistentStore& persist_;
    const SprotoCodec* codec_;
    EntityStateBackend* backend_;
};
}
