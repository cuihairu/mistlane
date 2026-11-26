// SPDX-License-Identifier: Apache-2.0
// The host side of the MLD2 entity-state endpoint, as a one-shot client.
// The entity host saves every forced snapshot to the home data role and
// loads it back on spawn; data is the single durable owner. One connection
// per question keeps this off the session plane and makes a save completion
// re-enter the caller's serial loop — the same shape as RecordFetcher, with
// its own shorter deadline so an unreachable data leg cannot strand a node
// shutdown for long. Nothing here is retried: a refused or timed-out save
// aborts the in-flight receipt and the periodic driver retries on its next
// pass; a refused load keeps the freshly spawned defaults.
#pragma once
#include "tcp_channel.hpp"
#include "node_internal.hpp"
#include "mistlane/entity_state.hpp"
#include "mistlane/entity_state_wire.hpp"
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
namespace mistlane {

class EntityStateFetcher : public std::enable_shared_from_this<EntityStateFetcher> {
public:
    using Done = std::function<void(bool ok, std::string reason)>;
    // present=false means MLD2 EMPTY (no snapshot on the data node), not a
    // failure; reason distinguishes "empty" from the short refusal words
    // ("unreachable", "refused", "timeout", "INVALID"/"STORAGE"/"unexpected").
    using LoadDone = std::function<void(bool present, std::uint64_t generation,
                                        std::uint64_t revision, std::string bytes, std::string reason)>;
    static void save(asio::any_io_executor executor, Endpoint data, std::string identity,
                     EntityStateSnapshot snapshot, Done done) {
        std::string hex;
        state_hex_encode(snapshot.snapshot, hex);
        std::shared_ptr<EntityStateFetcher> self(
            new EntityStateFetcher(std::move(executor), data, std::move(identity), Kind::Save,
                                   std::chrono::seconds(2)));
        self->question_ = "MLD2 SAVE " + std::to_string(snapshot.scope) + " "
            + std::to_string(snapshot.type_id) + " " + std::to_string(snapshot.incarnation) + " "
            + std::to_string(snapshot.entity_id) + " gen " + std::to_string(snapshot.generation)
            + " rev " + std::to_string(snapshot.committed_revision) + " " + hex;
        self->done_ = std::move(done);
        self->start();
    }
    static void load(asio::any_io_executor executor, Endpoint data, std::string identity,
                     EntityStateKey key, LoadDone done) {
        std::shared_ptr<EntityStateFetcher> self(
            new EntityStateFetcher(std::move(executor), data, std::move(identity), Kind::Load,
                                   std::chrono::seconds(2)));
        self->question_ = "MLD2 LOAD " + std::to_string(key.scope) + " "
            + std::to_string(key.type_id) + " " + std::to_string(key.incarnation) + " "
            + std::to_string(key.entity_id);
        self->load_done_ = std::move(done);
        self->start();
    }
    // The transfer's unique-write tombstone. fenced=false is a refusal (the
    // data row already outlives the asked generation, the leg refused, or a
    // volatile node answered a receipt it cannot honor durably); fenced=true
    // carries the floor the data node now holds.
    using FenceDone = std::function<void(bool fenced, std::uint64_t generation, std::string reason)>;
    static void fence(asio::any_io_executor executor, Endpoint data, std::string identity,
                      EntityStateKey key, std::uint64_t generation, FenceDone done) {
        std::shared_ptr<EntityStateFetcher> self(
            new EntityStateFetcher(std::move(executor), data, std::move(identity), Kind::Fence,
                                   std::chrono::seconds(2)));
        self->question_ = "MLD2 FENCE " + std::to_string(key.scope) + " "
            + std::to_string(key.type_id) + " " + std::to_string(key.incarnation) + " "
            + std::to_string(key.entity_id) + " gen " + std::to_string(generation);
        self->fence_done_ = std::move(done);
        self->start();
    }
private:
    enum class Kind { Save, Load, Fence };
    EntityStateFetcher(asio::any_io_executor executor, Endpoint data, std::string identity,
                       Kind kind, std::chrono::seconds deadline)
      : endpoint_(tcp::endpoint(parse_address(data.address), data.port)),
        identity_(std::move(identity)), kind_(kind), deadline_(deadline),
        channel_(std::make_shared<TcpChannel>(tcp::socket(executor))), timer_(executor) {}
    void start() {
        arm_deadline();
        channel_->socket().async_connect(endpoint_, [self=shared_from_this()](Error ec) {
            if (self->finished_) return;
            if (ec) return self->fail("unreachable");
            if (self->identity_.empty()) return self->ask();
            // A tokened data endpoint refuses every MLD2 frame until the
            // peer identifies (session plane: refuse_service MLS1 TOKEN).
            self->channel_->write(self->identity_, [self](Error ec) {
                if (self->finished_) return;
                if (ec) return self->fail("unreachable");
                self->channel_->read([self](Error ec, std::string payload) {
                    if (self->finished_) return;
                    if (ec) return self->fail("unreachable");
                    if (payload != "MLS1 OK") return self->fail("refused");
                    self->ask();
                });
            });
        });
    }
    void ask() {
        channel_->write(question_, [self=shared_from_this()](Error ec) {
            if (self->finished_) return;
            if (ec) return self->fail("unreachable");
            self->channel_->read([self](Error ec, std::string payload) {
                if (self->finished_) return;
                if (ec) return self->fail("unreachable");
                self->verdict(std::move(payload));
            });
        });
    }
    void verdict(std::string payload) {
        if (kind_ == Kind::Load) {
            if (payload == "MLD2 EMPTY") return load_result(false, 0, 0, {}, "empty");
            if (payload.starts_with("MLD2 STATE ")) {
                std::istringstream in(payload);
                std::string magic, state, scope, type, incarnation, entity, gen_tag, gen, rev_tag, rev, hex;
                std::uint64_t generation = 0, revision = 0;
                if (!(in >> magic >> state >> scope >> type >> incarnation >> entity
                      >> gen_tag >> gen >> rev_tag >> rev >> hex)
                        || magic != "MLD2" || state != "STATE" || gen_tag != "gen" || rev_tag != "rev"
                        || !state_decimal(gen, generation) || !state_decimal(rev, revision, true))
                    return load_result(false, 0, 0, {}, "unexpected");
                std::string bytes;
                if (!state_hex_decode(hex, bytes)) return load_result(false, 0, 0, {}, "unexpected");
                return load_result(true, generation, revision, std::move(bytes), {});
            }
            if (payload.starts_with("MLD2 ERR ")) return load_result(false, 0, 0, {}, payload.substr(9));
            if (payload.starts_with("MLS1")) return load_result(false, 0, 0, {}, "refused");
            return load_result(false, 0, 0, {}, "unexpected");
        }
        if (kind_ == Kind::Fence) {
            if (payload.starts_with("MLD2 FENCED ")) {
                std::istringstream in(payload);
                std::string magic, fenced, scope, type, incarnation, entity, gen_tag, gen;
                std::uint64_t generation = 0;
                if (!(in >> magic >> fenced >> scope >> type >> incarnation >> entity
                      >> gen_tag >> gen)
                        || magic != "MLD2" || fenced != "FENCED" || gen_tag != "gen"
                        || !state_decimal(gen, generation))
                    return fence_result(false, 0, "unexpected");
                return fence_result(true, generation, {});
            }
            if (payload.starts_with("MLD2 ERR ")) return fence_result(false, 0, payload.substr(9));
            if (payload.starts_with("MLS1")) return fence_result(false, 0, "refused");
            return fence_result(false, 0, "unexpected");
        }
        // Only the durable ACK completes the receipt. A volatile RECEIVED is
        // an honest non-durability receipt: the sender aborts and the record
        // stays dirty for the next period, exactly like an MLD1 sender left
        // in its retry state — never an acknowledged-but-lost snapshot.
        if (payload.starts_with("MLD2 ACK ")) return finish(true, {});
        if (payload.starts_with("MLD2 RECEIVED ")) return finish(false, "volatile");
        if (payload.starts_with("MLD2 ERR ")) return finish(false, payload.substr(9));
        if (payload.starts_with("MLS1")) return finish(false, "refused");
        return finish(false, "unexpected");
    }
    void arm_deadline() {
        timer_.expires_after(deadline_);
        timer_.async_wait([self=shared_from_this()](Error ec) {
            if (!ec && !self->finished_) self->fail("timeout");
        });
    }
    void finish(bool ok, std::string reason) {
        if (finished_) return;
        finished_ = true;
        timer_.cancel();
        channel_->close();
        if (done_) done_(ok, std::move(reason));
    }
    void load_result(bool present, std::uint64_t gen, std::uint64_t rev,
                     std::string bytes, std::string reason) {
        if (finished_) return;
        finished_ = true;
        timer_.cancel();
        channel_->close();
        if (load_done_) load_done_(present, gen, rev, std::move(bytes), std::move(reason));
    }
    void fence_result(bool fenced, std::uint64_t generation, std::string reason) {
        if (finished_) return;
        finished_ = true;
        timer_.cancel();
        channel_->close();
        if (fence_done_) fence_done_(fenced, generation, std::move(reason));
    }
    void fail(std::string reason) {
        if (kind_ == Kind::Load) return load_result(false, 0, 0, {}, std::move(reason));
        if (kind_ == Kind::Fence) return fence_result(false, 0, std::move(reason));
        return finish(false, std::move(reason));
    }
    tcp::endpoint endpoint_;
    std::string identity_, question_;
    Kind kind_ = Kind::Save;
    std::chrono::seconds deadline_{2};
    std::shared_ptr<TcpChannel> channel_;
    asio::steady_timer timer_;
    Done done_;
    LoadDone load_done_;
    FenceDone fence_done_;
    bool finished_ = false;
};
}
