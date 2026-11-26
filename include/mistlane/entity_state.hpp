// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "mistlane/entity_codec.hpp"
#include "mistlane/entity_dispatch.hpp"
#include "mistlane/entity_persist.hpp"
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace mistlane {
// One entity address: the key of every entity-state row and fence.
struct EntityStateKey {
    std::uint32_t scope = 0, type_id = 0, incarnation = 0;
    std::int64_t entity_id = 0;
    auto tie() const { return std::tie(scope, type_id, incarnation, entity_id); }
    bool operator<(const EntityStateKey& o) const { return tie() < o.tie(); }
    bool operator==(const EntityStateKey& o) const { return tie() == o.tie(); }
};

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

// One persisted entity-migration transaction (the coordinator journal): the
// unique-write handoff of one entity address. `generation` is the declared
// handoff generation the source flushed at; `destination` names the member
// whose DESTINED poll takes the entity over; `state` is the step word
// (prepared/ready/fenced/committed/aborted/cleanup); `state_hex` is the
// opaque bundle state the source's script handed off and the destination's
// script consumes — the framework carries it, never interprets it.
struct EntityMigrationRecord {
    EntityStateKey key;
    std::string id;
    std::uint64_t generation = 0;
    std::string destination;
    std::string state;
    std::string state_hex;
};

// Storage boundary for the owning data side's entity state — the durable half
// of the persistence item that no backend carried before. The SQLite backend
// lives beside the coordinator journal and reuses its versioned-BLOB encoding;
// an in-memory-only caller passes no backend and keeps everything in process
// (save() then skips the durable step and confirms the receipt directly, so
// the record lands in-process and the fence model is still exercised).
class EntityStateBackend {
public:
    virtual ~EntityStateBackend() = default;
    virtual void save_state(const EntityStateSnapshot&) = 0;
    virtual std::vector<EntityStateSnapshot> restore_states() const = 0;
    // A transfer's unique-write fence: no save at or below this generation may
    // ever be acknowledged again. Tombstones can name a key with no row — an
    // entity with zero persistent properties leaves no snapshot row at all.
    virtual void save_entity_fence(const EntityStateKey&, std::uint64_t generation) = 0;
    virtual std::map<EntityStateKey, std::uint64_t> restore_entity_fences() const = 0;
};

// The owning-data persistence driver: one PersistentStore per entity over a
// backend that durably holds the encoded snapshot. save() stays the single
// forced-save entry — the fences live in PersistentStore::save — and the
// driver only adds the durable step: capture, persist, then confirm the
// receipt, so a confirmed save is exactly a landed one and a persist that
// never landed aborts its own in-flight receipt (dirty survives, the slot
// frees for a retry). restore() loads a persisted snapshot back with explicit
// version conversion and the same whole-load refusal.
//
// The durable step has two shapes. With a backend the write is synchronous
// and confirm follows it inline. With a sink (set_async_sink — the data-leg
// transport) the encoded snapshot is handed to the sink and save() returns
// deferred; the sink's completion calls complete_deferred, which confirms on
// a durable ACK and aborts otherwise — dirty survives every non-durable
// outcome and the next period retries. Everything runs on the node's serial
// loop (the sink posts its completion there), so no additional
// synchronization is needed; a multi-threaded node would have to strand
// these calls first.
class EntityStateStore {
public:
    EntityStateStore(EntityTarget address, PersistentStore& persist,
                     const SprotoCodec* codec, EntityStateBackend* backend)
        : address_(address), persist_(persist), codec_(codec), backend_(backend) {}
    ~EntityStateStore() { *alive_ = false; }  // outstanding sink completions become no-ops

    // The async durability sink: one call per save, given the wire-ready
    // snapshot and a done callback that must fire exactly once (on the
    // node's loop). The sink owns the transport leg; the store keeps the
    // receipt until done() names the outcome.
    using AsyncSink = std::function<void(const EntityStateSnapshot&, std::function<void(bool, const std::string&)>)>;
    void set_async_sink(AsyncSink sink) { sink_ = std::move(sink); }
    bool async() const { return static_cast<bool>(sink_); }

    // One-shot transfer observer: armed by the migration driver right before
    // its forced flush, fired once with the durability verdict once an
    // accepted save's outcome is known (after the receipt's confirm/abort,
    // inline or from the sink completion), then disarmed — a later periodic
    // save never re-invokes a spent wait. A save refused before capture (a
    // racing in-flight receipt) fires nothing for itself, so a driver that
    // saw the refusal disarms with set_flush_observer(nullptr); a completion
    // of that racing save then honors the disarm and fires nothing.
    using FlushObserver = std::function<void(bool landed)>;
    void set_flush_observer(FlushObserver observer) { flush_observer_ = std::move(observer); }

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
        if (sink_) {
            // Deferred durability: the receipt stays in flight until the
            // sink answers. The alive guard makes a completion that arrives
            // after this store died (its entity destroyed) a no-op instead
            // of a dangling confirm/abort.
            auto alive = alive_;
            EntityStateSnapshot wire{address_.scope, address_.type_id, address_.incarnation,
                                     address_.entity_id, generation, expected_revision, *encoded};
            sink_(wire, [this, alive, save_id = outcome.receipt.save_id](bool landed, const std::string&) {
                if (!*alive) return;
                if (landed) persist_.confirm(save_id);
                else persist_.abort(save_id);
                fire_flush_observer(landed);
            });
            outcome.deferred = true;
            return outcome;
        }
        if (backend_) {
            try {
                backend_->save_state({address_.scope, address_.type_id, address_.incarnation,
                                      address_.entity_id, generation, expected_revision, *encoded});
            } catch (...) {
                persist_.abort(outcome.receipt.save_id);
                throw;
            }
        }
        if (!persist_.confirm(outcome.receipt.save_id)) {
            outcome.ok = false;
            outcome.error = EntityError::CONFLICT;
            outcome.reason = "save id";
            fire_flush_observer(false);
            return outcome;
        }
        fire_flush_observer(true);
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
    // The one-shot half of the flush observer: disarmed before it fires, so a
    // later periodic save cannot re-invoke a spent migration wait, and a
    // driver that disarmed (its forced save was refused) is honored by a
    // racing in-flight completion.
    void fire_flush_observer(bool landed) {
        if (!flush_observer_) return;
        auto fire = std::move(flush_observer_);
        flush_observer_ = nullptr;
        fire(landed);
    }
    EntityTarget address_;
    PersistentStore& persist_;
    const SprotoCodec* codec_;
    EntityStateBackend* backend_;
    AsyncSink sink_;
    FlushObserver flush_observer_;
    // Shared with every outstanding completion: destroyed() flips it, so a
    // late done() sees false and does not touch the dead persist_.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
}
