// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "mistlane/entity_envelope.hpp"
#include "mistlane/entity_runtime.hpp"
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace mistlane {
// Wire codec boundary between the endpoint and the schema-driven sproto
// decoder/encoder. The codec is a separate slice; until one is attached the
// endpoint serves declarations with no arguments and no returns and answers a
// typed INVALID for anything that would need the codec.
class EntityCodec {
public:
    virtual ~EntityCodec() = default;
    // Decodes the wire payload into typed arguments for the method's declared
    // argument shape; false plus a reason on malformed input, which the
    // endpoint answers as the only error code allowed to carry a reason.
    virtual bool decode_args(const ContractArgs& args, std::string_view payload,
                             EntityValues& out, std::string& reason) const = 0;
    // Encodes typed returns to the wire payload; nullopt when a declared
    // return cannot be encoded.
    virtual std::optional<std::string> encode_returns(const ContractArgs& returns,
                                                      const EntityValues& values) const = 0;
};

// Hosting-side transport binding for entity invocations (docs/entity-contract.md,
// "Invocation envelope and error protocol"): an invocation is a payload kind
// inside the existing bounded frames, not a new TCP protocol. A frame beginning
// with the MLE1 marker carries the 39-byte envelope; any other frame is not
// entity traffic. Replies carry the inner correlation id so a gateway can map
// them onto its own outer transport correlation:
//   request:  MLE1 | envelope (header + payload)
//   reply:    MLE1 | correlation u64 BE | EntityReply
// The endpoint fences first (route, hosting generation, correlation duplicate —
// a fenced call spends no decode work and consumes no slot), decodes the typed
// arguments through the codec, dispatches and answers exactly one EntityReply.
// A notify (zero correlation) answers nothing. Caller identity never comes from
// the frame: the transport supplies the trusted EntityCaller.
class EntityEndpoint {
public:
    explicit EntityEndpoint(const EntityContract& contract, const EntityCodec* codec = nullptr)
        : runtime_(contract), codec_(codec) {}
    EntityRuntime& runtime() { return runtime_; }
    // Handles one frame: nullopt for non-entity traffic, an empty string for a
    // handled notify (no reply exists), else the MLE1 reply frame. The result
    // word feeds a bounded result label, one per handled frame.
    std::optional<std::string> handle(std::string_view frame, const EntityCaller& caller, std::string& result) {
        if (!frame.starts_with("MLE1")) return std::nullopt;
        const auto envelope = decode_entity_envelope(frame.substr(4));
        if (!envelope) {
            result = "malformed";
            return reply_frame(0, {true, EntityError::INVALID, "malformed envelope", {}});
        }
        EntityInvocation invocation = to_entity_invocation(*envelope, caller);
        const bool notify = envelope->correlation_id == 0;
        if (const auto fenced = runtime_.fence(invocation, envelope->correlation_id)) {
            result = notify ? "notify-error" : label(*fenced);
            return notify ? std::string() : reply_frame(envelope->correlation_id, {true, *fenced, {}, {}});
        }
        // The codec needs the declared argument shape; an unknown entity or
        // method leaves the arguments empty so the dispatcher answers the
        // typed error instead of inventing one here.
        if (const auto* declared = method(invocation)) {
            std::string reason;
            if (!decode_arguments(*declared, envelope->payload, invocation, reason)) {
                result = "invalid";
                return reply_frame(envelope->correlation_id, {true, EntityError::INVALID, std::move(reason), {}});
            }
        }
        const auto outcome = runtime_.dispatcher().dispatch(invocation);
        if (notify) {
            result = outcome.ok ? "notify" : "notify-error";
            return std::string();
        }
        if (!outcome.ok) {
            result = label(outcome.error);
            return reply_frame(envelope->correlation_id, {true, outcome.error, {}, {}});
        }
        if (const auto* declared = method(invocation); declared && !declared->returns.empty()) {
            if (!codec_) {
                result = "invalid";
                return reply_frame(envelope->correlation_id, {true, EntityError::INVALID, "returns need a codec", {}});
            }
            if (const auto payload = codec_->encode_returns(declared->returns, outcome.response)) {
                result = "ok";
                return reply_frame(envelope->correlation_id, {false, EntityError::INVALID, {}, *payload});
            }
            result = "invalid";
            return reply_frame(envelope->correlation_id, {true, EntityError::INVALID, "returns not encodable", {}});
        }
        result = "ok";
        return reply_frame(envelope->correlation_id, {false, EntityError::INVALID, {}, {}});
    }
private:
    bool decode_arguments(const ContractMethod& declared, std::string_view payload,
                          EntityInvocation& invocation, std::string& reason) {
        if (declared.args.empty() && payload.empty()) return true;
        if (!codec_) {
            reason = "arguments need a codec";
            return false;
        }
        return codec_->decode_args(declared.args, payload, invocation.arguments, reason);
    }
    const ContractMethod* method(const EntityInvocation& call) {
        for (const auto& entity : runtime_.dispatcher().contract().entities()) {
            if (entity.id != call.target.type_id) continue;
            const auto& methods = call.side == "server" ? entity.server_methods : entity.client_methods;
            for (const auto& candidate : methods)
                if (candidate.id == call.method_id) return &candidate;
            return nullptr;
        }
        return nullptr;
    }
    static std::string reply_frame(std::uint64_t correlation, const EntityReply& reply) {
        const auto body = encode_entity_reply(reply);
        if (!body) return {}; // unreachable: the endpoint only builds valid replies
        std::string out = "MLE1";
        envelope_push_be(out, correlation, 8);
        out += *body;
        return out;
    }
    static const char* label(EntityError error) {
        switch (error) {
            case EntityError::INVALID: return "invalid";
            case EntityError::UNROUTED: return "unrouted";
            case EntityError::STALE: return "stale";
            case EntityError::FORBIDDEN: return "forbidden";
            case EntityError::TIMEOUT: return "timeout";
            case EntityError::CANCELLED: return "cancelled";
            case EntityError::NO_IMPLEMENTATION: return "no-implementation";
            case EntityError::NOT_READY: return "not-ready";
            case EntityError::CAPACITY: return "capacity";
            case EntityError::CONFLICT: return "conflict";
        }
        return "error";
    }
    EntityRuntime runtime_;
    const EntityCodec* codec_ = nullptr;
};
}
