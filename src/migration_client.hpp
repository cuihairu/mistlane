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
#include "mistlane/entity_state_wire.hpp"
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
        // DESTINED: the transfers this member's destination lease awaits;
        // each names the scene the transfer's destination owner chose (the
        // destination seeds the landed avatar's scene reference from it).
        struct DestinedTransfer { std::string realm, character, id, scene;
                                  std::uint64_t generation = 0; };
        std::vector<DestinedTransfer> destined;
        // The ENTITIES tail of the same answer: the entity handoffs this
        // member awaits. `state` is the step word (prepared/ready/fenced/
        // committed/aborted/cleanup) and `state_hex` the source bundle's
        // opaque transfer state (empty when the wire said "-").
        struct EntityDestined { EntityStateKey key; std::string id;
                                std::uint64_t generation = 0;
                                std::string state, state_hex; };
        std::vector<EntityDestined> entity_destined;
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
    // DESTINED: the fenced transfers this member's destination lease awaits.
    // <scope> is the realm or cross-group name, <role> is gameServer or
    // crossServer and <instance> names the member. The destination polls this
    // instead of being pushed a transfer it may not be ready to take.
    static std::string destined_frame(const std::string& namespace_, const std::string& scope,
                                      const std::string& role, const std::string& instance) {
        std::ostringstream out;
        out << "MLR1 MIG DESTINED";
        if (!namespace_.empty()) out << " NS " << namespace_;
        out << ' ' << scope << ' ' << role << ' ' << instance;
        return out.str();
    }
    // The entity-migration verbs, addressed by the entity 4-tuple the way the
    // MLD2 endpoint names a key. The NS group parses for grammar symmetry and
    // is ignored by the coordinator, so these builders never emit one. The
    // address fields print through state_decimal's family — the same helpers
    // the data leg parses keys with, so a key this side emits the data node
    // would accept.
    static std::string entity_head(std::string_view verb, const EntityStateKey& key) {
        std::ostringstream out;
        out << "MLR1 MIG " << verb << ' ' << key.scope << ' ' << key.type_id << ' '
            << key.incarnation << ' ' << key.entity_id;
        return out.str();
    }
    // EPREPARE: bind a request id, the source's flushed generation, the
    // destination member and the opaque bundle state (empty hex prints "-").
    static std::string entity_prepare_frame(const EntityStateKey& key, std::string_view id,
                                            std::uint64_t generation,
                                            std::string_view destination_instance,
                                            const std::string& state_hex) {
        return entity_head("EPREPARE", key) + " " + std::string(id) + " "
            + std::to_string(generation) + " " + std::string(destination_instance) + " "
            + (state_hex.empty() ? "-" : state_hex);
    }
    // EEVENT: drive one step (READY/FENCE/CLEANUP) for a request.
    static std::string entity_event_frame(const EntityStateKey& key, std::string_view id,
                                          std::uint64_t generation, std::string_view step) {
        return entity_head("EEVENT", key) + " " + std::string(id) + " "
            + std::to_string(generation) + " " + std::string(step);
    }
    // ECOMMIT / EABORT: the terminal verbs for a request.
    static std::string entity_token_frame(std::string_view verb, const EntityStateKey& key,
                                          std::string_view id, std::uint64_t generation) {
        return entity_head(verb, key) + " " + std::string(id) + " " + std::to_string(generation);
    }
    // ESTATUS: the coordinator's live view of one entity handoff.
    static std::string entity_status_frame(const EntityStateKey& key) {
        return entity_head("ESTATUS", key);
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
            if (ec) {
                return self->fail("unreachable");
            }
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
        if (payload.starts_with("MLR1 MIG DESTINED ")) {
            // <count> [<realm> <character> <id> <generation> <scene>]... — the
            // count bounds the entries, and an empty answer is count 0.
            Verdict v; v.applied = true;
            std::istringstream in(payload);
            std::string magic, mig, verb, count;
            if (!(in >> magic >> mig >> verb >> count)) return fail("unexpected");
            // The count may be 0 — an empty answer — so it cannot ride
            // record_decimal (record.hpp refuses zero: a record generation
            // starts at 1). The per-entry generations below still can.
            std::uint64_t n = 0;
            bool digits = !count.empty() && count.size() <= 10;
            for (unsigned char ch : count) {
                if (ch < '0' || ch > '9') { digits = false; break; }
                n = n*10 + ch-'0';
            }
            if (!digits) return fail("unexpected");
            for (std::uint64_t i = 0; i < n; ++i) {
                std::string realm, character, id, gen, scene;
                std::uint64_t generation = 0;
                if (!(in >> realm >> character >> id >> gen >> scene)
                        || !record_decimal(gen, generation))
                    return fail("unexpected");
                v.destined.push_back({realm, character, id, scene, generation});
            }
            std::string extra;
            if (in >> extra && extra != "ENTITIES") return fail("unexpected");
            if (extra == "ENTITIES") {
                // ENTITIES <count> [<scope> <type> <incarnation> <entity> <id>
                // <generation> <state> <state-hex|->]... — the entity handoffs
                // ride the same poll. The count accepts zero like the avatar
                // count; the address fields parse exactly the way the MLD2
                // endpoint and the registry's own entity verbs do.
                std::string entity_count;
                if (!(in >> entity_count)) return fail("unexpected");
                std::uint64_t m = 0;
                bool entity_digits = !entity_count.empty() && entity_count.size() <= 10;
                for (unsigned char ch : entity_count) {
                    if (ch < '0' || ch > '9') { entity_digits = false; break; }
                    m = m*10 + ch-'0';
                }
                if (!entity_digits) return fail("unexpected");
                for (std::uint64_t i = 0; i < m; ++i) {
                    Verdict::EntityDestined row;
                    std::string scope, type, incarnation, entity, gen;
                    std::uint64_t wide = 0;
                    if (!(in >> scope >> type >> incarnation >> entity >> row.id >> gen
                            >> row.state >> row.state_hex))
                        return fail("unexpected");
                    const auto narrow = [&](const std::string& text, std::uint32_t& out) {
                        if (!state_decimal(text, wide) || wide > ~0ull>>32) return false;
                        out = static_cast<std::uint32_t>(wide);
                        return true;
                    };
                    std::int64_t entity_id = 0;
                    if (!narrow(scope, row.key.scope) || !narrow(type, row.key.type_id)
                            || !narrow(incarnation, row.key.incarnation)
                            || !state_int64(entity, entity_id) || !entity_id
                            || !state_decimal(gen, row.generation)
                            || row.state_hex.size() > 4096 || row.state_hex.size() % 2)
                        return fail("unexpected");
                    row.key.entity_id = entity_id;
                    if (row.state_hex == "-") row.state_hex.clear();
                    v.entity_destined.push_back(std::move(row));
                }
                std::string tail;
                if (in >> tail) return fail("unexpected");
            }
            return done(std::move(v));
        }
        if (payload.starts_with("MLR1 MIG EPREPARED ")
                || payload.starts_with("MLR1 MIG ESTEP ")
                || payload.starts_with("MLR1 MIG ECOMMITTED ")
                || payload.starts_with("MLR1 MIG EABORTED ")
                || payload.starts_with("MLR1 MIG ESTATE ")) {
            // The entity-migration answers. Every one names the request id;
            // the caller already knows the address (it built the frame), so
            // the verdict carries only what the caller cannot derive: the
            // coordinator's step word, the committed destination member and
            // the generation. ECOMMITTED's destination is one identifier
            // word, so the trailing generation parses positionally.
            Verdict v; v.applied = true;
            std::istringstream in(payload);
            std::string magic, mig, verb, id;
            if (!(in >> magic >> mig >> verb >> id)) return fail("unexpected");
            v.id = id;
            if (verb == "ESTEP" || verb == "ESTATE") {
                if (!(in >> v.state)) return fail("unexpected");
            } else if (verb == "ECOMMITTED") {
                if (!(in >> v.owner)) return fail("unexpected");
            }
            // ESTEP and EABORTED end at the state word / id; only ESTATE and
            // ECOMMITTED carry the generation.
            if (verb == "ESTATE" || verb == "ECOMMITTED" || verb == "EPREPARED") {
                std::string gen;
                if (!(in >> gen) || !record_decimal(gen, v.generation))
                    return fail("unexpected");
            }
            std::string extra;
            if (in >> extra) return fail("unexpected");
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
    void applied() { Verdict v; v.applied = true; done(std::move(v)); }
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
