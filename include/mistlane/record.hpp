// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <map>
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
// change-set per character; merging change-sets into entity state is the
// deferred entity-contract work, not this endpoint's.
class RecordStore {
public:
    // Handles one frame and returns the reply; the result word feeds the
    // bounded result label of mistlane_record_results_total.
    std::string handle(std::string_view frame, std::string_view& result) {
        RecordRequest request;
        try { request = parse_record(frame); }
        catch (const std::exception&) { result = "invalid"; return "MLD1 ERR INVALID"; }
        const auto key = std::pair{request.realm, request.character};
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
        // Sequence discipline: per-character monotonic across generations, no
        // gaps the home side cannot fill. A fresh record starts at one.
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
    std::map<std::pair<std::string, std::string>, RecordState> records_;
};
}
