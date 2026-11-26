// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "mistlane/entity_state.hpp"
#include "mistlane/transport.hpp"
#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>
namespace mistlane {
// MLD2 (entity-state endpoint): the data role's durable store for entity
// snapshots — the gameServer→data half of the persistence chain. MLD1 is
// sequenced per (realm, character); entity state is keyed by the entity
// address (scope, type, incarnation, entity) and fenced by the writer's
// hosting generation plus the committed revision the snapshot captured, so
// it gets its own marker rather than overloading the record endpoint:
//   MLD2 SAVE <scope> <type> <incarnation> <entity> gen <n> rev <m> <hex>
//   MLD2 LOAD <scope> <type> <incarnation> <entity>
// Replies:
//   MLD2 ACK <scope> <type> <incarnation> <entity> gen <n> rev <m>
//   MLD2 RECEIVED <scope> <type> <incarnation> <entity> gen <n> rev <m>
//   MLD2 STATE <scope> <type> <incarnation> <entity> gen <n> rev <m> <hex>
//   MLD2 EMPTY
//   MLD2 ERR INVALID | STALE gen <n> | CONFLICT rev <m> | STORAGE
// The hex payload is the lowercase encoding of the encoded persistent
// snapshot (opaque bytes; the space-free form keeps the header scannable).
// Without a durable backend RECEIVED is a receipt, never a durability
// claim, exactly like MLD1 without --journal. With one, ACK follows the
// committed row, so a restart never loses an acknowledged snapshot.
// The fence: a lower generation is the superseded writer (STALE), a lower
// revision under the same generation lost a race (CONFLICT), and an equal
// generation+revision is an idempotent retry of a save whose reply may
// have been lost — the row is rewritten and acknowledged again. Rows are
// seeded from the backend at construction, so a restarted data node
// resumes the fence from what it durably holds.
struct EntityStateKey {
    std::uint32_t scope = 0, type_id = 0, incarnation = 0;
    std::int64_t entity_id = 0;
    auto tie() const { return std::tie(scope, type_id, incarnation, entity_id); }
    bool operator<(const EntityStateKey& o) const { return tie() < o.tie(); }
    bool operator==(const EntityStateKey& o) const { return tie() == o.tie(); }
};
inline bool state_decimal(std::string_view s, std::uint64_t& out, bool allow_zero = false) {
    // Strict unsigned decimal, width-bounded. Unlike record_decimal this
    // accepts zero: revision 0 is the legitimate initial committed revision
    // (generation is checked separately, where zero is never valid).
    if (s.empty() || s.size() > 20) return false;
    std::uint64_t n = 0;
    for (unsigned char ch : s) {
        if (ch < '0' || ch > '9') return false;
        const auto digit = static_cast<std::uint64_t>(ch-'0');
        if (n > (~0ull-digit)/10) return false;
        n = n*10 + digit;
    }
    if (!allow_zero && !n) return false;
    out = n;
    return true;
}
inline bool state_int64(std::string_view s, std::int64_t& out) {
    std::uint64_t n = 0;
    bool negative = false;
    if (!s.empty() && s.front() == '-') { negative = true; s.remove_prefix(1); }
    if (!state_decimal(s, n, true)) return false;
    if (negative) {
        if (n > 9'223'372'036'854'775'808ull) return false;
        out = n == 9'223'372'036'854'775'808ull ? (~0ll) : -static_cast<std::int64_t>(n);
    } else {
        if (n > 9'223'372'036'854'775'807ull) return false;
        out = static_cast<std::int64_t>(n);
    }
    return true;
}
// The snapshot bytes ride the frame as lowercase hex (2 chars per byte),
// so the header stays whitespace-scannable and the payload opaque. The
// frame cap is max_frame; the raw half is this bound.
inline constexpr std::size_t max_state_bytes = max_frame/2;
inline void state_hex_encode(const std::string& bytes, std::string& out) {
    static const char* digits = "0123456789abcdef";
    out.clear();
    out.reserve(bytes.size()*2);
    for (unsigned char ch : bytes) {
        out.push_back(digits[ch >> 4]);
        out.push_back(digits[ch & 0xf]);
    }
}
inline bool state_hex_decode(std::string_view hex, std::string& out) {
    if (hex.size() % 2 || hex.size()/2 > max_state_bytes) return false;
    out.clear();
    out.reserve(hex.size()/2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const auto hi = hex[i], lo = hex[i+1];
        const auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c-'0';
            if (c >= 'a' && c <= 'f') return c-'a'+10;
            return -1;  // uppercase is refused: the wire form is lowercase
        };
        const int h = nibble(hi), l = nibble(lo);
        if (h < 0 || l < 0) return false;
        out.push_back(static_cast<char>((h << 4) | l));
    }
    return true;
}
struct EntityStateRequest {
    enum class Type { Save, Load };
    Type type = Type::Load;
    EntityStateKey key;
    std::uint64_t generation = 0, revision = 0;
    std::string snapshot;
};
inline EntityStateRequest parse_entity_state(std::string_view payload) {
    // The transport already refuses frames past max_frame; this bounds the
    // header scan before the hex decode.
    if (payload.size() > max_frame) throw std::invalid_argument("INVALID");
    std::istringstream in{std::string(payload)};
    std::string magic, command;
    EntityStateRequest r;
    if (!(in >> magic >> command) || magic != "MLD2") throw std::invalid_argument("INVALID");
    const auto read_key = [&](EntityStateKey& key) {
        std::string scope, type, incarnation, entity;
        if (!(in >> scope >> type >> incarnation >> entity)) return false;
        std::uint64_t wide = 0;
        if (!state_decimal(scope, wide) || wide > ~0ull>>32) return false;
        key.scope = static_cast<std::uint32_t>(wide);
        if (!state_decimal(type, wide) || wide > ~0ull>>32) return false;
        key.type_id = static_cast<std::uint32_t>(wide);
        if (!state_decimal(incarnation, wide) || wide > ~0ull>>32) return false;
        key.incarnation = static_cast<std::uint32_t>(wide);
        return state_int64(entity, key.entity_id) && key.entity_id > 0
            && key.scope && key.type_id && key.incarnation;
    };
    if (command == "SAVE") {
        std::string gen_tag, gen, rev_tag, rev, hex;
        if (!read_key(r.key)) throw std::invalid_argument("INVALID");
        if (!(in >> gen_tag >> gen >> rev_tag >> rev) || gen_tag != "gen" || rev_tag != "rev"
                || !state_decimal(gen, r.generation) || !state_decimal(rev, r.revision, true))
            throw std::invalid_argument("INVALID");
        if (!(in >> hex) || hex.empty()) throw std::invalid_argument("INVALID");
        if (!state_hex_decode(hex, r.snapshot)) throw std::invalid_argument("INVALID");
        r.type = EntityStateRequest::Type::Save;
        return r;
    }
    if (command == "LOAD") {
        if (!read_key(r.key)) throw std::invalid_argument("INVALID");
        return r;
    }
    throw std::invalid_argument("INVALID");
}
// The data-side service over one EntityStateBackend. Backend-less it keeps
// rows in memory and answers RECEIVED (an honest non-durability receipt);
// with a backend the durable write precedes the ACK and a failed commit is
// STORAGE — never an acknowledged-but-lost snapshot.
class EntityStateService {
public:
    explicit EntityStateService(EntityStateBackend* backend = nullptr) : backend_(backend) {
        if (!backend_) return;
        // Seed the fence from what the store durably holds, so a restarted
        // data node keeps refusing superseded writers instead of re-ackowing
        // a generation it already outlived.
        for (const auto& row : backend_->restore_states()) {
            EntityStateKey key{row.scope, row.type_id, row.incarnation, row.entity_id};
            rows_.emplace(key, Fence{row.generation, row.committed_revision});
        }
    }
    // Handles one frame and returns the reply; the result word feeds the
    // bounded result label, mirroring RecordStore::handle.
    std::string handle(std::string_view frame, std::string_view& result) {
        EntityStateRequest request;
        try { request = parse_entity_state(frame); }
        catch (const std::exception&) { result = "invalid"; return "MLD2 ERR INVALID"; }
        const auto key = request.key;
        const auto render = [key](const char* verb, std::uint64_t gen, std::uint64_t rev) {
            return std::string("MLD2 ") + verb + " " + std::to_string(key.scope) + " "
                + std::to_string(key.type_id) + " " + std::to_string(key.incarnation) + " "
                + std::to_string(key.entity_id) + " gen " + std::to_string(gen)
                + " rev " + std::to_string(rev);
        };
        const auto it = rows_.find(key);
        const Fence held = it == rows_.end() ? Fence{0, 0} : it->second;
        if (request.type == EntityStateRequest::Type::Load) {
            if (it == rows_.end()) { result = "empty"; return "MLD2 EMPTY"; }
            // The snapshot bytes live on the backend rows; the in-memory
            // fence is the index. Scan is O(rows) — acceptable for a
            // load-on-spawn leg; a keyed backend index is a later slice if
            // a host ever hammers it.
            if (backend_) {
                for (const auto& row : backend_->restore_states()) {
                    if (row.scope == key.scope && row.type_id == key.type_id
                            && row.incarnation == key.incarnation && row.entity_id == key.entity_id) {
                        std::string hex;
                        state_hex_encode(row.snapshot, hex);
                        result = "loaded";
                        return render("STATE", row.generation, row.committed_revision) + " " + hex;
                    }
                }
                result = "empty";
                return "MLD2 EMPTY";
            }
            // No backend: the in-memory row is all there is (and its bytes
            // were kept on the last save).
            std::string hex;
            state_hex_encode(last_bytes_.count(key) ? last_bytes_.at(key) : std::string{}, hex);
            result = "loaded";
            return render("STATE", held.generation, held.revision) + " " + hex;
        }
        // Save. The fence precedes the write: a superseded writer never
        // reaches storage, and an equal generation+revision is the retry
        // path (rewrite + re-acknowledge).
        if (request.generation < held.generation) {
            result = "stale";
            return "MLD2 ERR STALE gen " + std::to_string(held.generation);
        }
        if (request.generation == held.generation && request.revision < held.revision) {
            result = "conflict";
            return "MLD2 ERR CONFLICT rev " + std::to_string(held.revision);
        }
        if (backend_) {
            try {
                backend_->save_state({key.scope, key.type_id, key.incarnation, key.entity_id,
                                      request.generation, request.revision, request.snapshot});
            } catch (const std::exception&) {
                result = "storage";
                return "MLD2 ERR STORAGE";
            }
        }
        rows_[key] = Fence{request.generation, request.revision};
        // Without a backend the bytes live here, so a LOAD still answers
        // (in-memory only, no durability claim — the verdict on SAVE said so).
        last_bytes_[key] = request.snapshot;
        result = backend_ ? "saved" : "received";
        return render(backend_ ? "ACK" : "RECEIVED", request.generation, request.revision);
    }
private:
    struct Fence { std::uint64_t generation = 0, revision = 0; };
    EntityStateBackend* backend_;
    std::map<EntityStateKey, Fence> rows_;
    std::map<EntityStateKey, std::string> last_bytes_;
};
}
