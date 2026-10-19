// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "mistlane/entity_dispatch.hpp"
#include <map>
#include <set>
#include <tuple>

namespace mistlane {
// Deterministic fixture-driven route table. An entity ref is single-live-instance;
// repeated delivery is rejected by the invocation correlation key.
class EntityRuntime {
public:
 explicit EntityRuntime(const EntityContract& contract) : dispatcher_(contract) {}
 EntityDispatcher& dispatcher() { return dispatcher_; }
 bool spawn(EntityTarget ref, std::uint32_t hosting_generation = 1) {
   if (!ref.scope || !ref.type_id || !ref.entity_id || !ref.incarnation || !hosting_generation) return false;
   return entities_.emplace(address(ref), hosting_generation).second;
 }
 EntityDispatchResult deliver(const EntityInvocation& call, std::uint64_t correlation_id) {
   if (!correlation_id) return fail(EntityError::INVALID);
   const auto a=address(call.target); const auto it=entities_.find(a);
   if (it==entities_.end()) {
     for (const auto& [known,generation]:entities_) {
       (void)generation;
       if (std::get<0>(known)==call.target.scope && std::get<1>(known)==call.target.type_id && std::get<2>(known)==call.target.entity_id)
         return fail(EntityError::STALE);
     }
     return fail(EntityError::UNROUTED);
   }
   // Hosting-generation fence (docs/entity-contract.md): internal routing
   // asserts the entity's current ownership generation; a call from an older
   // or unknown generation is STALE, matching the MLD1 durable fence. Zero
   // marks a client-originated call, which asserts no generation. The fence
   // precedes correlation registration, so a fenced call consumes no slot.
   if (call.hosting_generation && call.hosting_generation != it->second) return fail(EntityError::STALE);
   if (!delivered_.emplace(a,correlation_id).second) return fail(EntityError::CONFLICT);
   return dispatcher_.dispatch(call);
 }
 bool destroy(EntityTarget ref) { return entities_.erase(address(ref)) != 0; }
private:
 using Address=std::tuple<std::uint32_t,std::uint32_t,std::int64_t,std::uint32_t>;
 static Address address(const EntityTarget& r) { return {r.scope,r.type_id,r.entity_id,r.incarnation}; }
 static EntityDispatchResult fail(EntityError e) { EntityDispatchResult r;r.error=e;return r; }
 EntityDispatcher dispatcher_;
 std::map<Address,std::uint32_t> entities_;
 std::set<std::pair<Address,std::uint64_t>> delivered_;
};

// Single-threaded RPC lifecycle model driven by a monotonic clock supplied by tests.
class RpcHarness {
public:
 enum class State { PENDING, COMPLETED, TIMED_OUT, DISCONNECTED };
 struct Result { State state; EntityError error; EntityValues response; };
 bool request(std::uint64_t id, std::uint64_t deadline_ms) {
   if (!id || !deadline_ms || calls_.contains(id)) return false;
   calls_.emplace(id, Call{deadline_ms}); return true;
 }
 Result advance(std::uint64_t now) {
   now_=now;
   for (auto& [id,c]:calls_) { (void)id; if(c.state==State::PENDING && now>=c.deadline)c.state=State::TIMED_OUT; }
   return {State::PENDING,EntityError::INVALID,{}};
 }
 Result respond(std::uint64_t id, EntityValues value={}) {
   const auto i=calls_.find(id);
   if(i==calls_.end()) return {State::TIMED_OUT,EntityError::STALE,{}}; // unknown/late response is discarded
   auto& c=i->second;
   if(c.state==State::PENDING && now_>=c.deadline)c.state=State::TIMED_OUT;
   if(c.state==State::TIMED_OUT) return {c.state,EntityError::TIMEOUT,{}};
   if(c.state==State::DISCONNECTED) return {c.state,EntityError::CANCELLED,{}};
   if(c.state==State::COMPLETED) return {c.state,EntityError::CONFLICT,{}};
   c.state=State::COMPLETED;c.response=std::move(value);return {c.state,EntityError::INVALID,c.response};
 }
 void disconnect() { for(auto& [id,c]:calls_){(void)id;if(c.state==State::PENDING)c.state=State::DISCONNECTED;} }
 State state(std::uint64_t id) const { auto i=calls_.find(id);return i==calls_.end()?State::TIMED_OUT:i->second.state; }
private:
 struct Call { explicit Call(std::uint64_t d):deadline(d){} std::uint64_t deadline; State state=State::PENDING; EntityValues response; };
 std::uint64_t now_=0; std::map<std::uint64_t,Call> calls_;
};
}
