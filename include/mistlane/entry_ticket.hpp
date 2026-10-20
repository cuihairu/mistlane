// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "mistlane/api.hpp"
#include <algorithm>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace mistlane {

// MLT1 is the decided short-lived entry ticket: after its own credential
// check, login issues one ticket over (realm, character, target instance,
// hosting generation, expiry, nonce) keyed by a per-realm pre-shared secret,
// and the receiving gameServer/crossServer verifies it before accepting an
// entry or a transfer — a host never loads a character on a bare claim.
// The wire form here is hex (canonical bytes + raw signature); the MLC1 join
// carries it as the "tk" tail group of the participant context. Durable
// seen-claim state and per-realm secret configuration remain undecided, and
// no real secret exists here.
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
// The wire form: the canonical bytes followed by the raw signature, hex.
inline std::string entry_ticket_hex(const EntryTicket& ticket) {
    std::string raw = entry_ticket_canonical(ticket);
    raw.append(reinterpret_cast<const char*>(ticket.signature.data()), ticket.signature.size());
    std::string hex;
    hex.reserve(raw.size()*2);
    for (unsigned char byte : raw) {
        static const char digits[] = "0123456789abcdef";
        hex += digits[byte/16];
        hex += digits[byte%16];
    }
    return hex;
}
// Decodes that form back. Anything off — odd length, a non-hex digit, a lying
// field length, a truncated or extended blob, a shape-invalid ticket — is a
// null optional, never an exception into the handshake.
inline std::optional<EntryTicket> parse_entry_ticket(std::string_view hex) {
    // The 1024-char cap is well past a ticket-shaped token and bounds the
    // work one join can ask for.
    if (hex.empty() || hex.size() > 1024 || hex.size() % 2) return std::nullopt;
    const auto digit = [](char c) {
        if (c >= '0' && c <= '9') return c-'0';
        if (c >= 'a' && c <= 'f') return c-'a'+10;
        return -1;
    };
    std::string raw((hex.size()/2), '\0');
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const auto hi = digit(hex[2*i]);
        const auto lo = digit(hex[2*i+1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        raw[i] = static_cast<char>(hi*16 + lo);
    }
    std::string_view bytes(raw);
    if (bytes.size() < 4+4+1 || bytes.substr(0, 4) != "MLT1") return std::nullopt;
    bytes.remove_prefix(4);
    const auto take = [&bytes](std::string& out) {
        if (bytes.size() < 4) return false;
        std::uint32_t size = 0;
        for (int i = 0; i < 4; ++i) size = (size << 8) | static_cast<unsigned char>(bytes[i]);
        bytes.remove_prefix(4);
        if (size > bytes.size()) return false;
        out.assign(bytes.substr(0, size));
        bytes.remove_prefix(size);
        return true;
    };
    EntryTicket ticket;
    std::string generation, expiry;
    if (!take(ticket.character.first) || !take(ticket.character.second) || !take(ticket.instance)
            || !take(generation) || !take(expiry) || !take(ticket.nonce)
            || bytes.size() != sizeof(ApiSignature)) return std::nullopt;
    const auto number = [&](const std::string& text, std::uint64_t& out) {
        if (text.empty()) return false;
        for (unsigned char ch : text)
            if (ch < '0' || ch > '9') return false;
        out = 0;
        for (unsigned char ch : text)
            if (out > (std::numeric_limits<std::uint64_t>::max() - (ch-'0'))/10) return false;
        for (unsigned char ch : text) out = out*10 + (ch-'0');
        return true;
    };
    if (!number(generation, ticket.generation) || !number(expiry, ticket.expires)
            || !valid_entry_ticket(ticket)) return std::nullopt;
    std::copy_n(bytes.begin(), sizeof(ApiSignature), ticket.signature.begin());
    return ticket;
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
