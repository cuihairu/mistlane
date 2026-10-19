// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "mistlane/record.hpp"
#include <limits>

namespace mistlane {
using MigrationKey = RecordStore::RecordKey; // (home realm, character)

// Hosting scope is separate from the participant's immutable home realm.
struct MigrationScope {
    enum class Kind { Realm, CrossGroup };
    Kind kind = Kind::Realm;
    std::string realm, cross_group;
    bool operator==(const MigrationScope&) const = default;
};
struct MigrationOwner {
    MigrationScope scope;
    std::string role, instance, scene;
    bool operator==(const MigrationOwner&) const = default;
};
struct MigrationRoute {
    MigrationOwner owner;
    std::uint64_t generation = 0;
    bool operator==(const MigrationRoute&) const = default;
};
inline bool valid_migration_owner(const MigrationOwner& owner) {
    if (!record_identifier(owner.instance) || !record_identifier(owner.scene)) return false;
    if (owner.scope.kind == MigrationScope::Kind::Realm)
        return owner.role == "gameServer" && record_identifier(owner.scope.realm) && owner.scope.cross_group.empty();
    return owner.scope.kind == MigrationScope::Kind::CrossGroup && owner.role == "crossServer"
        && owner.scope.realm.empty() && record_identifier(owner.scope.cross_group);
}

// Typed protocol, deliberately not an MLR1 wire extension. Every event echoes
// the home key, request id and home-issued candidate generation. PREPARE also
// binds the complete expected route and destination (including scene/scope).
struct MigrationRequest {
    MigrationKey key;
    std::string id;
    MigrationRoute expected;
    MigrationOwner destination;
    bool operator==(const MigrationRequest&) const = default;
};
struct MigrationToken {
    MigrationKey key;
    std::string id;
    std::uint64_t generation = 0;
    bool operator==(const MigrationToken&) const = default;
};
enum class MigrationEvent { SourceDrained, HomeFence, DestinationReady, Commit, Abort, Cleanup, Recover };
enum class MigrationState { Idle, Preparing, Aborting, Recovering, Committed, Aborted };
enum class MigrationResult { Applied, Duplicate, Pending, Invalid, Conflict, Stale, Order };

// Home data's generation allocation boundary, not a coordinator counter.
// Its checkpoint must eventually be persisted with the transaction journal.
// This model is volatile; copying it is only a deterministic crash fixture.
class HomeGenerationAuthority {
public:
    bool seed(const MigrationKey& key, std::uint64_t generation) {
        return record_identifier(key.first) && record_identifier(key.second) && generation
            && issued_.emplace(key, generation).second;
    }
    std::uint64_t issued(const MigrationKey& key) const {
        const auto it = issued_.find(key);
        return it == issued_.end() ? 0 : it->second;
    }
private:
    friend class MigrationTransaction;
    std::optional<std::uint64_t> reserve(const MigrationKey& key) {
        const auto it = issued_.find(key);
        if (it == issued_.end() || it->second == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
        return ++it->second; // Never rolled back or reused, including ABORT.
    }
    std::map<MigrationKey, std::uint64_t> issued_;
};

// One participant's deterministic protocol aggregate. Home facts, source
// lease and coordinator journal/route are colocated ONLY in this model; no
// network authentication, durable storage or Avatar effects are implied.
class MigrationTransaction {
public:
    struct Entry {
        MigrationRequest request;
        MigrationToken token;
        MigrationState state = MigrationState::Preparing;
        bool source_drained = false, home_fenced = false, destination_ready = false;
        bool destination_staged = true;
    };
    struct Snapshot {
        MigrationKey key;
        MigrationRoute route, home_owner;
        WriterLease lease;
        std::map<std::string, Entry> journal; // Retain terminal ids for replay.
        std::string current;
    };
    MigrationTransaction(HomeGenerationAuthority& home, MigrationKey key, MigrationRoute route)
      : home_(home) {
        if (!valid_migration_owner(route.owner) || !route.generation || home.issued(key) != route.generation)
            throw std::invalid_argument("invalid migration bootstrap");
        data_.key = std::move(key);
        data_.route = data_.home_owner = std::move(route);
        data_.lease.start(data_.route.generation);
    }
    // Restore only a trusted checkpoint produced by snapshot(), together with
    // its home-authority checkpoint. This is not a disk recovery implementation.
    MigrationTransaction(HomeGenerationAuthority& home, Snapshot checkpoint)
      : home_(home), data_(std::move(checkpoint)) {}
    Snapshot snapshot() const { return data_; }
    const MigrationRoute& route() const { return data_.route; }
    const Entry* entry(std::string_view id) const {
        const auto it = data_.journal.find(std::string(id));
        return it == data_.journal.end() ? nullptr : &it->second;
    }
    MigrationState state() const {
        const auto* current = entry(data_.current);
        return current ? current->state : MigrationState::Idle;
    }
    bool write_allowed(const MigrationOwner& owner, std::uint64_t generation) const {
        return owner == data_.home_owner.owner && data_.lease.write_allowed(generation);
    }
    bool timers_running() const { return data_.lease.timer_running(); }
    bool begin_request() { return data_.lease.begin_request(); }
    bool finish_request() { return data_.lease.finish_request(); }

    MigrationResult prepare(const MigrationRequest& request) {
        if (request.key != data_.key || !record_identifier(request.id)
                || !valid_migration_owner(request.destination) || !valid_migration_owner(request.expected.owner))
            return MigrationResult::Invalid;
        if (const auto* old = entry(request.id))
            return old->request == request ? MigrationResult::Duplicate : MigrationResult::Conflict;
        if (state() != MigrationState::Idle && !terminal(state())) return MigrationResult::Conflict;
        if (request.expected != data_.route || request.expected != data_.home_owner)
            return MigrationResult::Stale; // Compare owner AND route generation.
        if (request.destination == request.expected.owner) return MigrationResult::Invalid;
        const auto generation = home_.reserve(request.key);
        if (!generation) return MigrationResult::Conflict;
        if (!data_.lease.request(*generation)) return MigrationResult::Order;
        Entry row{request, {request.key, request.id, *generation}};
        data_.journal.emplace(request.id, std::move(row));
        data_.current = request.id;
        return MigrationResult::Applied;
    }

    MigrationResult apply(const MigrationToken& token, MigrationEvent event) {
        if (token.key != data_.key) return MigrationResult::Invalid;
        auto it = data_.journal.find(token.id);
        if (it == data_.journal.end() || token.generation != it->second.token.generation)
            return MigrationResult::Stale;
        auto& row = it->second;
        // Replays of an old terminal id acknowledge its outcome without ever
        // republishing its route over a later migration.
        if (terminal(row.state)) {
            if (event == MigrationEvent::Recover
                    || (event == MigrationEvent::Commit && row.state == MigrationState::Committed)
                    || ((event == MigrationEvent::Abort || event == MigrationEvent::Cleanup)
                        && row.state == MigrationState::Aborted)) return MigrationResult::Duplicate;
            return MigrationResult::Conflict;
        }
        if (token.id != data_.current) return MigrationResult::Conflict;
        if (event == MigrationEvent::Recover) {
            row.state = MigrationState::Recovering;
            if (!row.home_fenced) {
                row.state = MigrationState::Aborting;
                return cleanup(row);
            }
            // A tombstone is irreversible. Never invent destination ACKs or
            // grant the source generation again after a crash/timeout.
            return row.destination_ready ? commit(row) : MigrationResult::Pending;
        }
        if (event == MigrationEvent::Abort) {
            if (row.home_fenced) return MigrationResult::Order;
            if (row.state == MigrationState::Aborting) return MigrationResult::Duplicate;
            row.state = MigrationState::Aborting;
            return MigrationResult::Applied;
        }
        if (event == MigrationEvent::Cleanup) return cleanup(row);
        if (row.state == MigrationState::Aborting) return MigrationResult::Order;
        switch (event) {
        case MigrationEvent::SourceDrained:
            if (row.source_drained) return MigrationResult::Duplicate;
            if (!data_.lease.drained()) return MigrationResult::Order;
            row.source_drained = true;
            return MigrationResult::Applied;
        case MigrationEvent::HomeFence:
            if (row.home_fenced) return MigrationResult::Duplicate;
            if (!row.source_drained || !data_.lease.relinquish()) return MigrationResult::Order;
            row.home_fenced = true;
            return MigrationResult::Applied;
        case MigrationEvent::DestinationReady:
            if (row.destination_ready) return MigrationResult::Duplicate;
            if (!row.home_fenced || !data_.lease.accept(token.generation)) return MigrationResult::Order;
            data_.home_owner = {row.request.destination, token.generation};
            row.destination_ready = true;
            return MigrationResult::Applied;
        case MigrationEvent::Commit: return commit(row);
        default: return MigrationResult::Invalid;
        }
    }
private:
    static bool terminal(MigrationState state) {
        return state == MigrationState::Committed || state == MigrationState::Aborted;
    }
    MigrationResult cleanup(Entry& row) {
        if (row.state != MigrationState::Aborting || row.home_fenced) return MigrationResult::Order;
        if (!data_.lease.drained()) return MigrationResult::Pending;
        if (!data_.lease.cancel_request()) return MigrationResult::Order;
        row.destination_staged = false; // Discard the half-built destination.
        row.state = MigrationState::Aborted;
        return MigrationResult::Applied; // Source timers/admission restored.
    }
    MigrationResult commit(Entry& row) {
        if (!row.home_fenced || !row.destination_ready || data_.route != row.request.expected)
            return MigrationResult::Order;
        // Publication is last: source drain -> home fence -> target ACK ->
        // CAS route + committed outcome. The route itself grants no writes.
        data_.route = {row.request.destination, row.token.generation};
        row.destination_staged = false;
        row.state = MigrationState::Committed;
        return MigrationResult::Applied;
    }
    HomeGenerationAuthority& home_;
    Snapshot data_;
};
}
