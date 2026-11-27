// SPDX-License-Identifier: Apache-2.0
// The source-side caller of an entity's unique-write handoff
// (docs/entity-contract.md "No server rebuild"): a host that holds a live
// entity and hands it to another member freezes the entity at an event-loop
// safe point, flushes its final snapshot through the single forced-save entry
// (waiting the durable ACK — never a volatile RECEIVED), then drives the
// coordinator's MLR1 MIG E-verbs. One driver per transfer; the script seam
// (mistlane.entity.transfer) constructs it through the node and owns the
// answer callback.
//
// The sequence is the step order registry_service.hpp documents:
//   freeze -> forced flush (durable ACK first: a fence before the last delta
//   lands would fence the delta out of data) -> EPREPARE (the source's
//   hosting generation is the declared handoff generation) -> poll ESTATUS
//   until the destination adopted and readied -> MLD2 FENCE <gen> (the
//   unique-write tombstone) -> EEVENT FENCE -> ECOMMIT -> destroy (publishes
//   HOST REMOVE; member-qualified, so it cannot erase the destination's SET)
//   -> finish. The destination resolves the terminal listing and sends the
//   CLEANUP — the source's cleanup would erase the row from the destination's
//   DESTINED answer before it learns the commit and unfreezes its copy.
//
// A refusal before the data fence aborts cleanly: EABORT (no cleanup — the
// destination resolves the aborted row), unfreeze, the entity stays active at
// its generation, nothing was lost. A refusal after the data fence cannot be
// undone: the entity stays frozen (its saves are held in process and fenced
// out of data anyway), the error surfaces to the script, and recovery is the
// coordinator's — the destination's poll completes the commit, or a strictly
// newer takeover supersedes the standing transaction.
#pragma once
#include "migration_client.hpp"
#include "entity_state_fetch.hpp"
#include "mistlane/entity_runtime.hpp"
#include "node_internal.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace mistlane {

class EntityTransferDriver : public std::enable_shared_from_this<EntityTransferDriver> {
public:
    // One verdict: an empty error is success (the entity released); a short
    // word otherwise — "disabled" (no runtime, no persistence chain, or no
    // coordinator/data endpoint), "no store", or the flush/coordinator/data
    // refusal ("volatile"/"in-flight"/"STALE"/"CONFLICT"/"ORDER"/"timeout"/...),
    // "superseded" (another transaction owns the address now), "aborted".
    using Answer = std::function<void(std::string error)>;

    // `runtime` resolves the host's EntityRuntime (null when the node hosts no
    // entities). `identity` is the MLS1 hello for a tokened coordinator/data
    // endpoint, empty for a plain one.
    EntityTransferDriver(asio::any_io_executor executor, Endpoint coordinator, Endpoint data,
                         std::string identity, EntityRuntime* runtime, Answer answer)
      : executor_(executor), coordinator_(std::move(coordinator)), data_(std::move(data)),
        identity_(std::move(identity)), runtime_(runtime), answer_(std::move(answer)),
        budget_(executor), poll_timer_(executor) {}

    // `destination` is the member instance the entity hands off to; `state_hex`
    // is the source bundle's opaque transfer state ("" = none). The declared
    // handoff generation is the store's live hosting generation, read after the
    // flush so it is exactly what the final snapshot landed at.
    void start(EntityTarget ref, std::string destination_instance, std::string state_hex) {
        ref_ = ref;
        destination_ = std::move(destination_instance);
        state_hex_ = std::move(state_hex);
        budget_.expires_after(std::chrono::milliseconds(kTransferBudgetMs));
        budget_.async_wait([self = shared_from_this()](Error ec) {
            if (!ec && !self->done_) self->finish("timeout");
        });
        if (!runtime_) return finish("disabled");
        // Liveness first: freeze() records the address unconditionally, so
        // freezing a dead address would leave the gate shut for a future
        // spawn. The store also names the persistence chain, which a handoff
        // cannot run without.
        if (!store()) return finish("no store");
        runtime_->freeze(ref_);
        flush();
    }

private:
    static constexpr std::uint64_t kTransferBudgetMs = 30000;
    static constexpr std::chrono::milliseconds kPollInterval{200};
    static std::atomic<std::uint64_t> next_id_;

    EntityStateKey key() const {
        return EntityStateKey{ref_.scope, ref_.type_id, ref_.incarnation, ref_.entity_id};
    }
    EntityStateStore* store() const { return runtime_ ? runtime_->state_store(ref_) : nullptr; }

    // The final snapshot. A clean record is already current: its last landed
    // save is what data holds, and an entity that never saved at all has no
    // row — the destination adopts from the declared generation alone. A dirty
    // record flushes through the single forced-save entry and the one-shot
    // observer carries the durability verdict; a volatile ACK (a data node
    // without a durable backend) aborts honestly — no durable handoff claim
    // over a receipt the data role cannot honor.
    void flush() {
        auto* record = store();
        if (!record) return finish("no store");
        if (!record->dirty()) return prepare();
        record->set_flush_observer([self = shared_from_this()](bool landed) {
            if (landed) self->prepare();
            else self->abort("volatile");
        });
        const auto outcome = record->save(record->generation(), record->revision());
        if (outcome.ok) return;  // deferred: the observer continues; inline: it already fired
        // Refused before capture (an in-flight receipt, a moved fence): the
        // observer would never fire for this save, so disarm it — a racing
        // in-flight completion must not fire into a spent wait.
        record->set_flush_observer(nullptr);
        abort(outcome.reason.empty() ? "refused" : outcome.reason);
    }

    void prepare() {
        // The flush landed (or nothing needed to): the store's generation is
        // now the declared handoff generation — the destination must adopt
        // strictly past it, and the fence names it.
        auto* record = store();
        if (!record) return abort("no store");
        declared_gen_ = record->generation();
        request_id_ = "emove-" + std::to_string(next_id_.fetch_add(1));
        MigrationClient::ask(executor_, coordinator_, identity_,
            MigrationClient::entity_prepare_frame(key(), request_id_, declared_gen_, destination_,
                                                  state_hex_),
            [self = shared_from_this()](MigrationClient::Verdict v) {
                if (self->done_) return;
                if (!v.applied) return self->abort(v.reason);
                self->poll();
            });
    }

    void poll() {
        MigrationClient::ask(executor_, coordinator_, identity_,
            MigrationClient::entity_status_frame(key()),
            [self = shared_from_this()](MigrationClient::Verdict v) {
                if (self->done_) return;
                if (!v.applied) return self->poll_again();
                // The answer names the standing transaction; a different id
                // means a takeover replaced ours mid-flight.
                if (v.id != self->request_id_) return self->finish("superseded");
                if (v.state == "ready") return self->fence_data();
                if (v.state == "aborted") {
                    if (self->runtime_) self->runtime_->unfreeze(self->ref_);
                    return self->finish("aborted");
                }
                if (v.state == "fenced") return self->fenced_event();  // a retry resuming
                self->poll_again();  // prepared: the destination has not readied yet
            });
    }
    void poll_again() {
        poll_timer_.expires_after(kPollInterval);
        poll_timer_.async_wait([self = shared_from_this()](Error ec) {
            if (!ec && !self->done_) self->poll();
        });
    }

    // The unique-write tombstone. Strictly after the durable ACK of the final
    // flush: a fence that outruns the flush would floor data below the last
    // delta and the snapshot would land truncated. From here the handoff is
    // no longer abortable — the floor refuses every writer at or below the
    // declared generation, including this host's own frozen copy.
    void fence_data() {
        EntityStateFetcher::fence(executor_, data_, identity_, key(), declared_gen_,
            [self = shared_from_this()](bool fenced, std::uint64_t, std::string reason) {
                if (self->done_) return;
                if (!fenced) return self->abort(reason);
                self->fenced_ = true;
                self->fenced_event();
            });
    }

    // Post-fence steps retry within the budget instead of aborting: the data
    // floor is already raised, so only the coordinator's step words can
    // complete the handoff. A budget timeout leaves the entity frozen with
    // the error surfaced — the destination's poll (or a takeover) recovers.
    void fenced_event() {
        MigrationClient::ask(executor_, coordinator_, identity_,
            MigrationClient::entity_event_frame(key(), request_id_, declared_gen_, "FENCE"),
            [self = shared_from_this()](MigrationClient::Verdict v) {
                if (self->done_) return;
                if (!v.applied) return self->fence_event_again();
                self->commit();
            });
    }
    void fence_event_again() {
        poll_timer_.expires_after(kPollInterval);
        poll_timer_.async_wait([self = shared_from_this()](Error ec) {
            if (!ec && !self->done_) self->fenced_event();
        });
    }
    void commit() {
        MigrationClient::ask(executor_, coordinator_, identity_,
            MigrationClient::entity_token_frame("ECOMMIT", key(), request_id_, declared_gen_),
            [self = shared_from_this()](MigrationClient::Verdict v) {
                if (self->done_) return;
                // PENDING: the destination stopped publishing — the commit
                // would strand the entity, so the coordinator refuses and the
                // source re-drives after it is back (the MIG-form answer
                // flags `pending`; the ERR-form words the reason). ORDER: the
                // FENCE event has not landed yet.
                if (!v.applied && !v.pending && v.reason != "PENDING" && v.reason != "ORDER")
                    return self->finish(v.reason);
                if (v.applied) self->release();
                else self->commit_again();
            });
    }
    void commit_again() {
        poll_timer_.expires_after(kPollInterval);
        poll_timer_.async_wait([self = shared_from_this()](Error ec) {
            if (!ec && !self->done_) self->commit();
        });
    }
    void release() {
        if (runtime_) (void)runtime_->destroy(ref_);
        finish("");
    }

    // Pre-fence refusal: tell the coordinator (best effort — the destination's
    // poll resolves the aborted row either way) and return the entity to
    // active. No CLEANUP from the source: the row must stay listed until the
    // destination learns the abort.
    void abort(std::string reason) {
        if (fenced_) return finish(std::move(reason));
        if (!request_id_.empty())
            MigrationClient::ask(executor_, coordinator_, identity_,
                MigrationClient::entity_token_frame("EABORT", key(), request_id_, declared_gen_),
                [](MigrationClient::Verdict) {});
        if (runtime_) runtime_->unfreeze(ref_);
        finish(std::move(reason));
    }
    void finish(std::string error) {
        if (done_) return;
        done_ = true;
        budget_.cancel();
        poll_timer_.cancel();
        answer_(std::move(error));
    }

    asio::any_io_executor executor_;
    Endpoint coordinator_, data_;
    std::string identity_;
    EntityRuntime* runtime_;
    Answer answer_;
    EntityTarget ref_;
    std::string destination_, state_hex_, request_id_;
    std::uint64_t declared_gen_ = 0;
    bool fenced_ = false, done_ = false;
    asio::steady_timer budget_, poll_timer_;
};

inline std::atomic<std::uint64_t> EntityTransferDriver::next_id_{0};

} // namespace mistlane
