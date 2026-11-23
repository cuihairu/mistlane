// SPDX-License-Identifier: Apache-2.0
// The source-side caller of a cross-process avatar transfer. A host that
// holds an active avatar and hands it off drives the coordinator's MLR1 MIG
// sequence and the home data endpoint's final MLD1 SUBMIT/FENCE, then releases
// the avatar once the destination accepted and the route committed — the
// production caller todo.md line 30 names as "源侧 transfer_out → MLD1
// SUBMIT/FENCE → MLR1 MIG EVENT DRAINED → COMMIT 后 release". One driver per
// transfer; the script seam (mistlane.transfer) constructs it and owns the
// answer callback. The destination half (load(TransferIn) + avatar_transfer_in
// + READY) is a later slice — here the destination is driven externally.
//
// The sequence is the coordinator_migration_fourstep ground truth:
//   OWNER -> PREPARE (generates the request id) -> transfer_out (snapshot)
//   -> MLD1 LOAD (current seq) -> MLD1 SUBMIT <snapshot> -> EVENT DRAINED
//   -> MLD1 FENCE -> EVENT FENCE -> poll STATUS until ready -> COMMIT -> release
// A refusal before the data tombstone (any step through the MLD1 FENCE answer)
// aborts the transfer — the coordinator gets ABORT then EVENT CLEANUP (which
// releases the request lease so the key is not blocked for a later transfer),
// the avatar returns to active and the script hears the reason. A refusal
// after the MLD1 FENCE cannot be undone: the data record is already fenced and
// migration.hpp's apply() answers Order to Abort once home_fenced, so the
// driver leaves the avatar transferring and reports the error — the
// coordinator's recovery (a takeover) is the only way the gap un-fences.
#pragma once
#include "migration_client.hpp"
#include "record_fetch.hpp"
#include "mistlane/avatar_runtime.hpp"
#include "mistlane/migration.hpp"
#include "node_internal.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace mistlane {

class TransferDriver : public std::enable_shared_from_this<TransferDriver> {
public:
    // One verdict: an empty error is success (the avatar released); a short
    // word otherwise — "disabled" (no runtime), "no avatar", "unreachable" /
    // "refused" / "CONFLICT" / "STALE" / "INVALID" / "ORDER" / "PENDING" /
    // "timeout", or the MLD1 refusal word ("STALE"/"SEQ"/"NOFENCE"/...).
    using Answer = std::function<void(std::string error)>;

    // `source` is the host's own owner with the role/instance/scope/namespace
    // filled and an empty scene; `destination` the target owner. The runtime
    // accessor returns the host's AvatarRuntime (null when the script hosts no
    // avatars). `identity` is the MLS1 hello for a tokened coordinator/data
    // endpoint (service_hello), empty for a plain one.
    TransferDriver(asio::any_io_executor executor, Endpoint coordinator, Endpoint data,
                   std::string identity, MigrationOwner source, MigrationOwner destination,
                   std::string namespace_, std::function<AvatarRuntime*()> runtime, Answer answer)
      : executor_(executor), coordinator_(std::move(coordinator)), data_(std::move(data)),
        identity_(std::move(identity)), source_(std::move(source)),
        destination_(std::move(destination)), namespace_(std::move(namespace_)),
        runtime_(std::move(runtime)), answer_(std::move(answer)),
        budget_(executor), poll_timer_(executor) {}

    // Realm + character name the avatar; the generation is recovered from the
    // runtime so the request need not carry it. The source scene comes from the
    // script (the avatar's current scene, the source owner's scene field).
    void start(std::string_view realm, std::string_view character, std::string_view source_scene) {
        realm_ = realm;
        character_ = character;
        budget_.expires_after(std::chrono::milliseconds(kTransferBudgetMs));
        budget_.async_wait([self = shared_from_this()](Error ec) {
            if (!ec && !self->done_) self->finish("timeout");
        });
        if (!runtime_()) return finish("disabled");
        const auto live = runtime_()->live_key(realm_, character_);
        if (!live) return finish("no avatar");
        key_ = *live;
        source_.scene = source_scene;
        owner();
    }

private:
    static constexpr std::uint64_t kTransferBudgetMs = 30000;
    static constexpr std::chrono::milliseconds kPollInterval{200};
    static std::atomic<std::uint64_t> next_id_;

    void owner() {
        MigrationClient::ask(executor_, coordinator_, identity_,
            MigrationClient::owner_frame(namespace_, realm_, character_, key_.generation, source_),
            [self = shared_from_this()](MigrationClient::Verdict v) {
                if (self->done_) return;
                if (!v.applied) return self->abort(v.reason);
                self->prepare();
            });
    }
    void prepare() {
        // PREPARE binds the caller's request id; the coordinator reserves the
        // next generation and echoes both. One id per driver — a global counter
        // keeps ids unique across transfers on this host.
        request_id_ = "move-" + std::to_string(next_id_.fetch_add(1));
        MigrationClient::ask(executor_, coordinator_, identity_,
            MigrationClient::prepare_frame("PREPARE", namespace_, realm_, character_, request_id_,
                                           key_.generation, source_, destination_),
            [self = shared_from_this()](MigrationClient::Verdict v) {
                if (self->done_) return;
                if (!v.applied) return self->abort(v.reason);
                self->candidate_ = v.generation;
                self->transfer_out();
            });
    }
    void transfer_out() {
        if (!runtime_()) return abort("disabled");
        std::string snapshot;
        const auto result = runtime_()->transfer_out(key_, snapshot);
        if (!result.ok()) return abort(result.reason);
        snapshot_ = std::move(snapshot);
        load_seq();
    }
    void load_seq() {
        RecordFetcher::load(executor_, data_, identity_, realm_, character_,
            [self = shared_from_this()](RecordFetcher::Record record, std::string reason) {
                if (self->done_) return;
                // MLD1 EMPTY is not a failure: a recordless avatar's first
                // record is seq 1. The next ordered change after a committed
                // record is its sequence + 1.
                if (reason == "EMPTY") return self->submit_seq(1);
                if (!reason.empty()) return self->abort(reason);
                self->submit_seq(record.present ? record.sequence + 1 : 1);
            });
    }
    void submit_seq(std::uint64_t sequence) {
        RecordFetcher::submit(executor_, data_, identity_, realm_, character_, key_.generation,
                              sequence, snapshot_,
            [self = shared_from_this()](bool ok, std::string reason) {
                if (self->done_) return;
                if (!ok) return self->abort(reason);
                self->drained();
            });
    }
    void drained() {
        MigrationClient::ask(executor_, coordinator_, identity_,
            MigrationClient::event_frame(namespace_, realm_, character_, request_id_,
                                         candidate_, "DRAINED"),
            [self = shared_from_this()](MigrationClient::Verdict v) {
                if (self->done_) return;
                if (!v.applied) return self->abort(v.reason);
                self->fence();
            });
    }
    void fence() {
        RecordFetcher::fence(executor_, data_, identity_, realm_, character_, key_.generation,
            [self = shared_from_this()](bool ok, std::string reason) {
                if (self->done_) return;
                if (!ok) return self->abort(reason);
                // The data record is tombstoned from here: no writer at this or
                // an older generation resumes, so the handoff cannot be undone.
                self->fenced_ = true;
                self->fenced_event();
            });
    }
    void fenced_event() {
        MigrationClient::ask(executor_, coordinator_, identity_,
            MigrationClient::event_frame(namespace_, realm_, character_, request_id_,
                                         candidate_, "FENCE"),
            [self = shared_from_this()](MigrationClient::Verdict v) {
                if (self->done_) return;
                if (!v.applied) return self->abort(v.reason);
                self->poll();
            });
    }
    void poll() {
        MigrationClient::ask(executor_, coordinator_, identity_,
            MigrationClient::status_frame(namespace_, realm_, character_),
            [self = shared_from_this()](MigrationClient::Verdict v) {
                if (self->done_) return;
                if (!v.applied) return self->poll_again();
                // flags: "<drained|open> <fenced|unfenced> <ready|notready>".
                std::istringstream in(v.flags);
                std::string drained, fenced, ready;
                if (!(in >> drained >> fenced >> ready)) return self->poll_again();
                if (ready != "ready") return self->poll_again();
                self->commit();
            });
    }
    void poll_again() {
        poll_timer_.expires_after(kPollInterval);
        poll_timer_.async_wait([self = shared_from_this()](Error ec) {
            if (!ec && !self->done_) self->poll();
        });
    }
    void commit() {
        MigrationClient::ask(executor_, coordinator_, identity_,
            MigrationClient::token_frame("COMMIT", namespace_, realm_, character_,
                                         request_id_, candidate_),
            [self = shared_from_this()](MigrationClient::Verdict v) {
                if (self->done_) return;
                // PENDING (destination not publishable) and ORDER (not ready
                // yet) are not done: keep polling. A hard refusal stays the
                // coordinator's to recover; the avatar stays transferring.
                if (!v.applied && v.reason != "PENDING" && v.reason != "ORDER")
                    return self->finish(v.reason);
                if (v.applied) self->release();
                else self->poll_again();
            });
    }
    void release() {
        if (runtime_()) (void)runtime_()->release(key_);
        finish("");
    }
    // Pre-tombstone refusal: tell the coordinator (best effort) and return the
    // avatar to active. After the data fence there is no abort — the callers
    // only reach this before the MLD1 FENCE answer.
    void abort(std::string reason) {
        if (fenced_) return finish(std::move(reason));
        if (!request_id_.empty()) {
            // ABORT only schedules the rollback; EVENT CLEANUP finishes it and
            // releases the request lease so the key is not blocked. The two are
            // separate connections, so the cleanup waits on the abort verdict —
            // the coordinator's cleanup requires the Aborting state.
            MigrationClient::ask(executor_, coordinator_, identity_,
                MigrationClient::token_frame("ABORT", namespace_, realm_, character_,
                                             request_id_, candidate_),
                [self = shared_from_this()](MigrationClient::Verdict) {
                    MigrationClient::ask(self->executor_, self->coordinator_, self->identity_,
                        MigrationClient::event_frame(self->namespace_, self->realm_,
                                                     self->character_, self->request_id_,
                                                     self->candidate_, "CLEANUP"),
                        [](MigrationClient::Verdict) {});
                });
        }
        if (runtime_()) runtime_()->transfer_abort(key_);
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
    MigrationOwner source_, destination_;
    std::string namespace_;
    std::function<AvatarRuntime*()> runtime_;
    Answer answer_;
    std::string realm_, character_, request_id_, snapshot_;
    AvatarKey key_;
    std::uint64_t candidate_ = 0;
    bool fenced_ = false, done_ = false;
    asio::steady_timer budget_, poll_timer_;
};

inline std::atomic<std::uint64_t> TransferDriver::next_id_{0};

} // namespace mistlane
