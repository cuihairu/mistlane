// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "mistlane/entity_codec_iface.hpp"
#include "mistlane/entity_envelope.hpp"
#include "mistlane/entity_handshake.hpp"
#include "mistlane/entity_runtime.hpp"
#include "mistlane/entity_version.hpp"
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mistlane {

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
    explicit EntityEndpoint(const EntityContract& contract, const EntityCodec* codec = nullptr,
                            const SprotoCodec* persist_codec = nullptr,
                            EntityStateBackend* persist_backend = nullptr,
                            std::uint64_t save_period_ms = 0,
                            std::vector<std::string> client_locks = {})
        : runtime_(contract), codec_(codec), gate_(contract) {
        // The persistence chain is opt-in: a host that passes the persistent
        // codec enables per-entity forced-save records under the node backend.
        if (persist_codec) runtime_.set_persistence(persist_codec, persist_backend, save_period_ms);
        // The negotiated client versions are opt-in: a host that lists
        // published client locks admits their digests at the MLV1 handshake
        // and projects observers onto their declared property sets. A bad
        // lock refuses startup here, never mid-session.
        for (const auto& lock : client_locks) gate_.add_client_lock(lock);
    }
    EntityRuntime& runtime() { return runtime_; }
    // The host contract digest a successful handshake echoes.
    std::uint64_t contract_digest() const { return gate_.digest(); }
    // One MLV1 frame: the verdict (BAD for a malformed digest, VERSION for a
    // well-formed digest no configured client lock claims) plus, on admission,
    // the projection the session's observers will be built with. Purity: no
    // session state lives here, so the same frame always answers the same way.
    enum class VersionVerdict { Accept, Bad, Incompatible };
    VersionVerdict negotiate(std::string_view frame, std::uint64_t& host_digest,
                             const SyncView*& view) const {
        const auto client = parse_entity_handshake(frame);
        if (!client) return VersionVerdict::Bad;
        if (!gate_.admits(*client)) return VersionVerdict::Incompatible;
        host_digest = gate_.digest();
        view = gate_.view_for(*client);
        return VersionVerdict::Accept;
    }
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
        // Owner attribution: the trusted session identity is matched against the
        // entity's owner, never a field the client reports (see EntityRuntime).
        invocation.caller = runtime_.resolve_caller(invocation.target, invocation.caller);
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
    EntityVersionGate gate_;
};
}
