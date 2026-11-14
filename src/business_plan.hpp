// SPDX-License-Identifier: Apache-2.0
// The business scenario shared by `mistlane bot --scenario business` and the
// standalone tools/bots load tool: what a bot connection does, resolved once
// from the contract so every thread encodes identical wire shapes. Method ids
// come from the contract document by name, never by assumed constants — a
// renumbered contract breaks the bot loudly at startup, not silently on wire.
#pragma once
#include "cli_client.hpp"
#include "mistlane/entity_contract.hpp"
#include "mistlane/entity_codec.hpp"
#include "mistlane/entity_envelope.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace mistlane {
struct BusinessPlan {
    std::uint32_t scope = 1, type_id = 2, incarnation = 1, generation = 1;
    std::string token_prefix = "bot-";
    unsigned login = 0, join = 0, move = 0, heartbeat = 0;
    SprotoCodec codec;
    const ContractMethod* login_method = nullptr;
    const ContractMethod* join_method = nullptr;
    const ContractMethod* move_method = nullptr;
    const ContractMethod* heartbeat_method = nullptr;
    // Method resolution reads type_id, so the entity type must be known at
    // construction; assigning it afterwards would not re-resolve.
    explicit BusinessPlan(const EntityContract& contract,
                          std::uint32_t entity_type = 2,
                          std::uint32_t plan_scope = 1,
                          std::uint32_t plan_incarnation = 1,
                          std::uint32_t plan_generation = 1)
      : scope(plan_scope), type_id(entity_type), incarnation(plan_incarnation),
        generation(plan_generation), codec(contract) {
        for (const auto& entity : contract.entities()) {
            if (entity.id != type_id) continue;
            for (const auto& method : entity.server_methods) {
                if (method.name == "login") login_method = &method;
                else if (method.name == "joinRoom") join_method = &method;
                else if (method.name == "move") move_method = &method;
                else if (method.name == "heartbeat") heartbeat_method = &method;
            }
        }
        const auto need = [&](const ContractMethod* method, const char* name) {
            if (!method) throw std::invalid_argument(std::string("contract has no server method: ")+name);
        };
        need(login_method, "login"); need(join_method, "joinRoom");
        need(move_method, "move"); need(heartbeat_method, "heartbeat");
    }
    // One MLE1 request frame: marker, 39-byte envelope, sproto payload. The
    // envelope is filled field-by-field on purpose: EntityEnvelope declares
    // incarnation before entity_id while the wire carries entity_id first,
    // so positional aggregate init silently swaps the two addresses.
    std::string frame(std::uint64_t entity_id, const ContractMethod& method, const EntityValues& values,
                      std::uint64_t correlation, std::uint32_t deadline_ms) const {
        auto payload = codec.encode_returns(method.args, values);
        if (!payload) throw std::runtime_error(std::string("cannot encode args for ")+method.name);
        EntityEnvelope envelope;
        envelope.scope = scope;
        envelope.type_id = type_id;
        envelope.entity_id = static_cast<std::int64_t>(entity_id);
        envelope.incarnation = incarnation;
        envelope.side = kEntitySideServer;
        envelope.method_id = static_cast<std::uint16_t>(method.id);
        envelope.correlation_id = correlation;
        envelope.hosting_generation = generation;
        envelope.deadline_ms = deadline_ms;
        envelope.payload = *payload;
        return "MLE1" + encode_entity_envelope(envelope);
    }
};

// Per-message-type latencies the load tool collects; the plain bot passes
// nullptr and keeps its historical counters-only behaviour.
inline constexpr unsigned business_step_types = 4; // login, join, move, heartbeat
using StepLatencies = std::array<std::vector<double>, business_step_types>;

// One step of the ramp: driven per connection thread, reports one of the four
// scenario counters. A reply must echo the correlation and decode as the OK
// kind; anything else throws and fails the connection.
inline void business_step(const BusinessPlan& plan, cli::Client& client, std::uint64_t entity_id,
                          std::uint64_t& correlation, std::uint32_t deadline_ms, unsigned step,
                          unsigned heartbeat_every, unsigned room, unsigned connection,
                          std::array<unsigned long, 4>& counters, std::mutex& mutex,
                          StepLatencies* latencies = nullptr) {
    const bool beat = heartbeat_every && step % heartbeat_every == heartbeat_every - 1;
    const ContractMethod* method;
    EntityValues values;
    if (step == 0) {
        method = plan.login_method;
        values.emplace("token", plan.token_prefix + std::to_string(connection));
    } else if (step == 1) {
        method = plan.join_method;
        values.emplace("room", static_cast<std::int64_t>(room));
    } else if (beat) {
        method = plan.heartbeat_method;
    } else {
        method = plan.move_method;
        values.emplace("x", static_cast<std::int64_t>(step));
        values.emplace("y", static_cast<std::int64_t>(connection));
    }
    const auto request = plan.frame(entity_id, *method, values, ++correlation, deadline_ms);
    const auto begin = std::chrono::steady_clock::now();
    auto response = client.request(request);
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now()-begin).count();
    if (response.size() < 12 || !response.starts_with("MLE1")) throw std::runtime_error("not an MLE1 reply");
    std::uint64_t echoed = 0;
    for (std::size_t i = 4; i < 12; ++i) echoed = echoed << 8 | static_cast<unsigned char>(response[i]);
    if (echoed != correlation) throw std::runtime_error("reply correlation mismatch");
    const auto reply = decode_entity_reply(std::string_view(response).substr(12));
    if (!reply) throw std::runtime_error("malformed entity reply");
    if (reply->is_error) throw std::runtime_error("entity error " + std::to_string(static_cast<unsigned>(reply->error)));
    const std::size_t slot = step == 0 ? 0 : step == 1 ? 1 : beat ? 3 : 2;
    std::lock_guard lock(mutex);
    ++counters[slot];
    if (latencies) (*latencies)[slot].push_back(ms);
}
}
