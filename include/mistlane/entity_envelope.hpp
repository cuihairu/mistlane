// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "mistlane/entity_dispatch.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mistlane {
// Wire form of the decided invocation envelope (docs/entity-contract.md,
// "Invocation envelope and error protocol"): a payload kind riding the
// existing bounded frames, not a new TCP protocol. Fixed big-endian header
// followed by the payload as the remainder; the transport frame bounds it.
//   scope u32 | type_id u32 | entity_id i64 | incarnation u32 | side u8
//   | method_id u16 | correlation_id u64 | hosting_generation u32
//   | deadline_ms u32 | payload
// side: 0 = client, 1 = server. The envelope carries no caller identity:
// authentication comes from the trusted connection, not the payload.
struct EntityEnvelope {
    std::uint32_t scope = 0, type_id = 0, incarnation = 0;
    std::int64_t entity_id = 0;
    std::uint8_t side = 1;
    std::uint16_t method_id = 0;
    std::uint64_t correlation_id = 0;
    std::uint32_t hosting_generation = 0;
    std::uint32_t deadline_ms = 0;
    std::string payload;
    bool operator==(const EntityEnvelope&) const = default;
};
inline constexpr std::size_t kEntityEnvelopeHeader = 39;
inline constexpr std::uint8_t kEntitySideClient = 0, kEntitySideServer = 1;

inline void envelope_push_be(std::string& out, std::uint64_t value, std::size_t width) {
    for (std::size_t shift = width * 8; shift;) {
        shift -= 8;
        out.push_back(static_cast<char>((value >> shift) & 0xff));
    }
}
inline bool envelope_read_be(std::string_view in, std::size_t& offset, std::size_t width, std::uint64_t& out) {
    if (in.size() - offset < width) return false;
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i) value = (value << 8) | static_cast<unsigned char>(in[offset + i]);
    offset += width;
    out = value;
    return true;
}
inline std::string encode_entity_envelope(const EntityEnvelope& envelope) {
    std::string out;
    out.reserve(kEntityEnvelopeHeader + envelope.payload.size());
    envelope_push_be(out, envelope.scope, 4);
    envelope_push_be(out, envelope.type_id, 4);
    envelope_push_be(out, static_cast<std::uint64_t>(envelope.entity_id), 8);
    envelope_push_be(out, envelope.incarnation, 4);
    envelope_push_be(out, envelope.side, 1);
    envelope_push_be(out, envelope.method_id, 2);
    envelope_push_be(out, envelope.correlation_id, 8);
    envelope_push_be(out, envelope.hosting_generation, 4);
    envelope_push_be(out, envelope.deadline_ms, 4);
    out += envelope.payload;
    return out;
}
inline std::optional<EntityEnvelope> decode_entity_envelope(std::string_view bytes) {
    if (bytes.size() < kEntityEnvelopeHeader) return std::nullopt;
    std::size_t offset = 0;
    std::uint64_t scope = 0, type_id = 0, entity_id = 0, incarnation = 0, side = 0, method_id = 0,
                  correlation_id = 0, hosting_generation = 0, deadline_ms = 0;
    if (!envelope_read_be(bytes, offset, 4, scope) || !envelope_read_be(bytes, offset, 4, type_id)
        || !envelope_read_be(bytes, offset, 8, entity_id) || !envelope_read_be(bytes, offset, 4, incarnation)
        || !envelope_read_be(bytes, offset, 1, side) || !envelope_read_be(bytes, offset, 2, method_id)
        || !envelope_read_be(bytes, offset, 8, correlation_id) || !envelope_read_be(bytes, offset, 4, hosting_generation)
        || !envelope_read_be(bytes, offset, 4, deadline_ms) || side > 1) return std::nullopt;
    EntityEnvelope envelope;
    envelope.scope = static_cast<std::uint32_t>(scope);
    envelope.type_id = static_cast<std::uint32_t>(type_id);
    envelope.entity_id = static_cast<std::int64_t>(entity_id);
    envelope.incarnation = static_cast<std::uint32_t>(incarnation);
    envelope.side = static_cast<std::uint8_t>(side);
    envelope.method_id = static_cast<std::uint16_t>(method_id);
    envelope.correlation_id = correlation_id;
    envelope.hosting_generation = static_cast<std::uint32_t>(hosting_generation);
    envelope.deadline_ms = static_cast<std::uint32_t>(deadline_ms);
    envelope.payload.assign(bytes.substr(kEntityEnvelopeHeader));
    return envelope;
}
// The envelope fixes addressing and routing fields; the sproto decode layer
// fills the typed arguments before the dispatcher sees the invocation.
inline EntityInvocation to_entity_invocation(const EntityEnvelope& envelope, EntityCaller caller) {
    EntityInvocation call;
    call.target = {envelope.scope, envelope.type_id, envelope.incarnation, envelope.entity_id};
    call.caller = std::move(caller);
    call.side = envelope.side == kEntitySideClient ? "client" : "server";
    call.method_id = envelope.method_id;
    call.hosting_generation = envelope.hosting_generation;
    return call;
}

// A request answers with exactly one of the typed returns or a single closed
// EntityError, never both and never partial (docs/entity-contract.md):
//   kind 0: u8 0x00 | returns payload
//   kind 1: u8 0x01 | u8 error code | [u16 reason length | reason] for INVALID
// Only INVALID may carry a reason; encode refuses anything else. Unknown kind
// or error-code bytes are malformed, not mapped.
struct EntityReply {
    bool is_error = false;
    EntityError error = EntityError::INVALID;
    std::string reason;
    std::string payload;
    bool operator==(const EntityReply&) const = default;
};
inline std::optional<std::string> encode_entity_reply(const EntityReply& reply) {
    if (!reply.is_error) {
        std::string out;
        out.push_back('\x00');
        out += reply.payload;
        return out;
    }
    if (static_cast<unsigned>(reply.error) > 9 || (!reply.reason.empty() && reply.error != EntityError::INVALID))
        return std::nullopt;
    std::string out;
    out.push_back('\x01');
    out.push_back(static_cast<char>(reply.error));
    if (reply.error == EntityError::INVALID) {
        envelope_push_be(out, reply.reason.size(), 2);
        out += reply.reason;
    }
    return out;
}
inline std::optional<EntityReply> decode_entity_reply(std::string_view bytes) {
    if (bytes.empty()) return std::nullopt;
    EntityReply reply;
    if (bytes[0] == '\x00') {
        reply.payload.assign(bytes.substr(1));
        return reply;
    }
    if (bytes[0] != '\x01' || bytes.size() < 2) return std::nullopt;
    const auto code = static_cast<unsigned char>(bytes[1]);
    if (code > 9) return std::nullopt;
    reply.is_error = true;
    reply.error = static_cast<EntityError>(code);
    if (reply.error == EntityError::INVALID) {
        std::size_t offset = 2;
        std::uint64_t width = 0;
        if (!envelope_read_be(bytes, offset, 2, width) || bytes.size() - offset != width) return std::nullopt;
        reply.reason.assign(bytes.substr(offset));
    } else if (bytes.size() != 2) {
        return std::nullopt;
    }
    return reply;
}
}
