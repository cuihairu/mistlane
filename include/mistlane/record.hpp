// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
namespace mistlane {
// MLD1 (cross return protocol): the data role's home-side record endpoint.
// A peer leg is one framed connection, normally identified by MLS1, that
// loads and submits character records many times over:
//   MLD1 LOAD <realm> <character>
//   MLD1 SUBMIT <realm> <character> gen <n> seq <m> [<payload>]
// Replies:
//   MLD1 EMPTY
//   MLD1 STATE gen <n> seq <m> [<payload>]
//   MLD1 RECEIVED gen <n> seq <m>
//   MLD1 ERR INVALID | STALE gen <n> | SEQ seq <n> | FULL
// RECEIVED is a receipt, never a durability claim: the persistence chain does
// not exist yet, so nothing here may read as "ACK durable" and a restart
// loses every record.
struct RecordRequest {
    enum class Type { Load, Submit };
    Type type = Type::Load;
    std::string realm, character;
    std::uint64_t generation = 0, sequence = 0;
    std::string payload;
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
    } else throw std::invalid_argument("INVALID");
    if (!record_identifier(r.realm) || !record_identifier(r.character))
        throw std::invalid_argument("INVALID");
    return r;
}
// Single event-loop thread: no locking. The store keeps the newest accepted
// change-set per (realm, character); merging change-sets into entity state is the
// deferred entity-contract work, not this endpoint's.
class RecordStore {
public:
    using RecordKey = std::pair<std::string, std::string>;
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
        if (it != records_.end() && request.generation < it->second.generation) {
            // The generation fence: a late writer cannot overwrite a newer
            // hosting assignment by arriving last.
            result = "stale";
            return "MLD1 ERR STALE gen " + std::to_string(it->second.generation);
        }
        if (it != records_.end() && request.generation == it->second.generation
                && request.sequence == it->second.sequence) {
            // At-least-once resends dedup to a second receipt; the stored
            // payload stays the first one.
            result = "duplicate";
            return "MLD1 RECEIVED gen " + std::to_string(it->second.generation)
                 + " seq " + std::to_string(it->second.sequence);
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
        auto& record = records_[key];
        record.generation = request.generation;
        record.sequence = request.sequence;
        record.payload = std::move(request.payload);
        result = "accepted";
        return "MLD1 RECEIVED gen " + std::to_string(record.generation)
             + " seq " + std::to_string(record.sequence);
    }
private:
    // Distinct realm/character keys only; a full store refuses new records
    // instead of growing without bound under a misbehaving peer.
    static constexpr std::size_t limit = 1000000;
    struct RecordState { std::uint64_t generation = 0, sequence = 0; std::string payload; };
    std::map<RecordKey, RecordState> records_;
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
    explicit ReturnSendQueue(std::size_t retry_limit) : retry_limit_(retry_limit) {}
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
        if (!pending_[key].emplace(change.sequence, std::move(change)).second)
            throw std::invalid_argument("duplicate sequence");
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
    bool ack_durable(std::string_view realm, std::string_view character, std::uint64_t sequence) {
        const auto key = std::pair{std::string(realm), std::string(character)};
        auto cursor = durable_cursor_.find(key);
        auto rows = pending_.find(key);
        if (cursor == durable_cursor_.end() || rows == pending_.end()
                || sequence != cursor->second + 1 || sequence > send_cursor_[key]) return false;
        if (!rows->second.contains(sequence)) return false;
        rows->second.erase(sequence);
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
private:
    using Key = std::pair<std::string, std::string>;
    std::size_t retry_limit_;
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
    bool write_allowed(std::uint64_t generation) const { return state_ == State::Active && generation == generation_; }
    bool timer_running() const { return timers_running_; }
    State state() const { return state_; }
private:
    State state_ = State::None; std::uint64_t generation_ = 0, next_ = 0;
    std::size_t in_flight_ = 0; bool timers_running_ = true;
};
}
