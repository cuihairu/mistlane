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

// Bounded outstanding-call table for internal entity RPC: capacity is fixed at
// construction and overflow is a typed CAPACITY refusal — never a silent drop
// or an unbounded table. A timed-out admission burns its slot as a surfaced
// tombstone until collected with take(), because the outcome of a
// non-idempotent call is unknown (it may already have applied): retry()
// re-arms only calls declared idempotent when admitted, always under a fresh
// correlation id, so a non-idempotent timeout can never be silently retried
// through this table. The clock is injected by tests, matching RpcHarness.
class RpcQueue {
public:
 enum class State { PENDING, OK, TIMED_OUT, CANCELLED };
 struct Result { State state = State::PENDING; EntityError error = EntityError::INVALID; EntityValues response; };
 explicit RpcQueue(std::size_t capacity) : capacity_(capacity) {}
 // Malformed ids, duplicate live entries and overflow are all typed refusals
 // surfaced through error().
 bool call(std::uint64_t id, std::uint64_t deadline_ms, bool idempotent = false) {
   error_ = EntityError::INVALID;
   if (!id || !deadline_ms) return false;
   if (calls_.count(id)) return false;
   if (calls_.size() >= capacity_) { error_ = EntityError::CAPACITY; return false; }
   calls_.emplace(id, Call{deadline_ms, idempotent});
   return true;
 }
 void advance(std::uint64_t now) {
   now_ = now;
   for (auto& [id, c] : calls_) { (void)id; if (c.state == State::PENDING && now >= c.deadline) c.state = State::TIMED_OUT; }
 }
 bool cancel(std::uint64_t id) {
   error_ = EntityError::INVALID;
   const auto it = calls_.find(id);
   if (it == calls_.end() || it->second.state != State::PENDING) return false;
   it->second.state = State::CANCELLED;
   return true;
 }
 // A late response is discarded with its reason; a second response to a
 // completed call is CONFLICT; an unknown correlation is STALE.
 Result complete(std::uint64_t id, EntityValues response = {}) {
   const auto it = calls_.find(id);
   if (it == calls_.end()) return {State::PENDING, EntityError::STALE, {}};
   auto& call = it->second;
   if (call.state == State::PENDING && now_ >= call.deadline) call.state = State::TIMED_OUT;
   if (call.state == State::TIMED_OUT) return {call.state, EntityError::TIMEOUT, {}};
   if (call.state == State::CANCELLED) return {call.state, EntityError::CANCELLED, {}};
   if (call.state == State::OK) return {call.state, EntityError::CONFLICT, {}};
   call.state = State::OK;
   call.response = std::move(response);
   return {call.state, EntityError::INVALID, call.response};
 }
 // Re-arm a timed-out admission under a fresh correlation. Only an idempotent
 // call may be retried: a non-idempotent timeout stays surfaced until taken,
 // a pending or already-completed call is refused, and the burned correlation
 // is never re-armed (the delivery layer rejects it as CONFLICT anyway).
 bool retry(std::uint64_t old_id, std::uint64_t new_id, std::uint64_t deadline_ms) {
   error_ = EntityError::INVALID;
   const auto old = calls_.find(old_id);
   if (old == calls_.end() || old->second.state != State::TIMED_OUT || !old->second.idempotent) return false;
   if (!new_id || new_id == old_id || !deadline_ms || calls_.count(new_id)) return false;
   calls_.erase(old);
   calls_.emplace(new_id, Call{deadline_ms, true});
   return true;
 }
 // Collect a terminal entry: the admission retires and frees its slot.
 Result take(std::uint64_t id) {
   const auto it = calls_.find(id);
   if (it == calls_.end()) return {State::PENDING, EntityError::STALE, {}};
   const Call call = it->second;
   if (call.state == State::PENDING) return {State::PENDING, EntityError::INVALID, {}};
   const Result result{call.state, EntityError::INVALID, call.response};
   calls_.erase(it);
   return result;
 }
 bool has(std::uint64_t id) const { return calls_.count(id) != 0; }
 State state(std::uint64_t id) const { auto i=calls_.find(id);return i==calls_.end()?State::TIMED_OUT:i->second.state; }
 std::size_t size() const { return calls_.size(); }
 EntityError error() const { return error_; }
private:
 struct Call {
  Call(std::uint64_t d, bool idem) : deadline(d), idempotent(idem) {}
  std::uint64_t deadline;
  bool idempotent;
  State state = State::PENDING;
  EntityValues response;
 };
 std::size_t capacity_;
 std::uint64_t now_=0; std::map<std::uint64_t,Call> calls_;
 EntityError error_ = EntityError::INVALID;
};
}
