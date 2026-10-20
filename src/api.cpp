// SPDX-License-Identifier: Apache-2.0
#include "mistlane/api.hpp"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <algorithm>
#include <charconv>
#include <limits>

namespace mistlane {
namespace {
bool valid_character(const RecordStore::RecordKey& key) {
    return record_identifier(key.first) && record_identifier(key.second);
}
void field(std::string& bytes, std::string_view value) {
    const auto size = static_cast<std::uint32_t>(value.size());
    for (int shift : {24, 16, 8, 0}) bytes.push_back(static_cast<char>((size >> shift) & 255));
    bytes.append(value);
}
// Consumes one framed field; false when the prefix lies about what remains.
bool read_field(std::string_view& bytes, std::string& out) {
    if (bytes.size() < 4) return false;
    std::uint32_t size = 0;
    for (int i = 0; i < 4; ++i) size = (size << 8) | static_cast<unsigned char>(bytes[i]);
    bytes.remove_prefix(4);
    if (bytes.size() < size) return false;
    out.assign(bytes.substr(0, size));
    bytes.remove_prefix(size);
    return true;
}
}
bool valid_api_request(const ApiRequest& r) {
    if (!record_identifier(r.audience) || !record_identifier(r.principal) || !record_identifier(r.key_id)
            || !valid_character(r.character) || !r.timestamp
            || r.timestamp > std::numeric_limits<std::uint64_t>::max() - api_window_seconds
            || r.nonce.size() != 32 || !std::all_of(r.nonce.begin(), r.nonce.end(), [](unsigned char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            })) return false;
    if (r.operation == ApiOperation::Accept)
        return record_identifier(r.idempotency_key) && r.payload.size() <= 1024*1024;
    return r.operation == ApiOperation::Owner && r.idempotency_key.empty() && r.payload.empty();
}
std::string api_canonical(const ApiRequest& r) {
    if (!valid_api_request(r)) throw std::invalid_argument("invalid API request");
    std::string bytes = "MLA1";
    // u32 BE byte length + exact bytes for ALL ten fields, in this order.
    for (const auto& value : {r.operation == ApiOperation::Accept ? std::string("ACCEPT") : std::string("OWNER"),
            r.audience, r.principal, r.key_id, std::to_string(r.timestamp), r.nonce,
            r.character.first, r.character.second, r.idempotency_key, r.payload}) field(bytes, value);
    return bytes;
}
ApiSignature hmac_sha256(const ApiSecret& secret, std::string_view message) {
    ApiSignature signature{};
    std::size_t size = 0;
    if (!EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA256", nullptr, secret.data(), secret.size(),
            reinterpret_cast<const unsigned char*>(message.data()), message.size(), signature.data(), signature.size(), &size)
            || size != signature.size()) throw std::runtime_error("HMAC-SHA256 failed");
    return signature;
}
ApiSignature api_sign(const ApiRequest& request, const ApiSecret& secret) {
    return hmac_sha256(secret, api_canonical(request));
}
ApiAuth ApiReplayGuard::claim(const ApiRequest& request, std::uint64_t now) {
    std::lock_guard lock(mutex_);
    if (!now || now < last_now_) return ApiAuth::Clock; // Fail closed on wall-clock rollback.
    last_now_ = now;
    const auto timestamp = request.timestamp;
    if ((now >= timestamp ? now - timestamp : timestamp - now) > api_window_seconds) return ApiAuth::Window;
    // Keep through timestamp + window INCLUSIVELY; no unexpired eviction.
    std::erase_if(expires_, [now](const auto& row) { return row.second < now; });
    const Key key{request.audience, request.principal, request.nonce};
    if (expires_.contains(key)) return ApiAuth::Replay;
    if (expires_.size() >= capacity_) return ApiAuth::Full;
    expires_.emplace(key, timestamp + api_window_seconds);
    return ApiAuth::Ok;
}
ApiVerifier::ApiVerifier(std::string audience, std::vector<ApiCredential> credentials, ApiReplayGuard& replay)
  : audience_(std::move(audience)), replay_(replay) {
    if (!record_identifier(audience_)) throw std::invalid_argument("invalid API audience");
    for (auto& credential : credentials) {
        if (!record_identifier(credential.principal) || !record_identifier(credential.key_id) || credential.realms.empty()
                || !std::all_of(credential.realms.begin(), credential.realms.end(), record_identifier))
            throw std::invalid_argument("invalid API credential metadata");
        const auto key = std::pair{credential.principal, credential.key_id};
        if (!credentials_.emplace(key, std::move(credential)).second)
            throw std::invalid_argument("duplicate API credential");
    }
}
ApiAuth ApiVerifier::verify(const ApiRequest& request, std::uint64_t now) const {
    if (!valid_api_request(request) || request.audience != audience_) return ApiAuth::Invalid;
    const auto key = credentials_.find({request.principal, request.key_id});
    if (key == credentials_.end()) return ApiAuth::UnknownCredential;
    if (key->second.valid_until && now > key->second.valid_until) return ApiAuth::UnknownCredential;
    if (!key->second.realms.contains(request.character.first)) return ApiAuth::Forbidden;
    const auto expected = api_sign(request, key->second.secret);
    if (CRYPTO_memcmp(expected.data(), request.signature.data(), expected.size()) != 0) return ApiAuth::BadSignature;
    return replay_.claim(request, now); // Failed signatures never consume a nonce.
}
ApiAcceptance ApiMemoryAcceptanceStore::create(const ApiCommand& command) {
    const auto& key = command.key;
    if (!record_identifier(key.audience) || !record_identifier(key.principal) || !valid_character(key.character)
            || !record_identifier(key.idempotency_key) || command.payload.size() > 1024*1024) return {};
    std::lock_guard lock(mutex_);
    if (const auto old = rows_.find(key); old != rows_.end()) {
        if (old->second.command != command) return {ApiCreate::Conflict, ApiDurability::Volatile, std::nullopt};
        return {ApiCreate::Existing, ApiDurability::Volatile, old->second};
    }
    if (rows_.size() >= capacity_) return {ApiCreate::Full, ApiDurability::Volatile, std::nullopt};
    // One mutation stores both the immutable receipt and pending command;
    // there is no separate check-then-insert or business callback to repeat.
    const auto [row, inserted] = rows_.emplace(key, ApiReceipt{command});
    return {inserted ? ApiCreate::Inserted : ApiCreate::Existing, ApiDurability::Volatile, row->second};
}
std::size_t ApiMemoryAcceptanceStore::size() const {
    std::lock_guard lock(mutex_);
    return rows_.size();
}
ApiReply ApiSurface::handle(const ApiRequest& request, std::uint64_t now) const {
    ApiReply reply;
    reply.auth = verifier_.verify(request, now);
    if (reply.auth != ApiAuth::Ok) return reply;
    if (request.operation == ApiOperation::Accept) {
        reply.acceptance = store_.create({{request.audience, request.principal, request.character,
            request.idempotency_key}, request.payload});
    } else {
        auto current = lookup_(request.character);
        reply.ownership = ApiOwnershipReply{current ? ApiOwnershipReply::Status::Found : ApiOwnershipReply::Status::NotFound,
            std::move(current)};
    }
    return reply;
}
bool parse_api_body(std::string_view body, ApiRequest& out) {
    if (body.substr(0, 4) != "MLA1") return false;
    body.remove_prefix(4);
    ApiRequest request;
    std::string operation, timestamp, realm, character;
    if (!read_field(body, operation) || !read_field(body, request.audience)
            || !read_field(body, request.principal) || !read_field(body, request.key_id)
            || !read_field(body, timestamp) || !read_field(body, request.nonce)
            || !read_field(body, realm) || !read_field(body, character)
            || !read_field(body, request.idempotency_key) || !read_field(body, request.payload)
            || body.size() != request.signature.size())
        return false; // Exactly ten framed fields, then exactly the signature.
    if (operation == "ACCEPT") request.operation = ApiOperation::Accept;
    else if (operation == "OWNER") request.operation = ApiOperation::Owner;
    else return false;
    const auto digits = std::from_chars(timestamp.data(), timestamp.data()+timestamp.size(), request.timestamp);
    if (digits.ec != std::errc() || digits.ptr != timestamp.data()+timestamp.size()) return false;
    request.character = {std::move(realm), std::move(character)};
    std::copy_n(body.begin(), request.signature.size(), request.signature.begin());
    if (!valid_api_request(request)) return false;
    out = std::move(request);
    return true;
}
std::string api_reply_body(const ApiReply& reply) {
    const auto auth = [reply] {
        switch (reply.auth) {
            case ApiAuth::Ok: return "OK";
            case ApiAuth::Invalid: return "INVALID";
            case ApiAuth::UnknownCredential: return "UNKNOWN_CREDENTIAL";
            case ApiAuth::Forbidden: return "FORBIDDEN";
            case ApiAuth::BadSignature: return "BAD_SIGNATURE";
            case ApiAuth::Window: return "WINDOW";
            case ApiAuth::Replay: return "REPLAY";
            case ApiAuth::Clock: return "CLOCK";
            case ApiAuth::Full: return "FULL";
        }
        return "INVALID";
    }();
    std::string bytes = "MLA1";
    field(bytes, auth);
    if (reply.acceptance) {
        switch (reply.acceptance->result) {
            case ApiCreate::Inserted: field(bytes, "INSERTED"); break;
            case ApiCreate::Existing: field(bytes, "EXISTING"); break;
            case ApiCreate::Conflict: field(bytes, "CONFLICT"); break;
            case ApiCreate::Full: field(bytes, "FULL"); break;
            default: field(bytes, "INVALID"); break;
        }
        field(bytes, reply.acceptance->durability == ApiDurability::Durable ? "DURABLE" : "VOLATILE");
        field(bytes, reply.acceptance->receipt ? reply.acceptance->receipt->command.payload : std::string{});
    } else {
        field(bytes, "");
        field(bytes, "");
        field(bytes, "");
    }
    field(bytes, reply.ownership
        ? (reply.ownership->status == ApiOwnershipReply::Status::Found ? "FOUND" : "NOT_FOUND") : "");
    return bytes;
}
unsigned api_http_status(const ApiReply& reply) {
    switch (reply.auth) {
        case ApiAuth::Ok: break;
        case ApiAuth::Invalid: return 400;
        case ApiAuth::UnknownCredential:
        case ApiAuth::BadSignature:
        case ApiAuth::Window:
        case ApiAuth::Replay: return 401;
        case ApiAuth::Forbidden: return 403;
        case ApiAuth::Clock:
        case ApiAuth::Full: return 503;
    }
    if (reply.ownership) return reply.ownership->status == ApiOwnershipReply::Status::Found ? 200 : 404;
    if (reply.acceptance) {
        switch (reply.acceptance->result) {
            case ApiCreate::Inserted:
            case ApiCreate::Existing: return 200;
            case ApiCreate::Conflict: return 409;
            case ApiCreate::Full: return 503;
            default: return 400;
        }
    }
    return 400;
}
}
