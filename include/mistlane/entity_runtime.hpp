// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "mistlane/entity_dispatch.hpp"
#include "mistlane/entity_persist.hpp"
#include "mistlane/entity_save.hpp"
#include "mistlane/entity_state.hpp"
#include "mistlane/entity_sync.hpp"
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace mistlane {
// Deterministic fixture-driven route table. An entity ref is single-live-instance;
// repeated delivery is rejected by the invocation correlation key. With
// persistence enabled (set_persistence) every declared entity also gets the
// persistence half of the same runtime: a PersistentStore over its property
// store (so a script write marks the record dirty only when it lands on a
// persistent property) and an EntityStateStore bound to the node's backend,
// registered with a PeriodicSaveDriver the node ticks. Persistence is opt-in
// per node; without it spawn behaves exactly as before. Owner attribution
// (set_owner/resolve_caller) records which trusted session may assert an
// entity's owner-declared methods — the transport endpoint and the in-process
// deliver path both resolve it, so a client never claims an owner field.
class EntityRuntime {
public:
 // Property sync push sink: one call per message drained from an entity's
 // observer channel. The transport wiring serializes through SyncWireCodec.
 // One drained message plus the observer it is addressed to: the transport
 // routes the encoded frame by observer id without re-deriving the audience.
 using PropertyPush = std::function<void(const EntityTarget&, const std::string& observer, const SyncMessage&)>;
 explicit EntityRuntime(const EntityContract& contract) : dispatcher_(contract) {}
 EntityDispatcher& dispatcher() { return dispatcher_; }
 // Switch on the persistence chain. The codec is what a snapshot is encoded
 // with, the backend the durable boundary (null keeps everything in process),
 // the period the driver's ms budget under the injected (steady by default)
 // clock. A store a node registered before this call is not back-registered:
 // the runtime is constructed before the node spawns anything.
 void set_persistence(const SprotoCodec* codec, EntityStateBackend* backend, std::uint64_t period_ms) {
   codec_ = codec;
   backend_ = backend;
   driver_.emplace(period_ms);
 }
 bool persistence_enabled() const { return codec_ != nullptr; }
 bool spawn(EntityTarget ref, std::uint32_t hosting_generation = 1) {
   if (!ref.scope || !ref.type_id || !ref.entity_id || !ref.incarnation || !hosting_generation) return false;
   if (!entities_.emplace(address(ref), hosting_generation).second) return false;
   // A declared entity also gets its controlled property store and observer
   // channel; an undeclared type keeps the fence-only behavior (dispatch
   // answers it), so existing spawn semantics are unchanged. With persistence
   // enabled it also gets the forced-save chain, fenced by the spawn
   // generation (monotone, so a re-spawn of a fresh incarnation starts
   // clean).
   for (const auto& entity : dispatcher_.contract().entities()) {
     if (entity.id != ref.type_id) continue;
     auto store = std::make_shared<PropertyStore>(dispatcher_.contract(), ref.type_id);
     stores_.emplace(address(ref), store);
     channels_.emplace(address(ref), store.get());
     if (codec_) {
       auto owned = std::make_unique<PersistentStore>(*store);
       owned->assume(hosting_generation);
       auto record = std::make_unique<EntityStateStore>(ref, *owned, codec_, backend_);
       driver_->add(record.get());
       records_.emplace(address(ref), std::move(record));
       owned_state_.emplace(address(ref), std::move(owned));
     }
     break;
   }
   if (host_route_) host_route_(ref, true);
   return true;
 }
 EntityDispatchResult deliver(const EntityInvocation& call, std::uint64_t correlation_id) {
   if (!correlation_id) return fail(EntityError::INVALID);
   // The in-process path resolves owner attribution exactly like the transport
   // endpoint does, so an owner-declared method is reachable here too.
   EntityInvocation resolved = call;
   resolved.caller = resolve_caller(call.target, call.caller);
   if (const auto fenced = fence(resolved, correlation_id)) return fail(*fenced);
   return dispatcher_.dispatch(resolved);
 }
 // Owner attribution: the trusted session that may assert an entity's
 // owner-declared methods. Set by the side that binds a session to its entity
 // (a login/home binding); never read from an invocation — a client leg
 // claims no owner field, it is matched against the trusted session identity.
 // Zero refuses, and so does an address that is not a live entity here.
 bool set_owner(const EntityTarget& ref, std::uint64_t session_id) {
   const auto a = address(ref);
   if (!entities_.contains(a) || !session_id) return false;
   owners_[a] = session_id;
   return true;
 }
 // The owning session of one entity; 0 when none is attributed (or the entity
 // is gone — destroy clears the binding with its entity).
 std::uint64_t owner(const EntityTarget& ref) const {
   const auto it = owners_.find(address(ref));
   return it == owners_.end() ? 0 : it->second;
 }
 // The caller kind the trusted connection may assert for this target: a
 // client leg whose session owns the entity is upgraded to "owner", so
 // owner-declared methods are reachable without the client ever claiming it.
 // Everything else passes through unchanged — a server leg stays "server",
 // and a session that does not own the entity stays a plain client.
 EntityCaller resolve_caller(const EntityTarget& target, const EntityCaller& caller) const {
   const auto owned = owner(target);
   if (caller.kind == "client" && owned && caller.id == owned) return {"owner", caller.id};
   return caller;
 }
 // Hosting fence without dispatching: routes the target, asserts the entity's
 // current ownership generation and registers the correlation slot for a
 // request (a zero correlation is a notify and registers nothing). Returns
 // nullopt when the call may proceed to argument decode and dispatch, else the
 // typed error to answer. A fenced call never consumes a correlation slot, so
 // the sender may retry under a fresh id. The transport endpoint calls this
 // before the wire codec so a stale or duplicate call spends no decode work.
 std::optional<EntityError> fence(const EntityInvocation& call, std::uint64_t correlation_id) {
   const auto a=address(call.target); const auto it=entities_.find(a);
   if (it==entities_.end()) {
     for (const auto& [known,generation]:entities_) {
       (void)generation;
       if (std::get<0>(known)==call.target.scope && std::get<1>(known)==call.target.type_id && std::get<2>(known)==call.target.entity_id)
         return EntityError::STALE;
     }
     return EntityError::UNROUTED;
   }
   // Hosting-generation fence (docs/entity-contract.md): internal routing
   // asserts the entity's current ownership generation; a call from an older
   // or unknown generation is STALE, matching the MLD1 durable fence. Zero
   // marks a client-originated call, which asserts no generation. The fence
   // precedes correlation registration, so a fenced call consumes no slot,
   // and so does authorization: a FORBIDDEN call burns nothing.
   if (call.hosting_generation && call.hosting_generation != it->second) return EntityError::STALE;
   if (!dispatcher_.authorized(call)) return EntityError::FORBIDDEN;
   if (correlation_id && !delivered_.emplace(a,correlation_id).second) return EntityError::CONFLICT;
   if (correlation_id && delivered_.size() > correlation_cap) {
     // The tombstone set is bounded per entity incarnation: the oldest rows
     // of this entity age out, so an unretired replay window cannot grow the
     // table without bound on a long-lived node.
     const auto begin = delivered_.lower_bound(std::pair{a,0});
     std::size_t aged = 0;
     for (auto row = begin; row != delivered_.end() && row->first == a && aged < correlation_evict; ++row, ++aged)
       delivered_.erase(row++);
   }
   return std::nullopt;
 }
 bool destroy(EntityTarget ref) {
   const auto a = address(ref);
   if (entities_.erase(a) == 0) return false;
   owners_.erase(a); // ownership dies with the entity
   channels_.erase(a);
   stores_.erase(a);
   // The record leaves the periodic driver with its entity. A dirty record is
   // not saved here: destroy is the reclaim path, not a persistence point —
   // the record's fate on the owning data side is a transfer/shutdown concern.
   if (const auto it = records_.find(a); it != records_.end() && driver_) driver_->remove(it->second.get());
   records_.erase(a);
   owned_state_.erase(a);
   // Drop correlation tombstones for this exact incarnation so a re-spawn of
   // the same address starts with a clean dedupe table instead of growing
   // without bound on a long-lived node.
   std::erase_if(delivered_, [a](const auto& row) { return row.first == a; });
   if (host_route_) host_route_(ref, false);
   return true;
 }
 // The controlled store of one live entity, seeded with its declared defaults.
 // Null for an unrouted, destroyed or undeclared address.
 PropertyStore* properties(const EntityTarget& ref) {
   const auto it = stores_.find(address(ref));
   return it == stores_.end() ? nullptr : it->second.get();
 }
 // The owning handle to the same store, for callers that must keep a handle
 // valid past this runtime's destroy (a Lua property proxy anchors on it, so a
 // script-held proxy outlives its entity instead of dangling).
 std::shared_ptr<PropertyStore> property_store(const EntityTarget& ref) {
   const auto it = stores_.find(address(ref));
   return it == stores_.end() ? nullptr : it->second;
 }
 // The persistence write path of one live entity: a script write through this
 // store marks the record dirty only when it lands on a persistent property,
 // and the single forced-save entry reads the same fence values it advanced.
 // Null when the address has no store or the node never enabled persistence.
 PersistentStore* persistent(const EntityTarget& ref) {
   const auto it = owned_state_.find(address(ref));
   return it == owned_state_.end() ? nullptr : it->second.get();
 }
 // The durable driver of one live entity: the store the periodic tick saves
 // through the single entry, bound to the node's backend. Null when
 // persistence is off or the address is not a live declared entity.
 EntityStateStore* state_store(const EntityTarget& ref) {
   const auto it = records_.find(address(ref));
   return it == records_.end() ? nullptr : it->second.get();
 }
 // One periodic save pass — the node calls this from its tick. A no-op when
 // persistence is off.
 PeriodicSaveDriver::Report tick_saves() {
   return driver_ ? driver_->tick() : PeriodicSaveDriver::Report{};
 }
 // Shutdown save: every dirty record through the same entry once, period
 // ignored. The node calls this before teardown, so a stop never silently
 // drops a refused save — the report names what did not land.
 PeriodicSaveDriver::Report shutdown_save() {
   return driver_ ? driver_->flush() : PeriodicSaveDriver::Report{};
 }
 // Live persistence records registered with the driver (for observability).
 std::size_t registered_saves() const { return driver_ ? driver_->registered() : 0; }
 // Observer admission and departure on one entity's property stream: visibility
 // is per property (the channel filters SERVER_ONLY and, for guests, OWN_CLIENT).
 bool observe(const EntityTarget& ref, const std::string& id, bool is_owner) {
   const auto it = channels_.find(address(ref));
   return it != channels_.end() && it->second.observe(id, is_owner);
 }
 bool depart(const EntityTarget& ref, const std::string& id) {
   const auto it = channels_.find(address(ref));
   return it != channels_.end() && it->second.depart(id);
 }
 // Host route publish: called with owned=true after a successful spawn and
 // owned=false after a successful destroy, so the node's coordinator leg can
 // tell the registry which endpoint hosts which entity (the dynamic half of
 // host routing). Unset, or a node without a coordinator leg, publishes nothing.
 using HostRoute = std::function<void(const EntityTarget&, bool)>;
 void set_host_route(HostRoute publish) { host_route_ = std::move(publish); }
 // The live entity addresses, for re-publishing the whole table when a
 // coordinator leg (re)establishes.
 std::vector<EntityTarget> live_targets() const {
   std::vector<EntityTarget> out;
   out.reserve(entities_.size());
   for (const auto& [a, generation] : entities_) {
     (void)generation;
     out.push_back({std::get<0>(a), std::get<1>(a), std::get<3>(a), std::get<2>(a)});
   }
   return out;
 }
 void set_property_push(PropertyPush push) { push_ = std::move(push); }
 // Drain one entity's journal and emit every message through the sink. False
 // when the address has no property stream. Without a sink nothing drains —
 // dropping deltas into nowhere would strand observers on gapped sequences.
 bool flush_properties(const EntityTarget& ref) {
   const auto it = channels_.find(address(ref));
   if (it == channels_.end()) return false;
   if (push_)
     for (const auto& delivery : it->second.deliver_tagged())
       push_(ref, delivery.observer, delivery.message);
   return true;
 }
private:
 using Address=std::tuple<std::uint32_t,std::uint32_t,std::int64_t,std::uint32_t>;
 static Address address(const EntityTarget& r) { return {r.scope,r.type_id,r.entity_id,r.incarnation}; }
 static EntityDispatchResult fail(EntityError e) { EntityDispatchResult r;r.error=e;return r; }
 // Per-entity tombstone retention: once an entity holds more than this many
 // live correlations, the oldest are evicted (a replay older than the window
 // may double-deliver, matching the bounded at-least-once contract).
 static constexpr std::size_t correlation_cap = 4096, correlation_evict = 256;
 EntityDispatcher dispatcher_;
 std::map<Address,std::uint32_t> entities_;
 // Owner attribution: the trusted session bound to one entity. A client leg
 // never reports its owner — the runtime matches the caller's session id
 // against this table (see resolve_caller).
 std::map<Address,std::uint64_t> owners_;
 std::set<std::pair<Address,std::uint64_t>> delivered_;
 std::map<Address,std::shared_ptr<PropertyStore>> stores_;
 std::map<Address,PropertyChannel> channels_;
 // Persistence half of the runtime, one chain per live declared entity: the
 // record (the durable driver) sits on top of the owned persistent store, so
 // a store outlives its record and the driver only ever holds a record that
 // still has a store behind it.
 std::map<Address,std::unique_ptr<EntityStateStore>> records_;
 std::map<Address,std::unique_ptr<PersistentStore>> owned_state_;
 std::optional<PeriodicSaveDriver> driver_;
 const SprotoCodec* codec_ = nullptr;
 EntityStateBackend* backend_ = nullptr;
 PropertyPush push_;
 HostRoute host_route_;
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
