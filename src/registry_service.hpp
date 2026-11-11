// SPDX-License-Identifier: Apache-2.0
// The MLR1 coordination plane, extracted from the node implementation: the
// coordinator-side registry with its journal, the member control session, the
// per-node member registration and the read-only placement client.
#pragma once
#include "node_internal.hpp"
#include "tcp_channel.hpp"
#include "return_sender.hpp"
#include "mistlane/cross.hpp"
#include "mistlane/journal.hpp"
#include "mistlane/migration.hpp"
#include "mistlane/record.hpp"
#include <cctype>
#include <charconv>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

namespace mistlane {

using Completion = Channel::Completion;

// Registry endpoints travel as "IP:PORT" text; "0" means none. Returns false
// instead of throwing when the text is not a connectable endpoint.
inline bool endpoint_text(std::string_view text, std::string& address, unsigned& port) {
    if (text == "0") { address.clear(); port = 0; return true; }
    const auto split = text.find_last_of(':');
    if (split == std::string_view::npos || split+1 >= text.size() || split == 0) return false;
    const auto raw_address = text.substr(0, split), raw_port = text.substr(split+1);
    unsigned value = 0;
    for (char ch : raw_port) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
        value = value*10 + static_cast<unsigned>(ch-'0');
    }
    if (value < 1 || value > 65535) return false;
    try { asio::ip::make_address(std::string(raw_address)); } catch (...) { return false; }
    address = std::string(raw_address);
    port = value;
    return true;
}

// Coordinator state: one entry per live member connection. Membership is
// connection-driven — a member stays registered while its control connection
// lives, heartbeats only refresh readiness and load. Listing order is ready
// first, then the lighter share of capacity, then the instance name, so a
// placement is deterministic for a given snapshot.
std::string registry_member_text(const std::string& namespace_) {
    return namespace_.empty() ? "-" : namespace_;
}
// Shared strict decimal generation parser. Zero is not a valid generation.
bool parse_generation(const std::string& text, std::uint64_t& generation) {
    if (text.empty() || text.size() > 19) return false;
    generation = 0;
    for (unsigned char ch : text) {
        if (ch < '0' || ch > '9') return false;
        if (generation > (std::numeric_limits<std::uint64_t>::max() - (ch-'0'))/10) return false;
        generation = generation*10 + (ch-'0');
    }
    return generation != 0;
}
class Registry {
public:
    explicit Registry(std::string journal_path = {}) {
        if (journal_path.empty()) return;
        journal_ = std::make_unique<MigrationJournal>(std::move(journal_path));
        // The durable authority cursor is loaded before journal rows: it is
        // the independent fence, not a value recomputed from whichever row
        // happened to survive a crash. It is keyed by the same composite
        // (namespace, realm, character) as the migration map so two groups can
        // never share a home cursor.
        std::map<std::string, std::uint64_t> authority_cursor;
        for (auto record : journal_->restore_authorities()) {
            const auto key = migration_key(record.namespace_, record.key);
            if (!authority_cursor.emplace(key.first, record.cursor).second)
                throw std::invalid_argument("migration journal home authority duplicated");
        }
        for (auto record : journal_->restore()) {
            const auto& snapshot = record.snapshot;
            if (!valid_migration_owner(snapshot.route.owner) || !snapshot.route.generation
                    || snapshot.key != record.key) throw std::invalid_argument("migration journal snapshot invalid");
            for (const auto& [id, entry] : snapshot.journal) {
                (void)id;
                if (entry.request.key != record.key || entry.token.key != record.key
                        || !entry.token.generation) throw std::invalid_argument("migration journal request invalid");
            }
            auto authority = std::make_shared<HomeGenerationAuthority>();
            const auto entry_key = migration_key(record.namespace_, record.key);
            const auto cursor = authority_cursor.find(entry_key.first);
            if (cursor != authority_cursor.end()) {
                HomeGenerationAuthority::View view;
                view.cursor.emplace(record.key, cursor->second);
                authority->restore(view);
            }
            // Legacy files can predate the authority table; the snapshot
            // high-water mark is the only safe fallback and is republished as
            // the durable cursor below.
            if (!authority->issued(record.key)) {
                std::uint64_t issued = snapshot.route.generation;
                for (const auto& [id, entry] : snapshot.journal)
                    issued = std::max(issued, entry.token.generation);
                if (!authority->seed(record.key, issued)) throw std::invalid_argument("migration journal authority invalid");
            }
            auto transaction = std::make_shared<MigrationTransaction>(*authority, std::move(record.snapshot));
            // Startup replay: a crash leaves live transactions everywhere
            // between prepare and terminal. Drive each to its deterministic
            // recovery state before the coordinator accepts traffic, and
            // persist it so a second restart replays the recovered snapshot
            // instead of re-scanning the pre-crash one. A fenced transaction
            // awaiting the destination ACK stays pending here.
            const auto recovery = transaction->recover();
            if (recovery != MigrationResult::Applied && recovery != MigrationResult::Pending
                    && recovery != MigrationResult::Duplicate)
                throw std::invalid_argument("migration journal recovery failed");
            if (recovery == MigrationResult::Applied || recovery == MigrationResult::Pending)
                journal_->save({record.namespace_, record.key, transaction->snapshot()},
                    authority->issued(record.key));
            MigrationEntry entry{std::move(record.namespace_), record.key, std::move(authority), std::move(transaction)};
            const auto key = migration_key(entry.namespace_, entry.key);
            if (!migrations_.emplace(key, std::move(entry)).second)
                throw std::invalid_argument("migration journal contains a duplicate participant");
        }
        // Durable placements: a coordinator restart must not forget which live
        // member was last assigned a character. The member control leg died
        // with the crash, so a restored placement answers EMPTY until that
        // member re-registers (route() requires a live member), and the
        // route record itself survives in the journal across the restart.
        for (const auto& record : journal_->restore_placements()) {
            const auto key = route_key(record.namespace_, record.realm, record.character);
            if (!routes_.emplace(key,
                    Route{record.scope, record.role, record.instance, record.scene,
                          record.generation}).second)
                throw std::invalid_argument("placement journal contains a duplicate route");
        }
    }
    struct Member {
        std::string role, instance, scope, namespace_, listen;
        unsigned probe = 0;
        bool ready = false;
        std::size_t active = 0, capacity = 0;
    };
    bool upsert(Member member, void* owner) {
        const auto key = member_key(member.namespace_, member.scope, member.role, member.instance);
        auto found = members_.find(key);
        if (found == members_.end()) {
            members_.emplace(std::move(key), std::make_pair(std::move(member), owner));
            return true;
        }
        // A duplicate key from another live connection loses: one owner per
        // (namespace, scope, role, instance), and a freshest registration
        // wins nothing.
        if (found->second.second != owner) return false;
        found->second.first = std::move(member);
        return true;
    }
    bool retire(const std::string& namespace_, const std::string& scope, const std::string& role,
                const std::string& instance,
                void* owner) {
        auto found = members_.find(member_key(namespace_, scope, role, instance));
        if (found == members_.end() || found->second.second != owner) return false;
        members_.erase(found);
        drop_routes(scope, namespace_, role, instance);
        retire_members(scope, namespace_, role, instance);
        return true;
    }
    // Character route record: the registry also remembers which live member
    // currently hosts a character, so a reconnect or a cross return has
    // something authoritative to ask about. A route is only accepted against a
    // registered member and dies with that member, so a dead host can never be
    // routed to.
    struct Route {
        std::string scope, role, instance;
        std::string scene;                  // the scene the owner reported
        std::uint64_t generation = 0; // the generation the coordinator last published
    };
    struct RouteAnswer {
        const Member* member = nullptr;
        std::string scene = "-";
        std::uint64_t generation = 0;
    };
    bool set_route(const std::string& scope, const std::string& realm,
                   const std::string& character, const std::string& role,
                   const std::string& instance, const std::string& namespace_,
                   std::uint64_t generation = 0, const std::string& scene = "-") {
        if (!members_.count(member_key(namespace_, scope, role, instance))) return false;
        // The journal write leads: a STORAGE refusal must mean the route was
        // not applied anywhere, not a memory/journal divergence a restart
        // would resolve against the memory view.
        persist_placement(namespace_, scope, realm, character, role, instance, scene, generation);
        routes_[route_key(namespace_, realm, character)] =
            Route{scope, role, instance, scene, generation};
        return true;
    }
    // A committed migration moves the character route to the new owner; the
    // route keeps pointing at the live member so readiness stays authoritative.
    bool set_route(const MigrationOwner& owner, const std::string& realm,
                   const std::string& character, const std::string& namespace_,
                   std::uint64_t generation) {
        const bool realm_scope = owner.scope.kind == MigrationScope::Kind::Realm;
        return set_route(realm_scope ? owner.scope.realm : owner.scope.cross_group,
            realm, character, owner.role, owner.instance, namespace_, generation, owner.scene);
    }
    // A destination that left the registry cannot receive the committed route;
    // the commit must stay pending so the fenced gap remains takeover-eligible
    // instead of answering COMMITTED with a route the registry cannot serve.
    bool route_publishable(const MigrationOwner& owner, const std::string& namespace_) const {
        const bool realm_scope = owner.scope.kind == MigrationScope::Kind::Realm;
        return members_.count(member_key(namespace_, realm_scope ? owner.scope.realm : owner.scope.cross_group,
            owner.role, owner.instance)) != 0;
    }
    std::optional<RouteAnswer> route(const std::string& namespace_, const std::string& realm,
                                     const std::string& character) const {
        const MigrationKey key{realm, character};
        const auto found = routes_.find(route_key(namespace_, realm, character));
        if (found == routes_.end()) return std::nullopt;
        const auto& hosted = found->second;
        const auto member =
            members_.find(member_key(namespace_, hosted.scope, hosted.role, hosted.instance));
        if (member == members_.end()) return std::nullopt;
        // The committed route names the live member; scene/generation come
        // from the transaction the commit published, if one is still open.
        std::string scene = "-";
        std::uint64_t generation = 0;
        if (const auto transaction = this->transaction(namespace_, key)) {
            const auto route = transaction->route();
            scene = route.owner.scene;
            generation = route.generation;
        } else {
            // A committed route whose transaction was pruned or never opened:
            // answer the durable comparison baseline published at commit.
            scene = hosted.scene;
            generation = hosted.generation;
        }
        return RouteAnswer{&member->second.first, scene, generation};
    }
    static std::string member_key(const std::string& namespace_, const std::string& scope,
                                  const std::string& role, const std::string& instance) {
        return scope+'\x1'+role+'\x1'+instance+'\x1'+namespace_;
    }
    static std::string route_key(const std::string& namespace_, const std::string& realm,
                                 const std::string& character) {
        return namespace_+'\x1'+realm+'\x1'+character;
    }
    std::size_t routes() const { return routes_.size(); }
    std::vector<const Member*> list(const std::string& namespace_, const std::string& role,
                                    const std::string& scope) const {
        std::vector<const Member*> found;
        for (const auto& [key, entry] : members_)
            if (entry.first.namespace_ == namespace_
                    && (role.empty() || entry.first.role == role)
                    && (scope.empty() || entry.first.scope == scope))
                found.push_back(&entry.first);
        std::sort(found.begin(), found.end(), [](const Member* a, const Member* b) {
            if (a->ready != b->ready) return a->ready;
            const auto share = [](const Member* m) { return m->capacity
                ? static_cast<double>(m->active)/static_cast<double>(m->capacity) : 1.0; };
            if (share(a) != share(b)) return share(a) < share(b);
            return a->instance < b->instance;
        });
        return found;
    }
    std::size_t size() const { return members_.size(); }
    // Migration state, all on the node's event-loop thread like members_ and
    // routes_: one volatile transaction per (realm, character), seeded from
    // the home facts a service publishes. The coordinator stores facts, it is
    // not the generation authority — home data publishes them, and the
    // candidates handed out here are strictly newer than what was published.
    bool owner_transaction(const MigrationKey& key, std::uint64_t generation,
                           const std::string& namespace_, const MigrationOwner& owner) {
        if (!valid_migration_owner(owner) || !generation) return false;
        const auto storage_key = migration_key(namespace_, key);
        const auto found = migrations_.find(storage_key);
        if (found != migrations_.end()) {
            // Facts are adopted once: a restatement must match the route the
            // coordinator already holds, and a changed fact is a conflict.
            return found->second.transaction->route() == MigrationRoute{owner, generation};
        }
        // The transaction holds the authority by reference, so both must share
        // one lifetime in the map — a local authority would leave the reference
        // dangling as soon as this function returns.
        MigrationEntry fresh{namespace_, key, std::make_shared<HomeGenerationAuthority>(), nullptr};
        if (!fresh.authority->seed(key, generation)) return false; // already seeded, never reseeds
        fresh.transaction = std::make_shared<MigrationTransaction>(
            *fresh.authority, key, MigrationRoute{owner, generation});
        const auto [it, inserted] = migrations_.emplace(storage_key, std::move(fresh));
        if (!inserted) return false;
        persist_transaction(it->second);
        return true;
    }
    std::shared_ptr<MigrationTransaction> transaction(const std::string& namespace_,
                                                      const MigrationKey& key) const {
        const auto found = migrations_.find(migration_key(namespace_, key));
        return found == migrations_.end() ? nullptr : found->second.transaction;
    }
    static MigrationKey migration_key(const std::string& namespace_, const MigrationKey& key) {
        return {namespace_+'\x1'+key.first+'\x1'+key.second, ""};
    }
    void persist_transaction(const std::string& namespace_, const MigrationKey& key,
                             const MigrationTransaction& transaction) {
        if (!journal_) return;
        const auto entry_key = migration_key(namespace_, key);
        const auto found = migrations_.find(entry_key);
        if (found == migrations_.end()) return;
        journal_->save({namespace_, key, transaction.snapshot()}, found->second.authority->issued(key));
    }
    static std::string line(const Member& member) {
        std::ostringstream out;
        out << "member " << member.role << ' ' << member.instance << ' '
            << member_text(member.namespace_) << ' ' << member.scope << ' '
            << member.listen << ' ' << member.probe << ' '
            << (member.ready ? "ready" : "notready") << ' '
            << member.active << '/' << member.capacity;
        return out.str();
    }
    static std::string member_text(const std::string& namespace_) {
        return namespace_.empty() ? "-" : namespace_;
    }
private:
    // A member that retires orphans every route that pointed at it: routes
    // follow the member, they never outlive it.
    void drop_routes(const std::string& scope, const std::string& namespace_, const std::string& role,
                     const std::string& instance) {
        for (auto it = routes_.begin(); it != routes_.end();) {
            const auto namespace_start = it->first.find('\x1');
            const auto& hosted = it->second;
            if (it->first.substr(0, namespace_start) == namespace_ && hosted.scope == scope
                    && hosted.role == role && hosted.instance == instance)
                it = routes_.erase(it);
            else ++it;
        }
        if (journal_) journal_->drop_placement(namespace_, scope, role, instance);
    }
    // A member's leave is a retirement claim for every live transaction that
    // depends on that process. The registry knows a process, not a scene, so
    // the claim is matched on (scope, role, instance) and issued with the
    // transaction's own owner value.
    void retire_members(const std::string& scope, const std::string& namespace_, const std::string& role,
                        const std::string& instance) {
        const auto matches = [&](const MigrationOwner& owner) {
            return owner.scope.namespace_ == namespace_ && owner.role == role
                && owner.instance == instance
                && (owner.scope.realm == scope || owner.scope.cross_group == scope);
        };
        for (const auto& [key, entry] : migrations_) {
            const auto& transaction = entry.transaction;
            const auto snapshot = transaction->snapshot();
            if (snapshot.current.empty()) continue;
            const auto* row = transaction->entry(snapshot.current);
            if (!row) continue;
            MigrationResult result = MigrationResult::Invalid;
            if (matches(row->request.expected.owner))
                result = transaction->retire_member(row->token, row->request.expected.owner);
            else if (matches(row->request.destination))
                result = transaction->retire_member(row->token, row->request.destination);
            if (result == MigrationResult::Applied) persist_transaction(entry);
        }
    }
    // The authority and its transaction share the map entry's lifetime: the
    // transaction keeps the authority by reference.
    struct MigrationEntry {
        std::string namespace_;
        MigrationKey key;
        std::shared_ptr<HomeGenerationAuthority> authority;
        std::shared_ptr<MigrationTransaction> transaction;
    };
    void persist_transaction(const MigrationEntry& entry) {
        persist_transaction(entry.namespace_, entry.key, *entry.transaction);
    }
    void persist_placement(const std::string& namespace_, const std::string& scope,
                           const std::string& realm, const std::string& character,
                           const std::string& role, const std::string& instance,
                           const std::string& scene, std::uint64_t generation) {
        if (!journal_) return;
        journal_->save_placement({namespace_, scope, realm, character, role, instance, scene, generation});
    }
    std::map<std::string, std::pair<Member, void*>> members_;
    std::map<std::string, Route> routes_;
    std::map<MigrationKey, MigrationEntry> migrations_;
    std::unique_ptr<MigrationJournal> journal_;
};
// One coordinator control connection: a member registration with its heartbeat
// channel, or a one-shot list/place query. The coordinator forwards no gameplay
// frame, so the role cannot quietly drift into being a router.
class RegistrySession : public std::enable_shared_from_this<RegistrySession> {
public:
    RegistrySession(std::shared_ptr<Channel> channel, Registry& registry, const Config& config,
            Diagnostics& diagnostics, std::function<void(RegistrySession*)> on_close)
      : channel_(std::move(channel)), timer_(channel_->executor()), registry_(registry),
        config_(config), diagnostics_(diagnostics), on_close_(std::move(on_close)) {}
    void start() { channel_->start([self=shared_from_this()](Error) { self->pump(); }); }
    void stop() {
        if (closed_) return;
        closed_ = true;
        timer_.cancel();
        if (member_) {
            registry_.retire(member_->namespace_, member_->scope, member_->role,
                member_->instance, this);
            member_.reset();
        }
        // Retiring a member may have dropped its routes; the gauge follows.
        diagnostics_.set_registry_routes(registry_.routes());
        channel_->close();
        on_close_(this);
    }
private:
    void deadline(int seconds) {
        const auto generation = ++generation_;
        timer_.expires_after(std::chrono::seconds(seconds));
        timer_.async_wait([self=shared_from_this(), generation](Error ec) {
            if (!ec && self->generation_ == generation) self->stop();
        });
    }
    void pump() {
        if (closed_ || reading_) return;
        reading_ = true;
        channel_->read([self=shared_from_this()](Error ec, std::string payload) {
            self->reading_ = false;
            if (self->closed_) return;
            if (ec) return self->stop();
            self->dispatch(std::move(payload));
        });
    }
    void reply(std::string payload) {
        channel_->write(std::move(payload), [self=shared_from_this()](Error ec) {
            if (ec) return self->stop();
            self->pump();
        });
    }
    void close_with(std::string payload) {
        channel_->write(std::move(payload), [self=shared_from_this()](Error) { self->stop(); });
    }
    void refuse(std::string_view reason) {
        std::cerr << "registry refused: " << reason << '\n';
        close_with("MLR1 ERR " + std::string(reason));
    }
    void refuse_service(const char* magic, const char* reason) {
        std::cerr << "service handshake rejected: " << reason << '\n';
        close_with(std::string(magic)+" ERR "+reason);
    }
    void dispatch(std::string payload) {
        // Registry lines are tiny; a migration line carries two owners.
        if (payload.size() > (payload.starts_with("MLR1 MIG") ? 1024 : 256)) return refuse("INVALID");
        std::istringstream in(payload);
        std::string magic;
        in >> magic;
        if (magic == "MLS1") return identity(payload);
        // A tokened coordinator only serves identified connections: the
        // handshake has to precede any registry traffic.
        if (!config_.service_token.empty() && !service_ok_) return refuse_service("MLS1", "TOKEN");
        if (magic != "MLR1") {
            if (!member_) return refuse("UNEXPECTED");
            std::istringstream status(payload);
            std::string state, active_text, capacity_text, extra;
            std::size_t active = 0, capacity = 0;
            if (!(status >> state >> active_text >> capacity_text) || (status >> extra)
                    || (state != "ready" && state != "notready")) return refuse("INVALID");
            try {
                active = static_cast<std::size_t>(std::stoull(active_text));
                capacity = static_cast<std::size_t>(std::stoull(capacity_text));
            } catch (...) {
                return refuse("INVALID");
            }
            if (capacity == 0 || capacity > 1000000 || active > capacity) return refuse("INVALID");
            member_->ready = state == "ready";
            member_->active = active;
            member_->capacity = capacity;
            // A heartbeat moves the member in the listing, not just the copy.
            if (!registry_.upsert(*member_, this)) return refuse("INVALID");
            return reply("MLR1 OK");
        }
        if (payload.starts_with("MLR1 MIG")) {
            try {
                return answer(migration_line(payload));
            } catch (const std::exception& error) {
                std::cerr << "registry migration journal failed: " << error.what() << '\n';
                return refuse("STORAGE");
            }
        }
        if (payload.starts_with("MLR1 REGISTER")) return register_member(payload);
        if (member_) return heartbeat(payload);
        return registry_query(payload);
    }
    // Identity stacks on the registry: the coordinator's MLS1 verdict precedes
    // the first MLR1 frame, and a stray handshake is refused like anywhere else.
    void identity(const std::string& payload) {
        if (service_ok_ || config_.service_token.empty()) return refuse_service("MLS1", "UNEXPECTED");
        std::string role, instance, token;
        if (!service_fields(payload, role, instance, token)) return refuse_service("MLS1", "INVALID");
        if (token.size() != config_.service_token.size()
                || CRYPTO_memcmp(token.data(), config_.service_token.data(), token.size()) != 0)
            return refuse_service("MLS1", "TOKEN");
        service_ok_ = true;
        reply("MLS1 OK");
    }
    void register_member(const std::string& payload) {
        std::istringstream in(payload);
        std::string magic, command, role, instance, namespace_, scope, listen,
                    probe = "0", extra;
        in >> magic >> command >> role >> instance;
        if (role != "gameServer" && role != "crossServer") return refuse("INVALID");
        // "<namespace>|NS <namespace>" — a bare token names the group, so the
        // positional spellings stay valid and "-" keeps the default one.
        {
            const auto tail = in.tellg();
            std::string marker;
            if (tail >= 0 && (in >> marker)) {
                if (marker == "NS") {
                    if (!namespace_fields(in, namespace_)) return refuse("INVALID");
                } else if (marker.size() > 64 || (marker != "-" && !record_identifier(marker))) {
                    return refuse("INVALID");
                } else if (marker != "-") {
                    namespace_ = marker;
                }
            } else if (tail >= 0) {
                in.clear();
                in.seekg(tail);
            }
        }
        if (!(in >> scope >> listen >> probe) || (in >> extra)) return refuse("INVALID");
        if (!record_identifier(instance) || !record_identifier(scope)) return refuse("INVALID");
        Registry::Member member;
        member.role = role;
        member.instance = instance;
        member.namespace_ = namespace_;
        member.scope = scope;
        std::string address;
        unsigned port = 0;
        if (!endpoint_text(listen, address, port)) return refuse("INVALID");
        member.listen = address+':'+std::to_string(port);
        if (!endpoint_text(probe, address, port)) return refuse("INVALID");
        member.probe = port;
        if (!registry_.upsert(member, this)) return refuse("INVALID");
        member_ = member;
        std::cout << "member registered " << role << ' ' << instance << ' '
                  << namespace_ << ' ' << scope << std::endl;
        diagnostics_.set_registry_members(registry_.size());
        reply("MLR1 OK");
    }
    void heartbeat(const std::string& payload) {
        if (!member_) return refuse("UNEXPECTED");
        // A registered member may also publish or look up character routes on
        // its control connection; the leg stays open afterwards.
        if (payload.starts_with("MLR1 ROUTE")) return route_frame(payload);
        std::istringstream in(payload);
        std::string magic, state, extra;
        std::size_t active = 0, capacity = 0;
        in >> magic;
        if (in >> state) {
            const bool values = static_cast<bool>(in >> active >> capacity);
            const bool trailing = static_cast<bool>(in >> extra);
            if (!values || trailing) return refuse("INVALID");
        }
        if ((state != "ready" && state != "notready") || capacity == 0 || capacity > 1000000
                || active > capacity) return refuse("INVALID");
        member_->ready = state == "ready";
        member_->active = active;
        member_->capacity = capacity;
        const bool saved = registry_.upsert(*member_, this);
        if (!saved) return refuse("INVALID");
        reply("MLR1 OK");
    }
    void registry_query(const std::string& payload) {
        if (payload.starts_with("MLR1 ROUTE")) return route_frame(payload);
        std::istringstream in(payload);
        std::string magic, command, role, namespace_, scope, extra;
        in >> magic;
        const auto command_position = in.tellg();
        in >> command >> role;
        if (command != "QUERY" && command != "PLACE") {
            if (command == "ready" && (role == "0" || role.empty()) && in.eof())
                return heartbeat("ready 0 0");
            if (command != "ready" && command != "notready") return refuse("UNEXPECTED");
            if (command_position >= 0) { in.clear(); in.seekg(command_position); }
            std::string state;
            in >> state;
            if (state == command && in.eof()) return heartbeat(state);
            return refuse("UNEXPECTED");
        }
        // Tail grammar: "<role> [<namespace>|NS <namespace>] [<scope>]" — role
        // "-" lists every role, and an absent or "-" namespace filter names the
        // default group, so a bare "MLR1 QUERY -" stays inside its own group.
        // The keyword and positional spellings both carry a namespace.
        {
            const auto tail = in.tellg();
            std::string ns;
            if (tail >= 0 && (in >> ns)) {
                if (ns == "NS") {
                    if (!namespace_fields(in, namespace_)) return refuse("INVALID");
                    in >> scope;
                } else {
                    if (ns.size() > 64 || (ns != "-" && !record_identifier(ns))) return refuse("INVALID");
                    if (ns != "-") namespace_ = ns;
                    in >> scope;
                }
            }
        }
        if (scope == "-") scope.clear();
        if (!scope.empty() && !record_identifier(scope)) return refuse("INVALID");
        if (in >> extra) return refuse("INVALID");
        if (command == "QUERY") {
            const auto members = registry_.list(namespace_, role == "-" ? "" : role, scope);
            if (members.empty()) {
                return close_with("MLR1 NONE");
            }
            std::ostringstream out;
            out << "MLR1 MEMBERS " << members.size();
            for (const auto* member : members) out << '\n' << Registry::line(*member);
            return close_with(out.str());
        }
        if (role.empty() || role == "-" || scope.empty()) return refuse("INVALID");
        for (const auto* member : registry_.list(namespace_, role, scope)) {
            if (!member->ready) continue;
            std::ostringstream out;
            out << "MLR1 PLACE " << member->instance << ' ' << member->listen << ' '
                << member->probe << " ready " << member->active << '/' << member->capacity;
            return close_with(out.str());
        }
        return close_with("MLR1 EMPTY");
    }
    // One-shot query legs close after the verdict, like QUERY/PLACE; a member's
    // control leg replies and keeps heartbeating.
    void answer(std::string payload) {
        if (member_) return reply(std::move(payload));
        close_with(std::move(payload));
    }
    // MLR1 ROUTE SET NS <namespace> <scope> <realm> <character> <role> <instance>
    //   -> MLR1 ROUTE OK | MLR1 ERR UNKNOWN | MLR1 ERR INVALID | MLR1 ERR STORAGE
    // MLR1 ROUTE GET NS <namespace> <realm> <character>
    //   -> MLR1 ROUTE AT <scope> <role> <instance> <listen> <probe>
    //      <ready|notready> <active>/<capacity>
    //   | MLR1 ROUTE EMPTY | MLR1 ERR INVALID
    // A route is only written against a member that is registered right now, so
    // it can never point at a host the registry does not know, and GET answers
    // from the live member state so a sick host reads as notready. SET persists
    // the placement through the journal; a journal failure must degrade into a
    // named refusal on this frame (and keep the registry serving) instead of
    // unwinding through the event loop.
    void route_frame(const std::string& payload) {
        try {
            route_frame_body(payload);
        } catch (const std::exception&) {
            refuse("STORAGE");
        }
    }
    void route_frame_body(const std::string& payload) {
        std::istringstream in(payload);
        std::string magic, command, sub, namespace_, spare, marker;
        in >> magic >> command >> sub;
        if (sub != "SET" && sub != "GET") return refuse("UNEXPECTED");
        // The NS group is required here: a route frame without it is the older
        // two-argument spelling and must not read as the empty namespace.
        if (!(in >> marker) || marker != "NS" || !namespace_fields(in, namespace_))
            return refuse("INVALID");
        if (sub == "SET") {
            std::string scope, realm, character, role, instance, generation_text, scene = "-";
            std::uint64_t generation = 0;
            if (!(in >> scope >> realm >> character >> role >> instance)) return refuse("INVALID");
            if (in >> generation_text) {
                // The comparison baseline is optional and, when a scene is also
                // given, precedes it. A "-" generation keeps the baseline unset
                // while still carrying the scene on the same frame.
                if (generation_text != "-") {
                    if (!parse_generation(generation_text, generation)) return refuse("INVALID");
                }
                if (in >> scene) {
                    if (in >> spare) return refuse("INVALID");
                }
            }
            if (!record_identifier(scope) || !record_identifier(realm) ||
                    !record_identifier(character) || !record_identifier(instance) ||
                    (role != "gameServer" && role != "crossServer")
                    || !record_identifier(scene))
                return refuse("INVALID");
            if (!registry_.set_route(scope, realm, character, role, instance, namespace_,
                    generation, scene))
                return refuse("UNKNOWN");
            diagnostics_.set_registry_routes(registry_.routes());
            return answer("MLR1 ROUTE OK");
        }
        if (sub == "GET") {
            std::string realm, character;
            if (!(in >> realm >> character) || (in >> spare)) return refuse("INVALID");
            if (!record_identifier(realm) || !record_identifier(character)) return refuse("INVALID");
            const auto route = registry_.route(namespace_, realm, character);
            if (!route || !route->member) return answer("MLR1 ROUTE EMPTY");
            const auto* member = route->member;
            std::ostringstream out;
            out << "MLR1 ROUTE AT " << member->scope << ' ' << member->role << ' '
                << member->instance << ' ' << member->listen << ' ' << member->probe
                << ' ' << (member->ready ? "ready" : "notready") << ' '
                << member->active << '/' << member->capacity
                << ' ' << route->scene << ' ' << route->generation;
            return answer(out.str());
        }
        return refuse("UNEXPECTED");
    }
    // MLR1 MIG: the migration model on the wire. It is registry traffic, so a
    // tokened coordinator gates it behind MLS1 like any other leg, and every
    // answer is keyed by the request id — a replayed frame repeats its verdict
    // instead of allocating a newer generation. Facts arrive with OWNER; the
    // coordinator stores them, it is not the generation authority.
    //   MLR1 MIG OWNER   [<namespace>] <realm> <character> <generation> <owner>
    //   MLR1 MIG PREPARE [<namespace>] <realm> <character> <id> <generation>
    //     <expected> <destination>
    //   MLR1 MIG EVENT   [<namespace>] <realm> <character> <id> <generation>
    //     <DRAINED|FENCE|READY|CLEANUP>
    //   MLR1 MIG COMMIT  [<namespace>] <realm> <character> <id> <generation>
    //   MLR1 MIG ABORT   [<namespace>] <realm> <character> <id> <generation>
    //   MLR1 MIG RETIRE  [<namespace>] <realm> <character> <id> <generation> <member>
    //   MLR1 MIG TAKEOVER [<namespace>] <realm> <character> <id> <generation>
    //     <expected> <destination>
    //   MLR1 MIG STATUS  [<namespace>] <realm> <character>
    // where <namespace> is the optional "NS <identifier>" group — "-" or
    // omitted names the default group, and a bare positional namespace is
    // refused so an older frame keeps meaning the default group — and
    // <owner> is "<realm|cross> NS <namespace> <scope> <role> <instance> <scene>".
    //   -> MLR1 MIG OWNER OK | PREPARED <id> <generation> | STEP <id> <STATE>
    //    | COMMITTED <id> <owner> <generation> | ABORTING <id> | ABORTED <id>
    //    | PENDING <id>
    //    | STATE idle <owner> <generation> | STATE <id> <STATE> <drained|open>
    //      <fenced|unfenced> <ready|notready> <generation>
    //    | MLR1 EMPTY | MLR1 ERR INVALID | STALE | CONFLICT | ORDER | UNEXPECTED
    // Owner wire text: "<realm|cross> NS <namespace> <scope> <role>
    // <instance> <scene>" — the same NS group as the registry verbs, so a
    // namespaced owner and a default-group owner never read alike.
    static std::string owner_text(const MigrationOwner& owner) {
        std::ostringstream out;
        const bool realm = owner.scope.kind == MigrationScope::Kind::Realm;
        out << (realm ? "realm" : "cross") << " NS " << registry_member_text(owner.scope.namespace_) << ' '
            << (realm ? owner.scope.realm : owner.scope.cross_group)
            << ' ' << owner.role << ' ' << owner.instance << ' ' << owner.scene;
        return out.str();
    }
    static bool owner_fields(std::istream& in, MigrationOwner& owner) {
        std::string kind, marker, namespace_, scope, role, instance, scene;
        if (!(in >> kind >> marker >> namespace_ >> scope >> role >> instance >> scene)
                || marker != "NS"
                || (namespace_.size() > 64 || (namespace_ != "-" && !record_identifier(namespace_))))
            return false;
        if (namespace_ != "-") owner.scope.namespace_ = namespace_;
        if (kind == "realm") {
            owner.scope.kind = MigrationScope::Kind::Realm;
            owner.scope.realm = scope;
        } else if (kind == "cross") {
            owner.scope.kind = MigrationScope::Kind::CrossGroup;
            owner.scope.cross_group = scope;
        } else return false;
        owner.role = role;
        owner.instance = instance;
        owner.scene = scene;
        return true;
    }
    static bool migration_generation(const std::string& text, std::uint64_t& generation) {
        return parse_generation(text, generation);
    }
    static bool namespace_fields(std::istream& in, std::string& namespace_) {
        std::string token;
        if (!(in >> token)) {
            namespace_.clear();
            return true;
        }
        if (token.size() > 64 || (token != "-" && !record_identifier(token))) return false;
        namespace_ = token == "-" ? std::string() : token;
        return true;
    }
    static bool registry_namespace_fields(std::istream& in, std::string& namespace_) {
        const auto position = in.tellg();
        std::string marker;
        if (position >= 0 && (in >> marker) && marker == "NS") {
            return namespace_fields(in, namespace_);
        }
        if (position >= 0) { in.clear(); in.seekg(position); }
        namespace_.clear();
        return true;
    }
    static bool migration_token(std::istream& in, MigrationToken& token) {
        std::string id, generation;
        if (!(in >> token.id >> generation) || !record_identifier(token.id)) return false;
        return migration_generation(generation, token.generation);
    }
    static std::string migration_state(MigrationState state) {
        switch (state) {
        case MigrationState::Preparing: return "PREPARING";
        case MigrationState::Aborting: return "ABORTING";
        case MigrationState::Recovering: return "RECOVERING";
        case MigrationState::Committed: return "COMMITTED";
        case MigrationState::Aborted: return "ABORTED";
        default: return "IDLE";
        }
    }
    static std::string migration_reason(MigrationResult result) {
        switch (result) {
        case MigrationResult::Stale: return "STALE";
        case MigrationResult::Conflict: return "CONFLICT";
        case MigrationResult::Order: return "ORDER";
        case MigrationResult::Pending: return "PENDING";
        default: return "INVALID";
        }
    }
    // Applied and Duplicate answer the same line: idempotent by request id.
    static std::string migration_verdict(const MigrationToken& token, MigrationResult result,
                                         const std::string& success) {
        if (result == MigrationResult::Applied || result == MigrationResult::Duplicate) return success;
        if (result == MigrationResult::Pending) return "MLR1 MIG PENDING " + token.id;
        return "MLR1 ERR " + migration_reason(result);
    }
    static std::string migration_state_line(const MigrationTransaction::Entry& row) {
        std::ostringstream out;
        out << "MLR1 MIG STATE " << row.token.id << ' ' << migration_state(row.state) << ' '
            << (row.source_drained ? "drained" : "open") << ' '
            << (row.home_fenced ? "fenced" : "unfenced") << ' '
            << (row.destination_ready ? "ready" : "notready") << ' ' << row.token.generation;
        return out.str();
    }
    std::string migration_line(const std::string& payload) {
        std::istringstream in(payload);
        std::string magic, tag, verb, namespace_, realm, character, generation, event, extra;
        if (!(in >> magic >> tag)) return "MLR1 ERR UNEXPECTED";
        if (tag != "MIG") return "MLR1 ERR INVALID";
        if (!(in >> verb)) return "MLR1 ERR UNEXPECTED";
        if (!registry_namespace_fields(in, namespace_)) return "MLR1 ERR INVALID";
        if (!(in >> realm >> character) || !record_identifier(realm)
                || !record_identifier(character))
            return "MLR1 ERR INVALID";
        const MigrationKey key{realm, character};
        (void)Registry::migration_key(namespace_, key);
        if (verb == "OWNER") {
            std::uint64_t value = 0;
            MigrationOwner owner;
            // A shape-invalid owner is INVALID; only a disagreement with facts
            // the coordinator already holds is CONFLICT.
            if (!(in >> generation) || !migration_generation(generation, value)
                    || !owner_fields(in, owner) || !valid_migration_owner(owner) || (in >> extra))
                return "MLR1 ERR INVALID";
            if (!registry_.owner_transaction(key, value, namespace_, owner))
                return "MLR1 ERR CONFLICT";
            return "MLR1 MIG OWNER OK";
        }
        if (verb == "PREPARE") {
            MigrationToken token;
            MigrationRequest request;
            if (!migration_token(in, token) || !owner_fields(in, request.expected.owner)
                    || !owner_fields(in, request.destination) || (in >> extra)) return "MLR1 ERR INVALID";
            const auto tx = registry_.transaction(namespace_, key);
            if (!tx) return "MLR1 ERR EMPTY";
            request.key = key;
            request.id = token.id;
            request.expected.generation = token.generation;
            const auto result = tx->prepare(request);
            if (result != MigrationResult::Applied && result != MigrationResult::Duplicate)
                return "MLR1 ERR " + migration_reason(result);
            if (result == MigrationResult::Applied) registry_.persist_transaction(namespace_, key, *tx);
            const auto* row = tx->entry(token.id);
            std::ostringstream out;
            out << "MLR1 MIG PREPARED " << token.id << ' ' << (row ? row->token.generation : 0);
            return out.str();
        }
        if (verb == "EVENT") {
            MigrationToken token;
            MigrationEvent step;
            if (!migration_token(in, token) || !(in >> event) || (in >> extra)) return "MLR1 ERR INVALID";
            token.key = key;
            if (event == "DRAINED") step = MigrationEvent::SourceDrained;
            else if (event == "FENCE") step = MigrationEvent::HomeFence;
            else if (event == "READY") step = MigrationEvent::DestinationReady;
            else if (event == "CLEANUP") step = MigrationEvent::Cleanup;
            else return "MLR1 ERR INVALID";
            const auto tx = registry_.transaction(namespace_, key);
            if (!tx) return "MLR1 ERR EMPTY";
            const auto result = tx->apply(token, step);
            if (result == MigrationResult::Applied) registry_.persist_transaction(namespace_, key, *tx);
            // The step answer names the state the event produced, so read it
            // strictly after apply — argument evaluation order is unspecified.
            const std::string stepped = "MLR1 MIG STEP " + token.id + " " + migration_state(tx->state());
            return migration_verdict(token, result, stepped);
        }
        if (verb == "COMMIT") {
            MigrationToken token;
            if (!migration_token(in, token) || (in >> extra)) return "MLR1 ERR INVALID";
            token.key = key;
            const auto tx = registry_.transaction(namespace_, key);
            if (!tx) return "MLR1 ERR EMPTY";
            // Reuse the transaction's COMMIT admission source so only a valid,
            // ready commit is downgraded to PENDING for a dead destination.
            if (tx->commit_eligible(token)
                && !registry_.route_publishable(tx->entry(token.id)->request.destination, namespace_))
                return "MLR1 ERR PENDING";
            const auto result = tx->apply(token, MigrationEvent::Commit);
            if (result == MigrationResult::Applied) {
                const auto route = tx->route();
                registry_.persist_transaction(namespace_, key, *tx);
                registry_.set_route(route.owner, key.first, key.second, namespace_, route.generation);
            }
            const auto* row = tx->entry(token.id);
            // The route publishes only on commit, so the answer names the route
            // this request committed, not the transaction's live route.
            std::string committed;
            if (row) committed = "MLR1 MIG COMMITTED " + token.id + " "
                + owner_text(row->request.destination) + " " + std::to_string(row->token.generation);
            return migration_verdict(token, result, committed);
        }
        if (verb == "ABORT") {
            MigrationToken token;
            if (!migration_token(in, token) || (in >> extra)) return "MLR1 ERR INVALID";
            token.key = key;
            const auto tx = registry_.transaction(namespace_, key);
            if (!tx) return "MLR1 ERR EMPTY";
            // Abort only schedules the rollback; CLEANUP finishes it, so the
            // word tells the caller which of the two just happened.
            const auto result = tx->apply(token, MigrationEvent::Abort);
            if (result == MigrationResult::Applied) registry_.persist_transaction(namespace_, key, *tx);
            const char* word = tx->state() == MigrationState::Aborted
                ? "MLR1 MIG ABORTED " : "MLR1 MIG ABORTING ";
            return migration_verdict(token, result, word + token.id);
        }
        if (verb == "RETIRE") {
            MigrationToken token;
            MigrationOwner member;
            if (!migration_token(in, token) || !owner_fields(in, member) || (in >> extra))
                return "MLR1 ERR INVALID";
            token.key = key;
            const auto tx = registry_.transaction(namespace_, key);
            if (!tx) return "MLR1 ERR EMPTY";
            const auto result = tx->retire_member(token, member);
            if (result == MigrationResult::Applied) registry_.persist_transaction(namespace_, key, *tx);
            return migration_verdict(token, result, "MLR1 MIG ABORTED " + token.id);
        }
        if (verb == "TAKEOVER") {
            MigrationToken token;
            MigrationRequest request;
            if (!migration_token(in, token) || !owner_fields(in, request.expected.owner)
                    || !owner_fields(in, request.destination) || (in >> extra))
                return "MLR1 ERR INVALID";
            token.key = key;
            request.key = key;
            request.id = token.id;
            request.expected.generation = token.generation;
            const auto tx = registry_.transaction(namespace_, key);
            if (!tx) return "MLR1 ERR EMPTY";
            const auto result = tx->takeover(request);
            if (result != MigrationResult::Applied && result != MigrationResult::Duplicate)
                return "MLR1 ERR " + migration_reason(result);
            if (result == MigrationResult::Applied)
                registry_.persist_transaction(namespace_, key, *tx);
            const auto* row = tx->entry(token.id);
            std::ostringstream out;
            out << "MLR1 MIG TAKEOVER " << token.id << ' ' << (row ? row->token.generation : 0);
            return out.str();
        }
        if (verb == "STATUS") {
            if ((in >> extra)) return "MLR1 ERR INVALID";
            const auto tx = registry_.transaction(namespace_, key);
            if (!tx) return "MLR1 EMPTY";
            const auto snapshot = tx->snapshot();
            if (const auto* row = tx->entry(snapshot.current)) return migration_state_line(*row);
            const auto route = tx->route();
            return "MLR1 MIG STATE idle " + owner_text(route.owner) + " " + std::to_string(route.generation);
        }
        return "MLR1 ERR UNEXPECTED";
    }
    std::shared_ptr<Channel> channel_;
    asio::steady_timer timer_;
    Registry& registry_;
    const Config& config_;
    Diagnostics& diagnostics_;
    std::function<void(RegistrySession*)> on_close_;
    std::optional<Registry::Member> member_;
    unsigned generation_ = 0;
    bool service_ok_ = false, reading_ = false, closed_ = false;
};
// Member side of MLR1: one outbound control connection that registers the
// process and heartbeats its admission state. The registry informs placement,
// it does not own this member, so a missing coordinator only re-schedules the
// registration instead of failing the process.
class RegistryMember : public std::enable_shared_from_this<RegistryMember> {
public:
    RegistryMember(asio::io_context& io, const Config& config, std::function<std::string()> status)
      : io_(io), config_(config), status_(std::move(status)),
        endpoint_(parse_address(config.coordinator.address), config.coordinator.port),
        connect_(io), heartbeat_(io), timer_(io) {}
    void start() { channel_ = std::make_shared<TcpChannel>(tcp::socket(io_)); connect(); }
    void stop() {
        dead_ = true;
        connect_.cancel();
        heartbeat_.cancel();
        timer_.cancel();
        if (channel_) channel_->close();
    }
private:
    enum class Phase { connecting, identifying, registering, live };
    void pump() {
        if (dead_) return;
        if (!channel_) return;
        if (phase_ == Phase::connecting) return connect();
        if (reading_) return;
        reading_ = true;
        deadline(phase_ == Phase::live ? 60 : 10);
        channel_->read([self=shared_from_this()](Error ec, std::string payload) {
            self->reading_ = false;
            if (self->dead_) return;
            if (ec) return self->fail("registry connection");
            self->dispatch(std::move(payload));
        });
    }
    void connect() {
        channel_->socket().async_connect(endpoint_, [self=shared_from_this()](Error ec) {
            if (self->dead_) return;
            if (ec) return self->reschedule();
            // Identified members lead with MLS1; the token gates the registry.
            self->phase_ = self->config_.upstream_token.empty() ? Phase::registering : Phase::identifying;
            self->deadline(10);
            if (self->phase_ == Phase::identifying)
                self->write(service_hello(self->config_), [self](Error) { self->pump(); });
            else
                self->write(self->register_line(), [self](Error) { self->pump(); });
        });
    }
    std::string register_line() const {
        const std::string instance = !config_.service_name.empty() ? config_.service_name
            : "node-"+std::to_string(config_.listen.port);
        std::ostringstream out;
        out << "MLR1 REGISTER " << config_.role << ' ' << instance << ' '
            << registry_member_text(config_.namespace_) << ' ' << config_.scope << ' '
            << config_.listen.address << ':' << config_.listen.port << ' '
            << (config_.probe_enabled ? config_.probe_listen.address+':'
                +std::to_string(config_.probe_listen.port) : "0");
        return out.str();
    }
    void dispatch(std::string payload) {
        if (phase_ == Phase::identifying) {
            if (payload != "MLS1 OK") {
                std::cerr << "coordinator identity refused: " << payload << '\n';
                return reschedule();
            }
            phase_ = Phase::registering;
            deadline(10);
            return write(register_line(), [self=shared_from_this()](Error) { self->pump(); });
        }
        if (payload != "MLR1 OK") {
            std::cerr << "coordinator verdict: " << payload << '\n';
            return reschedule();
        }
        if (phase_ == Phase::registering) {
            phase_ = Phase::live;
            std::cout << "registered with coordinator " << config_.coordinator.address << ':'
                      << config_.coordinator.port << ' ' << config_.namespace_ << ' '
                      << config_.scope << std::endl;
            beat();
        }
        deadline(60);
        return pump();
    }
    void beat() {
        if (dead_) return;
        heartbeat_.expires_after(std::chrono::milliseconds(500));
        heartbeat_.async_wait([self=shared_from_this()](Error ec) {
            if (ec || self->dead_ || self->phase_ != Phase::live) return;
            self->write(self->status_(), [self](Error) { self->pump(); });
            self->beat();
        });
    }
    void write(std::string payload, Completion completion) {
        channel_->write(std::move(payload), [self=shared_from_this(), cb=std::move(completion)]
                (Error ec) mutable {
            cb(ec);
            self->pump();
        });
    }
    void deadline(int seconds) {
        const auto generation = ++generation_;
        timer_.expires_after(std::chrono::seconds(seconds));
        timer_.async_wait([self=shared_from_this(), generation](Error ec) {
            if (!ec && self->generation_ == generation) self->fail("registry idle");
        });
    }
    void fail(const char* why) {
        if (dead_) return;
        std::cerr << "coordinator link failed: " << why << '\n';
        reschedule();
    }
    void reschedule() {
        phase_ = Phase::connecting;
        timer_.cancel();
        heartbeat_.cancel();
        if (channel_) { channel_->close(); channel_.reset(); }
        connect_.expires_after(std::chrono::milliseconds(500));
        connect_.async_wait([self=shared_from_this()](Error ec) {
            if (ec || self->dead_ || self->channel_) return;
            self->channel_ = std::make_shared<TcpChannel>(tcp::socket(self->io_));
            self->pump();
        });
    }
    asio::io_context& io_;
    const Config& config_;
    std::function<std::string()> status_;
    tcp::endpoint endpoint_;
    asio::steady_timer connect_, heartbeat_, timer_;
    std::shared_ptr<TcpChannel> channel_;
    Phase phase_ = Phase::connecting;
    unsigned generation_ = 0;
    bool reading_ = false, dead_ = false;
};
// One-shot placement lookups: connect, optionally identify, ask MLR1 PLACE,
// answer once and close. The registry closes a PLACE connection itself, so
// every lookup owns a fresh connection and keeps no state between lookups.
class RegistryClient : public std::enable_shared_from_this<RegistryClient> {
public:
    using Answer = std::function<void(std::string address, std::string reason)>;
    static void lookup(asio::io_context& io, const Config& config,
                       std::string_view role, std::string_view namespace_, std::string_view scope,
                       Answer answer) {
        std::shared_ptr<RegistryClient> self(new RegistryClient(io, config));
        self->begin(std::string(role), std::string(namespace_),
                    !scope.empty() ? std::string(scope) : config.scope, std::move(answer));
    }
private:
    RegistryClient(asio::io_context& io, const Config& config)
      : io_(io), config_(config), endpoint_(parse_address(config.coordinator.address), config.coordinator.port),
        channel_(std::make_shared<TcpChannel>(tcp::socket(io))), timer_(io) {}
    void begin(std::string role, std::string namespace_, std::string scope, Answer answer) {
        question_ = "MLR1 PLACE " + role + ' '
            + (namespace_.empty() ? "-" : "NS " + namespace_) + ' ' + scope;
        answer_ = std::move(answer);
        deadline();
        channel_->socket().async_connect(endpoint_, [self=shared_from_this()](Error ec) {
            if (self->answered()) return;
            if (ec) return self->answer({}, "unreachable");
            // A tokened coordinator requires the identity first; a plain one
            // would refuse the handshake instead of the placement.
            if (self->config_.upstream_token.empty()) return self->ask();
            self->write(service_hello(self->config_));
        });
    }
    void ask() {
        write(question_);
    }
    void write(std::string payload) {
        channel_->write(std::move(payload), [self=shared_from_this()](Error ec) {
            if (self->answered()) return;
            if (ec) return self->answer({}, "unreachable");
            self->channel_->read([self](Error ec, std::string payload) {
                if (self->answered()) return;
                if (ec) return self->answer({}, "unreachable");
                self->verdict(std::move(payload));
            });
        });
    }
    void verdict(std::string payload) {
        std::istringstream in(payload);
        std::string magic, command, instance, address, extra;
        in >> magic >> command;
        if (magic == "MLS1") {
            if (command == "OK") return ask(); // identified: now ask the placement
            // "MLS1 ERR <reason>": TOKEN / INVALID / UNEXPECTED.
            std::string reason;
            if (command == "ERR") in >> reason;
            return answer({}, reason.empty() ? std::string("refused") : reason);
        }
        if (magic != "MLR1") return answer({}, "unexpected");
        if (command == "EMPTY") return answer({}, "EMPTY");
        // "MLR1 PLACE <instance> <listen> <probe> ready <active>/<capacity>"
        if (command != "PLACE" || !(in >> instance >> address)) return answer({}, "unexpected");
        answer(address, {});
    }
    void deadline() {
        timer_.expires_after(std::chrono::seconds(10));
        timer_.async_wait([self=shared_from_this()](Error ec) {
            if (!ec && !self->answered()) self->answer({}, "timeout");
        });
    }
    bool answered() {
        if (done_) return true;
        return false;
    }
    void answer(std::string address, std::string reason) {
        if (done_) return;
        done_ = true;
        timer_.cancel();
        channel_->close();
        answer_(std::move(address), std::move(reason));
    }
    asio::io_context& io_;
    const Config& config_;
    tcp::endpoint endpoint_;
    std::shared_ptr<TcpChannel> channel_;
    asio::steady_timer timer_;
    std::string question_;
    Answer answer_;
    bool done_ = false;
};

}
