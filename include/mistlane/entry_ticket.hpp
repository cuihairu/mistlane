// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "mistlane/api.hpp"
#include <algorithm>
#include <map>
#include <mutex>
#include <stdexcept>

namespace mistlane {

// MLT1 is the decided short-lived entry ticket: after its own credential
// check, login issues one ticket over (realm, character, target instance,
// hosting generation, expiry, nonce) keyed by a per-realm pre-shared secret,
// and the receiving gameServer/crossServer verifies it before accepting an
// entry or a transfer — a host never loads a character on a bare claim.
// This is the typed model only: transport binding (MLC1 tail or otherwise)
// and durable seen-claim state are undecided, and no real secret exists here.
struct EntryTicket {
    RecordStore::RecordKey character; // (realm, character); the realm keys the secret
    std::string instance;          // the one instance this ticket admits
    std::uint64_t generation = 0;  // hosting generation at issue time
    std::uint64_t expires = 0;     // absolute unix seconds, valid through this value
    std::string nonce;             // exactly 32 lowercase hex digits, issuer CSPRNG
    ApiSignature signature{};
    bool operator==(const EntryTicket&) const = default;
};

bool valid_entry_ticket(const EntryTicket& ticket);
// magic "MLT1", then u32 BE byte length + exact bytes per field, in field
// order; the numbers ride as decimal text like the MLA1 request.
std::string entry_ticket_canonical(const EntryTicket& ticket);
ApiSignature entry_ticket_sign(const EntryTicket& ticket, const ApiSecret& secret);

enum class TicketVerdict { Ok, Malformed, BadSignature, Expired, WrongInstance, WrongCharacter, WrongGeneration, Replay, Full, UnknownRealm, Clock };

// Seen nonces of one receiving process. Volatile by design: a multi-process
// or restarted deployment needs shared durable claims before Replay is honest.
class TicketClaims {
public:
    explicit TicketClaims(std::size_t capacity = 4096) : capacity_(capacity) {}
private:
    friend class TicketVerifier;
    TicketVerdict claim(const EntryTicket& ticket, std::uint64_t now);
    std::mutex mutex_;
    std::map<std::string, std::uint64_t> expires_;  // nonce -> kept through this second
    std::uint64_t last_now_ = 0;
    std::size_t capacity_;
};

class TicketVerifier {
public:
    explicit TicketVerifier(std::map<std::string, ApiSecret> secrets, std::string instance, TicketClaims& claims)
      : secrets_(std::move(secrets)), instance_(std::move(instance)), claims_(claims) {
        if (secrets_.empty() || !record_identifier(instance_)
                || !std::all_of(secrets_.begin(), secrets_.end(), [](const auto& realm) {
                    return record_identifier(realm.first); }))
            throw std::invalid_argument("invalid ticket verifier");
    }
    TicketVerdict verify(const EntryTicket& ticket, const RecordStore::RecordKey& arrival, std::uint64_t generation, std::uint64_t now) const;
private:
    std::map<std::string, ApiSecret> secrets_;
    std::string instance_;
    TicketClaims& claims_;
};

inline bool valid_entry_ticket(const EntryTicket& ticket) {
    return record_identifier(ticket.character.first) && record_identifier(ticket.character.second)
        && record_identifier(ticket.instance) && ticket.generation
        && ticket.expires && ticket.nonce.size() == 32
        && std::all_of(ticket.nonce.begin(), ticket.nonce.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
}

inline std::string entry_ticket_canonical(const EntryTicket& ticket) {
    if (!valid_entry_ticket(ticket)) throw std::invalid_argument("invalid entry ticket");
    const auto field = [](std::string& bytes, const std::string& value) {
        const auto size = static_cast<std::uint32_t>(value.size());
        for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<char>((size >> shift) & 255));
        bytes += value;
    };
    std::string bytes = "MLT1";
    for (const auto& value : {ticket.character.first, ticket.character.second, ticket.instance,
            std::to_string(ticket.generation), std::to_string(ticket.expires), ticket.nonce}) field(bytes, value);
    return bytes;
}

inline ApiSignature entry_ticket_sign(const EntryTicket& ticket, const ApiSecret& secret) {
    return hmac_sha256(secret, entry_ticket_canonical(ticket));
}

inline TicketVerdict TicketClaims::claim(const EntryTicket& ticket, std::uint64_t now) {
    std::lock_guard lock(mutex_);
    if (!now || now < last_now_) return TicketVerdict::Clock; // Fail closed on wall-clock rollback.
    last_now_ = now;
    // Keep through expiry INCLUSIVELY, matching the validity rule.
    std::erase_if(expires_, [now](const auto& row) { return row.second < now; });
    if (expires_.contains(ticket.nonce)) return TicketVerdict::Replay;
    if (expires_.size() >= capacity_) return TicketVerdict::Full;
    expires_.emplace(ticket.nonce, ticket.expires);
    return TicketVerdict::Ok;
}

inline TicketVerdict TicketVerifier::verify(const EntryTicket& ticket, const RecordStore::RecordKey& arrival,
        std::uint64_t generation, std::uint64_t now) const {
    if (!valid_entry_ticket(ticket) || !record_identifier(arrival.first) || !record_identifier(arrival.second)
            || !now) return TicketVerdict::Malformed;
    const auto secret = secrets_.find(ticket.character.first);
    if (secret == secrets_.end()) return TicketVerdict::UnknownRealm;
    if (entry_ticket_sign(ticket, secret->second) != ticket.signature) return TicketVerdict::BadSignature;
    if (now > ticket.expires) return TicketVerdict::Expired;
    if (ticket.character != arrival) return TicketVerdict::WrongCharacter;
    if (ticket.instance != instance_) return TicketVerdict::WrongInstance;
    if (ticket.generation != generation) return TicketVerdict::WrongGeneration;
    return claims_.claim(ticket, now);
}
}
