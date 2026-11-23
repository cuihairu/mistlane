// SPDX-License-Identifier: Apache-2.0
// The host side of the coordinator's MLR1 MIG verbs, as a one-shot client.
// A host that participates in a cross-process avatar transfer (the source
// that hands the avatar off, the destination that takes it over, or the
// login that publishes home facts) asks the coordinator one migration verb
// per connection and reads one verdict — the same connect / (optional MLS1
// identity) / write / read / answer shape as RecordFetcher, with a deadline
// that cancels a silent coordinator. The coordinator side of the same
// protocol is registry_service.hpp's migration_line(); this is deliberately
// one-shot and un-retried, because each migration step has its own refusal
// and the caller re-drives it. Frame builders here mirror the wire grammar
// the coordinator documents (owner text is "<realm|cross> NS <namespace>
// <scope> <role> <instance> <scene>").
#pragma once
#include "tcp_channel.hpp"
#include "node_internal.hpp"
#include "mistlane/migration.hpp"
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace mistlane {

class MigrationClient : public std::enable_shared_from_this<MigrationClient> {
public:
    // One typed migration verdict. applied=true means the verb applied (or
    // replayed idempotently); pending=true means the commit was downgraded
    // because the destination is not publishable (the caller retries later).
    // A transport or protocol refusal is reason, never applied: "unreachable"
    // (no leg), "refused" (MLS1 refused or no OK), "timeout", "EMPTY",
    // "INVALID"/"STALE"/"CONFLICT"/"ORDER"/"UNEXPECTED" (the coordinator's
    // MLR1 ERR <word>) or "unexpected" (an answer this client does not parse).
    struct Verdict {
        bool applied = false, pending = false;
        std::string reason;
        std::string id;
        std::uint64_t generation = 0;
        std::string state;  // STEP's <STATE>, or the STATE line's state word
        std::string owner;  // the COMMITTED / idle-STATE owner text
        std::string flags;  // the STATE line's drained/open etc. tail
    };
    using Answer = std::function<void(Verdict)>;
    // Answers exactly once, on the executor's thread that started it.
    // `identity` is the MLS1 hello presented when the coordinator is tokened
    // (the host's service_hello), empty for a plain one. `frame` is a full
    // "MLR1 MIG <verb> ..." line built by one of the builders below.
    static void ask(asio::any_io_executor executor, Endpoint coordinator, std::string identity,
                    std::string frame, Answer answer) {
        std::shared_ptr<MigrationClient> self(new MigrationClient(std::move(executor), coordinator,
                                                                  std::move(identity)));
        self->frame_ = std::move(frame);
        self->answer_ = std::move(answer);
        self->start();
    }

    // Owner wire text: "<realm|cross> NS <namespace> <scope> <role> <instance> <scene>".
    static std::string owner(MigrationScope::Kind kind, const std::string& namespace_,
                             const std::string& scope, const std::string& role,
                             const std::string& instance, const std::string& scene) {
        std::ostringstream out;
        out << (kind == MigrationScope::Kind::Realm ? "realm" : "cross") << " NS "
            << (namespace_.empty() ? "-" : namespace_) << ' ' << scope << ' '
            << role << ' ' << instance << ' ' << scene;
        return out.str();
    }
    static std::string owner_text(const MigrationOwner& owner) {
        return MigrationClient::owner(owner.scope.kind, owner.scope.namespace_,
            owner.scope.kind == MigrationScope::Kind::Realm ? owner.scope.realm
                                                            : owner.scope.cross_group,
            owner.role, owner.instance, owner.scene);
    }
    // "MLR1 MIG <verb> [NS <namespace>] <realm> <character>" — the shared head.
    static std::string head(std::string_view verb, const std::string& namespace_,
                            std::string_view realm, std::string_view character) {
        std::ostringstream out;
        out << "MLR1 MIG " << verb;
        if (!namespace_.empty()) out << " NS " << namespace_;
        out << ' ' << realm << ' ' << character;
        return out.str();
    }
    // OWNER: publish the home facts the coordinator adopts once.
    static std::string owner_frame(const std::string& namespace_, std::string_view realm,
                                   std::string_view character, std::uint64_t generation,
                                   const MigrationOwner& home) {
        return head("OWNER", namespace_, realm, character) + " " + std::to_string(generation)
            + " " + owner_text(home);
    }
    // PREPARE / TAKEOVER: bind a request id, the home-issued candidate
    // generation, the expected owner and the destination.
    static std::string prepare_frame(std::string_view verb, const std::string& namespace_,
                                     std::string_view realm, std::string_view character,
                                     std::string_view id, std::uint64_t generation,
                                     const MigrationOwner& expected,
                                     const MigrationOwner& destination) {
        return head(verb, namespace_, realm, character) + " " + std::string(id) + " "
            + std::to_string(generation) + " " + owner_text(expected) + " " + owner_text(destination);
    }
    // EVENT: drive one step (DRAINED/FENCE/READY/CLEANUP) for a request.
    static std::string event_frame(const std::string& namespace_, std::string_view realm,
                                   std::string_view character, std::string_view id,
                                   std::uint64_t generation, std::string_view step) {
        return head("EVENT", namespace_, realm, character) + " " + std::string(id) + " "
            + std::to_string(generation) + " " + std::string(step);
    }
    // COMMIT / ABORT: the terminal verb for a request.
    static std::string token_frame(std::string_view verb, const std::string& namespace_,
                                   std::string_view realm, std::string_view character,
                                   std::string_view id, std::uint64_t generation) {
        return head(verb, namespace_, realm, character) + " " + std::string(id) + " "
            + std::to_string(generation);
    }
    // STATUS: the coordinator's live view of one participant.
    static std::string status_frame(const std::string& namespace_, std::string_view realm,
                                    std::string_view character) {
        return head("STATUS", namespace_, realm, character);
    }
private:
    MigrationClient(asio::any_io_executor executor, Endpoint coordinator, std::string identity)
      : endpoint_(tcp::endpoint(parse_address(coordinator.address), coordinator.port)),
        identity_(std::move(identity)),
        channel_(std::make_shared<TcpChannel>(tcp::socket(executor))), timer_(executor) {}
    void start() {
        deadline();
        channel_->socket().async_connect(endpoint_, [self=shared_from_this()](Error ec) {
            if (self->done_) return;
            if (ec) return self->fail("unreachable");
            if (self->identity_.empty()) return self->ask();
            // A tokened coordinator refuses every MLR1 frame until the peer
            // identifies (registry_service: refuse_service MLS1 TOKEN).
            self->channel_->write(self->identity_, [self](Error ec) {
                if (self->done_) return;
                if (ec) return self->fail("unreachable");
                self->channel_->read([self](Error ec, std::string payload) {
                    if (self->done_) return;
                    if (ec) return self->fail("unreachable");
                    if (payload != "MLS1 OK") return self->fail("refused");
                    self->ask();
                });
            });
        });
    }
    void ask() {
        channel_->write(frame_, [self=shared_from_this()](Error ec) {
            if (self->done_) return;
            if (ec) return self->fail("unreachable");
            self->channel_->read([self](Error ec, std::string payload) {
                if (self->done_) return;
                if (ec) return self->fail("unreachable");
                self->verdict(std::move(payload));
            });
        });
    }
    void verdict(std::string payload) {
        if (payload == "MLR1 MIG OWNER OK") return applied();
        if (payload == "MLR1 EMPTY") return fail("EMPTY");
        if (payload.starts_with("MLR1 ERR ")) {
            const std::string word = payload.substr(9);
            // A dead destination downgrades the commit: the caller retries
            // after the destination is publishable again.
            if (word == "PENDING") { Verdict v; v.pending = true; return done(std::move(v)); }
            return fail(word);
        }
        if (payload.starts_with("MLS1")) return fail("refused");
        if (payload.starts_with("MLR1 MIG PREPARED ")
                || payload.starts_with("MLR1 MIG TAKEOVER ")) {
            std::istringstream in(payload);
            std::string magic, mig, verb, id, gen;
            std::uint64_t generation = 0;
            if (!(in >> magic >> mig >> verb >> id >> gen) || !record_decimal(gen, generation))
                return fail("unexpected");
            Verdict v; v.applied = true; v.id = id; v.generation = generation;
            return done(std::move(v));
        }
        if (payload.starts_with("MLR1 MIG STEP ")) {
            std::istringstream in(payload);
            std::string magic, mig, verb, id, state;
            if (!(in >> magic >> mig >> verb >> id >> state)) return fail("unexpected");
            Verdict v; v.applied = true; v.id = id; v.state = state;
            return done(std::move(v));
        }
        if (payload.starts_with("MLR1 MIG ABORTING ")
                || payload.starts_with("MLR1 MIG ABORTED ")) {
            Verdict v; v.applied = true;
            std::istringstream in(payload);
            std::string magic, mig, verb, id;
            if (!(in >> magic >> mig >> verb >> id)) return fail("unexpected");
            v.id = id;
            return done(std::move(v));
        }
        if (payload.starts_with("MLR1 MIG PENDING ")) {
            Verdict v; v.pending = true;
            std::istringstream in(payload);
            std::string magic, mig, verb, id;
            if (!(in >> magic >> mig >> verb >> id)) return fail("unexpected");
            v.id = id;
            return done(std::move(v));
        }
        if (payload.starts_with("MLR1 MIG COMMITTED ")) {
            // <id> <owner (7 words)> <gen> — owner is space-bearing, so the
            // generation is the last token and the owner is everything between.
            Verdict v; v.applied = true;
            std::istringstream in(payload);
            std::string magic, mig, verb, id;
            if (!(in >> magic >> mig >> verb >> id)) return fail("unexpected");
            v.id = id;
            std::vector<std::string> tail;
            std::string token;
            while (in >> token) tail.push_back(token);
            if (tail.size() < 2 || !record_decimal(tail.back(), v.generation))
                return fail("unexpected");
            for (std::size_t i = 0; i + 1 < tail.size(); ++i)
                v.owner += (v.owner.empty() ? "" : " ") + tail[i];
            return done(std::move(v));
        }
        if (payload.starts_with("MLR1 MIG STATE ")) {
            // "STATE idle <owner7> <gen>" or "STATE <id> <STATE> <f> <f> <f> <gen>".
            std::istringstream in(payload);
            std::string magic, mig, verb, first;
            if (!(in >> magic >> mig >> verb >> first)) return fail("unexpected");
            if (first == "idle") {
                Verdict v; v.applied = true; v.state = "idle";
                std::vector<std::string> tail;
                std::string token;
                while (in >> token) tail.push_back(token);
                if (tail.size() < 2 || !record_decimal(tail.back(), v.generation))
                    return fail("unexpected");
                for (std::size_t i = 0; i + 1 < tail.size(); ++i)
                    v.owner += (v.owner.empty() ? "" : " ") + tail[i];
                return done(std::move(v));
            }
            Verdict v; v.applied = true; v.id = first;
            if (!(in >> v.state)) return fail("unexpected");
            // tail = "<flag> <flag> <flag> <gen>" — the generation is last.
            std::vector<std::string> tail;
            std::string token;
            while (in >> token) tail.push_back(token);
            if (tail.size() < 2 || !record_decimal(tail.back(), v.generation))
                return fail("unexpected");
            for (std::size_t i = 0; i + 1 < tail.size(); ++i)
                v.flags += (v.flags.empty() ? "" : " ") + tail[i];
            return done(std::move(v));
        }
        return fail("unexpected");
    }
    void applied() { done(Verdict{true, false, {}, {}, 0, {}, {}, {}}); }
    void done(Verdict v) {
        if (done_) return;
        done_ = true;
        timer_.cancel();
        channel_->close();
        answer_(std::move(v));
    }
    void fail(std::string reason) {
        Verdict v; v.reason = std::move(reason);
        done(std::move(v));
    }
    void deadline() {
        timer_.expires_after(std::chrono::seconds(10));
        timer_.async_wait([self=shared_from_this()](Error ec) {
            if (!ec && !self->done_) self->fail("timeout");
        });
    }
    tcp::endpoint endpoint_;
    std::string identity_, frame_;
    std::shared_ptr<TcpChannel> channel_;
    asio::steady_timer timer_;
    Answer answer_;
    bool done_ = false;
};
} // namespace mistlane
