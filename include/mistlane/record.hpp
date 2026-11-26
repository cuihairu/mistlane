// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace mistlane {
// MLD1 (cross return protocol): the data role's home-side record endpoint.
// A peer leg is one framed connection, normally identified by MLS1, that
// loads and submits character records many times over:
//   MLD1 LOAD <realm> <character>
//   MLD1 SUBMIT <realm> <character> gen <n> seq <m> [<payload>]
//   MLD1 FENCE <realm> <character> gen <n>
//   MLD1 GRANT <realm> <character> [<instance>]
//   MLD1 OWNERSHIP <realm> <character>
// Replies:
//   MLD1 EMPTY
//   MLD1 STATE gen <n> seq <m> [<payload>]
//   MLD1 RECEIVED gen <n> seq <m>
//   MLD1 FENCED gen <n> seq <m>
//   MLD1 GRANTED gen <n>
//   MLD1 ACK durable <realm> <character> gen <n> seq <m>
//   MLD1 OWNER NONE
//   MLD1 OWNER AT <instance|-> gen <n> <ACTIVE|FENCED>
//   MLD1 ERR INVALID | STALE gen <n> | SEQ seq <n> | FULL | NOFENCE | STORAGE
// Without a durable backend RECEIVED is a receipt, never a durability claim.
// With one, acceptance is acknowledged with ACK durable only after the home
// write is durable, so a restart never loses an acknowledged change-set.
// GRANT is the data-issued hosting generation (the login entry flow): the
// caller names the participant and never a generation, because this endpoint
// is the sole authority that hands them out. The optional trailing instance
// is the target host the entry flow placed the participant on; when present,
// the lease records it, which is what makes OWNERSHIP able to name a live
// owner. OWNERSHIP is the read-only home-facts lookup (the apiServer MLA1
// OWNER callback): it never reserves a generation or changes a lease, and it
// answers what the data side honestly knows — the record/lease generation,
// the named lease owner while the lease is the live top, and FENCED while the
// top generation is frozen mid-lease. It never invents an owner: NONE means
// no record and no lease exist at all.
struct RecordRequest {
    enum class Type { Load, Submit, Fence, Grant, Ownership };
    Type type = Type::Load;
    std::string realm, character;
    std::uint64_t generation = 0, sequence = 0;
    std::string payload;
    // GRANT only: the target host instance the lease should record (empty =
    // an anonymous grant, the lease stays unnamed).
    std::string instance;
};
inline bool record_identifier(std::string_view s) {
    if (s.empty() || s.size() > 64) return false;
    for (unsigned char ch : s)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) return false;
    return true;
}
inline bool record_decimal(std::string_view s, std::uint64_t& out) {
    if (s.empty() || s.size() > 10) return false;
    std::uint64_t n = 0;
    for (unsigned char ch : s) {
        if (ch < '0' || ch > '9') return false;
        n = n*10 + ch-'0';
    }
    if (!n) return false;
    out = n;
    return true;
}
// Durable confirmation is a separate terminal reply. It is valid only after
// the home store atomically commits payload and sequence under this fence.
struct DurableAck { std::string realm, character; std::uint64_t generation = 0, sequence = 0; };
inline std::string encode_durable_ack(const DurableAck& ack) {
    if (!record_identifier(ack.realm) || !record_identifier(ack.character) || !ack.generation || !ack.sequence)
        throw std::invalid_argument("INVALID");
    return "MLD1 ACK durable " + ack.realm + " " + ack.character + " gen "
        + std::to_string(ack.generation) + " seq " + std::to_string(ack.sequence);
}
inline DurableAck parse_durable_ack(std::string_view frame) {
    std::istringstream in{std::string(frame)};
    std::string magic, ack, durable, gen_tag, gen, seq_tag, seq, extra;
    DurableAck out;
    if (!(in >> magic >> ack >> durable >> out.realm >> out.character >> gen_tag >> gen >> seq_tag >> seq)
        || (in >> extra) || magic != "MLD1" || ack != "ACK" || durable != "durable"
        || gen_tag != "gen" || seq_tag != "seq" || !record_identifier(out.realm)
        || !record_identifier(out.character) || !record_decimal(gen, out.generation)
        || !record_decimal(seq, out.sequence)) throw std::invalid_argument("INVALID");
    return out;
}
inline RecordRequest parse_record(std::string_view payload) {
    // The transport already refuses frames past max_frame; this only bounds
    // the header scan, so the change-set payload stays as large as a frame.
    if (payload.size() > 1024*1024) throw std::invalid_argument("INVALID");
    std::istringstream in{std::string(payload)};
    std::string magic, command;
    RecordRequest r;
    if (!(in >> magic >> command) || magic != "MLD1") throw std::invalid_argument("INVALID");
    if (command == "LOAD") {
        std::string extra;
        if (!(in >> r.realm >> r.character) || (in >> extra)) throw std::invalid_argument("INVALID");
    } else if (command == "SUBMIT") {
        std::string gen_tag, gen, seq_tag, seq;
        if (!(in >> r.realm >> r.character >> gen_tag >> gen >> seq_tag >> seq)
                || gen_tag != "gen" || seq_tag != "seq"
                || !record_decimal(gen, r.generation) || !record_decimal(seq, r.sequence))
            throw std::invalid_argument("INVALID");
        r.type = RecordRequest::Type::Submit;
        // Everything past one separating space is the opaque change-set; it
        // may hold spaces and may be absent entirely.
        if (const auto at = in.tellg(); at >= 0) {
            auto offset = static_cast<std::size_t>(at);
            if (offset < payload.size() && payload[offset] == ' ') ++offset;
            r.payload.assign(payload.data()+offset, payload.size()-offset);
        }
    } else if (command == "FENCE") {
        std::string gen_tag, gen;
        if (!(in >> r.realm >> r.character >> gen_tag >> gen) || gen_tag != "gen"
                || !record_decimal(gen, r.generation))
            throw std::invalid_argument("INVALID");
        r.type = RecordRequest::Type::Fence;
    } else if (command == "GRANT") {
        // The grant names the participant and never a generation, because a
        // generation the caller tried to choose or advance would defeat the
        // sole authority this endpoint is. The optional trailing word is the
        // target host instance the lease should record — an identity, nothing
        // else; anything past it is a malformed frame.
        std::string extra;
        if (!(in >> r.realm >> r.character)) throw std::invalid_argument("INVALID");
        if (in >> r.instance) {
            if (!record_identifier(r.instance)) throw std::invalid_argument("INVALID");
            if (in >> extra) throw std::invalid_argument("INVALID");
        }
        r.type = RecordRequest::Type::Grant;
    } else if (command == "OWNERSHIP") {
        std::string extra;
        if (!(in >> r.realm >> r.character) || (in >> extra)) throw std::invalid_argument("INVALID");
        r.type = RecordRequest::Type::Ownership;
    } else throw std::invalid_argument("INVALID");
    if (!record_identifier(r.realm) || !record_identifier(r.character))
        throw std::invalid_argument("INVALID");
    return r;
}
// One durable MLD1 record row: the canonical home-store snapshot after an
// accepted submission — fence generation, monotonic sequence and the opaque
// change-set payload.
struct RecordSnapshot {
    std::string realm, character;
    std::uint64_t generation = 0, sequence = 0;
    std::string payload;
};
// The home data side's final ordered cursor for one participant. A migration
// fence freezes the cursor exactly once per fenced generation; after that the
// old generation is a tombstone — a later source submission can never advance
// the cursor, and only a strictly newer generation (the next owner) may resume
// from the frozen cursor. This is the home-side counterpart of the coordinator
// generation authority: it is what makes the final sequence authoritative and
// durable across a data restart.
class HomeSequenceAuthority {
public:
    using Key = std::pair<std::string, std::string>;
    struct Pin {
        Key key;
        std::uint64_t generation = 0, sequence = 0;
        bool operator==(const Pin&) const = default;
    };
    // Restore a persisted pin. A pin is immutable once fenced, so re-seeding
    // the same key at the same generation is idempotent and a changed pin is
    // refused fail-closed rather than silently rewritten.
    bool restore(const Pin& pin) {
        if (!record_identifier(pin.key.first) || !record_identifier(pin.key.second)
                || !pin.generation || !pin.sequence) return false;
        const auto [it, inserted] = pins_.try_emplace(pin.key, pin);
        return inserted || it->second == pin;
    }
    std::optional<Pin> pin(const Key& key) const {
        const auto found = pins_.find(key);
        return found == pins_.end() ? std::nullopt : std::optional(found->second);
    }
    bool fenced(const Key& key) const {
        const auto found = pin(key);
        return found && found->generation;
    }
    // Freeze the final cursor for a fenced generation. Idempotent for the same
    // pin; a different generation or a double fence with a changed cursor is a
    // conflict and changes nothing.
    bool fence(const Key& key, std::uint64_t generation, std::uint64_t sequence) {
        if (!generation || !sequence) return false;
        const auto [it, inserted] = pins_.try_emplace(key, Pin{key, generation, sequence});
        return inserted ? true : it->second == Pin{key, generation, sequence};
    }
private:
    std::map<Key, Pin> pins_;
};
// The data role's sole generation authority for one participant. The hosting
// generation is allocated here, durably, and never chosen or advanced by a
// host: a fresh key starts at one and every later grant is strictly greater,
// so an aborted or timed-out allocation is never reused. The row is a
// high-water mark — it records the last generation the authority issued,
// which is what a load reports as the active grant. This is deliberately
// separate from the record's fence generation: the ordered lease handoff
// freezes the latter, while the authority answers "which generation may
// write next" without touching the cursor.
class GenerationAuthority {
public:
    using Key = std::pair<std::string, std::string>;
    struct Grant {
        Key key;
        std::uint64_t generation = 0;
        // The target host instance the grant was issued to (empty = unnamed).
        std::string owner;
        bool operator==(const Grant&) const = default;
    };
    // Restore a persisted grant. A grant is immutable once issued, so
    // re-seeding the same key at the same generation is idempotent and a
    // changed row is refused fail-closed rather than silently rewritten.
    bool restore(const Grant& grant) {
        if (!record_identifier(grant.key.first) || !record_identifier(grant.key.second)
                || !grant.generation
                || (!grant.owner.empty() && !record_identifier(grant.owner))) return false;
        const auto [it, inserted] = grants_.try_emplace(grant.key, grant);
        return inserted || it->second == grant;
    }
    std::optional<Grant> lease(const Key& key) const {
        const auto found = grants_.find(key);
        return found == grants_.end() ? std::nullopt : std::optional(found->second);
    }
    std::optional<std::uint64_t> current(const Key& key) const {
        const auto found = grants_.find(key);
        return found == grants_.end() ? std::nullopt : std::optional(found->second.generation);
    }
    // The generation a new grant would receive: strictly greater than the
    // last one issued for the key and at least one past `floor` (the record's
    // committed generation, when one already exists — a migration-era record
    // must never be overwritten by a grant below it). A fresh key starts at
    // one. nullopt when the counter has no representable next value to hand out.
    std::optional<std::uint64_t> next(const Key& key, std::uint64_t floor = 0) const {
        const auto last = current(key);
        const auto base = std::max(last.value_or(0), floor);
        const std::uint64_t candidate = base + 1;
        return candidate > base ? std::optional<std::uint64_t>(candidate) : std::nullopt;
    }
    // Record an allocated grant after the durable write succeeded. A later
    // allocation advances the high-water mark and takes the lease's owner
    // name; the same row is idempotent (a retry of a grant the store already
    // committed keeps the first lease's owner — one generation was issued to
    // one host, and a retry cannot rename it); a lower one is refused
    // fail-closed — the authority is a monotone cursor, never a suggestion.
    bool grant(const Grant& grant) {
        if (!grant.generation
                || (!grant.owner.empty() && !record_identifier(grant.owner))) return false;
        const auto [it, inserted] = grants_.try_emplace(grant.key, grant);
        if (inserted) return true;
        if (grant.generation == it->second.generation) return true;
        if (grant.generation < it->second.generation) return false;
        it->second = grant;
        return true;
    }
private:
    std::map<Key, Grant> grants_;
};
// Storage boundary for the data role's persistent record store. The SQLite
// backend lives beside the coordinator journal and reuses its versioned-BLOB
// encoding; an in-memory-only store passes no backend here.
class RecordBackend {
public:
    virtual ~RecordBackend() = default;
    virtual void save_record(const RecordSnapshot&) = 0;
    virtual std::vector<RecordSnapshot> restore_records() const = 0;
    // A durable final-sequence pin is part of the same store boundary. The
    // default no-ops keep in-memory test backends and non-fencing callers
    // working; a SQLite-backed store overrides them.
    virtual void save_fence(const HomeSequenceAuthority::Pin&) {}
    virtual std::vector<HomeSequenceAuthority::Pin> restore_fences() const { return {}; }
    // The durable generation authority is part of the same store boundary.
    virtual void save_grant(const GenerationAuthority::Grant&) {}
    virtual std::vector<GenerationAuthority::Grant> restore_grants() const { return {}; }
};
// Single event-loop thread: no locking. The store keeps the newest accepted
// change-set per (realm, character); merging change-sets into entity state is the
// deferred entity-contract work, not this endpoint's.
class RecordStore {
public:
    using RecordKey = std::pair<std::string, std::string>;
    RecordStore() = default;
    // Wires durable storage and restores every persisted row before the
    // endpoint accepts traffic; a corrupt or full store fails startup.
    void attach_backend(RecordBackend* backend) {
        if (!backend || backend_) return;
        for (auto snapshot : backend->restore_records()) {
            if (!record_identifier(snapshot.realm) || !record_identifier(snapshot.character)
                    || !snapshot.generation || !snapshot.sequence || records_.size() >= limit)
                throw std::invalid_argument("record store row corrupted");
            RecordState& record = records_[{std::move(snapshot.realm), std::move(snapshot.character)}];
            record.generation = snapshot.generation;
            record.sequence = snapshot.sequence;
            record.payload = std::move(snapshot.payload);
        }
        for (auto pin : backend->restore_fences()) {
            if (!sequence_authority_.restore(pin))
                throw std::invalid_argument("record store fence row corrupted");
        }
        for (auto grant : backend->restore_grants()) {
            if (!generation_authority_.restore(grant))
                throw std::invalid_argument("record store grant row corrupted");
        }
        backend_ = backend;
    }
    bool durable() const { return backend_ != nullptr; }
    // The authoritative final cursor for a key, if it has been fenced.
    std::optional<HomeSequenceAuthority::Pin> final_sequence(const RecordKey& key) const {
        return sequence_authority_.pin(key);
    }
    // The last hosting generation this authority issued for a key, if any.
    std::optional<std::uint64_t> granted(const RecordKey& key) const {
        return generation_authority_.current(key);
    }
    // Freeze the final ordered cursor for a fenced generation. Returns the pin,
    // or nullopt when there is no committed cursor yet, the store cannot make
    // the fence durable, or a different generation is already fenced. The
    // durable write precedes the in-memory pin so a failed commit never leaves
    // a fence the store did not record.
    std::optional<HomeSequenceAuthority::Pin> fence(const RecordKey& key, std::uint64_t generation) {
        const auto it = records_.find(key);
        if (it == records_.end() || !generation) return std::nullopt;
        const HomeSequenceAuthority::Pin pin{key, generation, it->second.sequence};
        if (const auto existing = sequence_authority_.pin(key)) {
            return *existing == pin ? existing : std::nullopt;
        }
        if (backend_) {
            try { backend_->save_fence(pin); }
            catch (const std::exception&) { return std::nullopt; }
        }
        if (!sequence_authority_.fence(key, generation, it->second.sequence)) return std::nullopt;
        return pin;
    }
    // Handles one frame and returns the reply; the result word feeds the
    // bounded result label of mistlane_record_results_total.
    std::string handle(std::string_view frame, std::string_view& result) {
        RecordRequest request;
        try { request = parse_record(frame); }
        catch (const std::exception&) { result = "invalid"; return "MLD1 ERR INVALID"; }
        const RecordKey key{request.realm, request.character};
        const auto it = records_.find(key);
        if (request.type == RecordRequest::Type::Load) {
            if (it == records_.end()) { result = "empty"; return "MLD1 EMPTY"; }
            result = "loaded";
            return "MLD1 STATE gen " + std::to_string(it->second.generation)
                 + " seq " + std::to_string(it->second.sequence)
                 + (it->second.payload.empty() ? "" : " " + it->second.payload);
        }
        // The data-issued hosting generation: allocate the strictly greater
        // next generation durably, then remember it in memory. The durable
        // write precedes the in-memory grant so a failed commit never leaves
        // a generation the store did not record — the caller may retry and
        // get the same (or a later) generation, never a gap it must skip.
        if (request.type == RecordRequest::Type::Grant) {
            const auto next = generation_authority_.next(key,
                it == records_.end() ? 0 : it->second.generation);
            if (!next) { result = "storage"; return "MLD1 ERR STORAGE"; }
            const GenerationAuthority::Grant grant{key, *next, request.instance};
            if (backend_) {
                try { backend_->save_grant(grant); }
                catch (const std::exception&) {
                    result = "storage";
                    return "MLD1 ERR STORAGE";
                }
            }
            if (!generation_authority_.grant(grant)) {
                result = "storage";
                return "MLD1 ERR STORAGE";
            }
            result = "granted";
            return "MLD1 GRANTED gen " + std::to_string(grant.generation);
        }
        // A migration fence freezes the final cursor for the requested
        // generation. It is idempotent for the same pin; a different
        // generation already fenced is stale, a key with no committed cursor
        // cannot be fenced, and a store failure changes nothing.
        if (request.type == RecordRequest::Type::Fence) {
            if (const auto existing = sequence_authority_.pin(key)) {
                if (existing->generation == request.generation) {
                    result = "fenced";
                    return "MLD1 FENCED gen " + std::to_string(existing->generation)
                         + " seq " + std::to_string(existing->sequence);
                }
                result = "stale";
                return "MLD1 ERR STALE gen " + std::to_string(existing->generation);
            }
            if (it == records_.end()) { result = "nofence"; return "MLD1 ERR NOFENCE"; }
            // A fence may only freeze the generation the store currently
            // holds. Fencing a different generation would pin the wrong
            // cursor and strand both the current and requested generation.
            if (request.generation != it->second.generation) {
                result = "stale";
                return "MLD1 ERR STALE gen " + std::to_string(it->second.generation);
            }
            const auto pin = fence(key, request.generation);
            if (!pin) { result = "storage"; return "MLD1 ERR STORAGE"; }
            result = "fenced";
            return "MLD1 FENCED gen " + std::to_string(pin->generation)
                 + " seq " + std::to_string(pin->sequence);
        }
        // The read-only home-facts lookup (the apiServer MLA1 OWNER callback's
        // source). It reads only what this store already holds — the record's
        // committed generation, the issued lease and the final-cursor pin —
        // and never reserves a generation or touches a lease. The generation
        // answered is the hosting top: the lease while one stands above the
        // record, else the record itself (the implicit lease of a migration-era
        // or never-granted record). The owner is named only while the named
        // lease IS that top: a resumed writer past a fence arrived at a
        // coordinator-issued generation this endpoint never named, and a
        // frozen top is a fenced gap with no owner at all. NONE means no
        // record and no lease exist — the one honest NotFound.
        if (request.type == RecordRequest::Type::Ownership) {
            const auto lease = generation_authority_.lease(key);
            const auto pin = sequence_authority_.pin(key);
            const std::uint64_t record_gen = it == records_.end() ? 0 : it->second.generation;
            const std::uint64_t lease_gen = lease ? lease->generation : 0;
            const std::uint64_t top = std::max(record_gen, lease_gen);
            if (!top) { result = "none"; return "MLD1 OWNER NONE"; }
            const bool fenced = pin && pin->generation == top;
            // The fence wins over a standing lease: the frozen gap names no
            // one, even when the source's lease still sits at the pinned top.
            const bool named = !fenced && lease_gen == top && !lease->owner.empty();
            result = fenced ? "fenced" : (named ? "owned" : "owned-anon");
            return "MLD1 OWNER AT " + (named ? lease->owner : std::string("-"))
                 + " gen " + std::to_string(top)
                 + (fenced ? " FENCED" : " ACTIVE");
        }
        // The durable-write lease: when home data has allocated a generation
        // for the key, a submission is authorized only at exactly that granted
        // generation. A writer below it is the superseded host (STALE, named at
        // the current lease); a writer above it is a host that names a
        // generation home data never handed out — the whole point of the sole
        // authority is that no host self-advances. A key home data never
        // granted keeps the record-relative fence below (migration-era records
        // carry their own generation as the implicit lease, and the coordinator
        // handoff authorizes the destination past a FENCE).
        if (const auto lease = generation_authority_.lease(key)) {
            if (request.generation < lease->generation) {
                result = "stale";
                return "MLD1 ERR STALE gen " + std::to_string(lease->generation);
            }
            if (request.generation > lease->generation) {
                result = "ungranted";
                return "MLD1 ERR UNGRANTED gen " + std::to_string(lease->generation);
            }
        }
        if (it != records_.end() && request.generation < it->second.generation) {
            // The generation fence: a late writer cannot overwrite a newer
            // hosting assignment by arriving last.
            result = "stale";
            return "MLD1 ERR STALE gen " + std::to_string(it->second.generation);
        }
        if (it != records_.end() && request.generation == it->second.generation
                && request.sequence == it->second.sequence) {
            // At-least-once resends dedup to a second receipt; the stored
            // payload stays the first one. A durable store echoes the already
            // committed ACK instead of a fresh volatile receipt.
            result = "duplicate";
            if (backend_) return encode_durable_ack({request.realm, request.character,
                it->second.generation, it->second.sequence});
            return "MLD1 RECEIVED gen " + std::to_string(it->second.generation)
                 + " seq " + std::to_string(it->second.sequence);
        }
        // A fenced generation is a tombstone: the source may only resend the
        // committed cursor (handled above) and can never advance it. Only a
        // strictly newer generation may resume from the frozen cursor.
        if (const auto pin = sequence_authority_.pin(key);
                pin && request.generation <= pin->generation) {
            result = "stale";
            return "MLD1 ERR STALE gen " + std::to_string(pin->generation);
        }
        // Sequence discipline per (realm, character), monotonic across
        // generations, with no gaps the home side can fill. A fresh key starts at one.
        const auto expected = it == records_.end() ? 1 : it->second.sequence + 1;
        if (request.sequence != expected) {
            result = "seq";
            return "MLD1 ERR SEQ seq " + std::to_string(expected);
        }
        if (records_.size() >= limit && it == records_.end()) {
            result = "full";
            return "MLD1 ERR FULL";
        }
        RecordState accepted;
        accepted.generation = request.generation;
        accepted.sequence = request.sequence;
        accepted.payload = request.payload;
        if (backend_) {
            try {
                backend_->save_record({request.realm, request.character, accepted.generation,
                    accepted.sequence, accepted.payload});
            } catch (const std::exception&) {
                // A failed durable write is not an acceptance: the sender stays
                // in its retry queue and the in-memory view is unchanged.
                result = "storage";
                return "MLD1 ERR STORAGE";
            }
        }
        auto& record = records_[key];
        record = std::move(accepted);
        result = "accepted";
        if (backend_) return encode_durable_ack({request.realm, request.character,
            record.generation, record.sequence});
        return "MLD1 RECEIVED gen " + std::to_string(record.generation)
             + " seq " + std::to_string(record.sequence);
    }
private:
    // Distinct realm/character keys only; a full store refuses new records
    // instead of growing without bound under a misbehaving peer.
    static constexpr std::size_t limit = 1000000;
    struct RecordState { std::uint64_t generation = 0, sequence = 0; std::string payload; };
    std::map<RecordKey, RecordState> records_;
    HomeSequenceAuthority sequence_authority_;
    GenerationAuthority generation_authority_;
    RecordBackend* backend_ = nullptr;
};

// Sender-side ordering boundary. The caller seeds the durable cursor from
// LOAD, then may enqueue changes in any arrival order. Only the next
// contiguous sequence for a (realm, character) can be sent, and only one
// frame per participant may be in flight. RECEIVED releases the transport
// slot but retains the item until a future durable acknowledgement exists.
class ReturnSendQueue {
public:
    struct Change {
        std::string realm, character;
        std::uint64_t generation = 0, sequence = 0;
        std::string payload;
    };
    // retry_limit bounds per-item attempts; capacity bounds total queued
    // changes across all participants. A full queue refuses enqueue instead of
    // growing without bound, matching the "bounded queue" help text.
    explicit ReturnSendQueue(std::size_t retry_limit, std::size_t capacity = 1024)
      : retry_limit_(retry_limit), capacity_(capacity) {}
    void seed(std::string realm, std::string character, std::uint64_t stored_sequence) {
        const auto key = std::pair{std::move(realm), std::move(character)};
        if (send_cursor_.contains(key)) throw std::invalid_argument("participant already seeded");
        send_cursor_[key] = stored_sequence;
        durable_cursor_[key] = stored_sequence;
    }
    void enqueue(Change change) {
        const auto key = std::pair{change.realm, change.character};
        if (!send_cursor_.contains(key) || !change.sequence || !change.generation)
            throw std::invalid_argument("unseeded participant or invalid sequence");
        if (pending_count_ >= capacity_) throw std::invalid_argument("queue is full");
        if (!pending_[key].emplace(change.sequence, std::move(change)).second)
            throw std::invalid_argument("duplicate sequence");
        ++pending_count_;
    }
    // Total changes waiting to be sent (before any durable ACK). This is the
    // occupancy the sender's pre-load holding area must count against, so a
    // pending pool cannot overflow the bounded queue when it is drained.
    std::size_t pending() const { return pending_count_; }
    // Whether a change with this sequence is already held (pending or in
    // flight). Used by the sender's pre-load pool to reject a duplicate before
    // it is moved into the queue, so a drain never fails halfway.
    bool contains(std::string_view realm, std::string_view character,
                  std::uint64_t sequence) const {
        const auto key = std::pair{std::string(realm), std::string(character)};
        const auto rows = pending_.find(key);
        return rows != pending_.end() && rows->second.contains(sequence);
    }
    std::optional<Change> next(std::string_view realm, std::string_view character) {
        const auto key = std::pair{std::string(realm), std::string(character)};
        if (in_flight_.contains(key)) return std::nullopt;
        const auto seq = send_cursor_.find(key);
        const auto rows = pending_.find(key);
        if (seq == send_cursor_.end() || rows == pending_.end()) return std::nullopt;
        const auto item = rows->second.find(seq->second + 1);
        if (item == rows->second.end() || attempts_[key][item->first] >= retry_limit_)
            return std::nullopt;
        ++attempts_[key][item->first];
        in_flight_[key] = item->first;
        return item->second;
    }
    bool retry(std::string_view realm, std::string_view character) {
        const auto key = std::pair{std::string(realm), std::string(character)};
        const auto flight = in_flight_.find(key);
        if (flight == in_flight_.end()) return false;
        const auto seq = flight->second;
        in_flight_.erase(flight);
        return attempts_[key][seq] < retry_limit_;
    }
    bool received(std::string_view realm, std::string_view character, std::uint64_t sequence) {
        const auto key = std::pair{std::string(realm), std::string(character)};
        const auto flight = in_flight_.find(key);
        if (flight == in_flight_.end() || flight->second != sequence) return false;
        in_flight_.erase(flight);
        send_cursor_[key] = sequence;
        return true;
    }
    // The change currently in flight for a participant, if any.
    const Change* peek(std::string_view realm, std::string_view character) const {
        const auto key = std::pair{std::string(realm), std::string(character)};
        const auto flight = in_flight_.find(key);
        if (flight == in_flight_.end()) return nullptr;
        const auto rows = pending_.find(key);
        if (rows == pending_.end()) return nullptr;
        const auto item = rows->second.find(flight->second);
        return item == rows->second.end() ? nullptr : &item->second;
    }
    bool ack_durable(std::string_view realm, std::string_view character, std::uint64_t sequence) {
        const auto key = std::pair{std::string(realm), std::string(character)};
        auto cursor = durable_cursor_.find(key);
        auto rows = pending_.find(key);
        if (cursor == durable_cursor_.end() || rows == pending_.end()
                || sequence != cursor->second + 1 || sequence > send_cursor_[key]) return false;
        if (!rows->second.contains(sequence)) return false;
        rows->second.erase(sequence);
        --pending_count_;
        cursor->second = sequence;
        return true;
    }
    bool ack_durable(const DurableAck& ack) {
        const auto key = std::pair{ack.realm, ack.character};
        const auto current = owner_generation_.find(key);
        if (current == owner_generation_.end() || current->second != ack.generation) return false;
        return ack_durable(ack.realm, ack.character, ack.sequence);
    }
    void owner_generation(std::string realm, std::string character, std::uint64_t generation) {
        owner_generation_[{std::move(realm), std::move(character)}] = generation;
    }
    // The next sequence a caller may assign for this participant: one past the
    // send cursor and every pending (or in-flight) change, so a framework
    // change enqueued here lands after everything the caller already queued.
    // Zero when the participant is unseeded — there is no cursor to be past.
    std::uint64_t next_sequence(std::string_view realm, std::string_view character) const {
        const auto key = std::pair{std::string(realm), std::string(character)};
        const auto cursor = send_cursor_.find(key);
        if (cursor == send_cursor_.end()) return 0;
        std::uint64_t highest = cursor->second;
        const auto rows = pending_.find(key);
        if (rows != pending_.end() && !rows->second.empty())
            highest = std::max(highest, rows->second.rbegin()->first);
        return highest + 1;
    }
    // Whether this participant holds no pending change. The sender's in-flight
    // set and pre-load pool are the sender's, so drained() on the queue alone
    // is only the pending map — the ReturnSender composes all three.
    bool drained(std::string_view realm, std::string_view character) const {
        const auto key = std::pair{std::string(realm), std::string(character)};
        const auto rows = pending_.find(key);
        return rows == pending_.end() || rows->second.empty();
    }
private:
    using Key = std::pair<std::string, std::string>;
    std::size_t retry_limit_;
    std::size_t capacity_;
    std::size_t pending_count_ = 0;
    std::map<Key, std::uint64_t> send_cursor_, durable_cursor_;
    std::map<Key, std::map<std::uint64_t, Change>> pending_;
    std::map<Key, std::map<std::uint64_t, std::size_t>> attempts_;
    std::map<Key, std::uint64_t> in_flight_;
    std::map<Key, std::uint64_t> owner_generation_;
};

// Deterministic lease protocol model. The source fences itself before the
// destination can become writable; in-flight work is drained and timers stop.
class WriterLease {
public:
    enum class State { None, Active, Requested, Relinquished };
    // Storage boundary view: the journal encoded this lease in the same
    // snapshot as the transaction, so restore must not invent lease facts.
    struct View {
        State state = State::None;
        std::uint64_t generation = 0, next = 0;
        std::size_t in_flight = 0;
        bool timers_running = true;
    };
    bool start(std::uint64_t generation) { if (state_ != State::None || !generation) return false; generation_ = generation; state_ = State::Active; return true; }
    bool begin_request() { if (!write_allowed(generation_)) return false; ++in_flight_; return true; }
    bool finish_request() { if (state_ != State::Requested || !in_flight_) return false; --in_flight_; return true; }
    // A retired member cannot finish its own in-flight handlers, so the
    // coordinator force-completes them instead of waiting on a dead process.
    bool drain_requests() { if (state_ != State::Requested) return false; in_flight_ = 0; return true; }
    bool request(std::uint64_t next) { if (state_ != State::Active || next <= generation_) return false; next_ = next; state_ = State::Requested; timers_running_ = false; return true; }
    bool drained() const { return state_ == State::Requested && !in_flight_; }
    // Only a pre-tombstone cancellation may restore the source generation.
    bool cancel_request() { if (state_ != State::Requested) return false; next_ = 0; state_ = State::Active; timers_running_ = true; return true; }
    bool relinquish() { if (state_ != State::Requested || in_flight_) return false; state_ = State::Relinquished; return true; }
    bool accept(std::uint64_t generation) { if (state_ != State::Relinquished || generation != next_) return false; generation_ = generation; state_ = State::Active; timers_running_ = true; return true; }
    // Continue a relinquished grant onto a strictly newer generation without
    // reviving the old one: the lease stays relinquished (no writer at all)
    // and simply re-targets `accept`, so the old generation is never drained or
    // cancelled after the fence and the new destination activates only when it
    // ACKs. The tombstone forbids requesting or revoking a burned generation.
    bool takeover(std::uint64_t generation) { if (state_ != State::Relinquished || generation <= generation_) return false; next_ = generation; return true; }
    bool write_allowed(std::uint64_t generation) const { return state_ == State::Active && generation == generation_; }
    bool timer_running() const { return timers_running_; }
    State state() const { return state_; }
    View view() const { return {state_, generation_, next_, in_flight_, timers_running_}; }
    void restore(const View& view) {
        state_ = view.state; generation_ = view.generation; next_ = view.next;
        in_flight_ = view.in_flight; timers_running_ = view.timers_running;
    }
private:
    State state_ = State::None; std::uint64_t generation_ = 0, next_ = 0;
    std::size_t in_flight_ = 0; bool timers_running_ = true;
};
}
