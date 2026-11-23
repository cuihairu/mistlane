// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mistlane {

// docs/scene-lifecycle.md: three layers, one rule each — a map definition is
// authored static content registered per process and identified by
// (definition id, digest); the spatial cache is the process-local read-only
// subset keyed by the same pair, shared by every instance of a definition and
// released by references; a scene instance references a definition and never
// owns a map. This is the model half (the AvatarRuntime pattern): the loader,
// the timers and the node wiring are later slices — what is total here is the
// prepare state machine (only `ready` accepts entry; a timeout is a failure,
// never a success with holes), the reference/release rules (an entry is
// evicted only at refcount zero with no load in flight; a failed load is
// recorded for the requests that asked and never poisons a ready entry) and
// the cleanup discipline (a close is attempted only when the instance is
// empty; a failed prepare leaves no half-created instance registered).
enum class SceneCacheState { Missing, Loading, Ready };
enum class SceneInstanceState { Creating, Preparing, Ready, Failed };

inline const char* to_string(SceneInstanceState s) {
    switch (s) {
        case SceneInstanceState::Creating: return "creating";
        case SceneInstanceState::Preparing: return "preparing";
        case SceneInstanceState::Ready: return "ready";
        case SceneInstanceState::Failed: return "failed";
    }
    return "unknown";
}

class SceneRuntime {
public:
    struct Result {
        bool ok = true;
        std::string reason;
        // Non-explicit: the model's answers read directly in assertions and
        // conditionals, like a bool with a recorded reason.
        operator bool() const { return ok; }
    };

    explicit SceneRuntime(std::function<std::chrono::steady_clock::time_point()> clock = {})
        : clock_(clock ? std::move(clock) : [] { return std::chrono::steady_clock::now(); }) {}

    // --- Map definitions ----------------------------------------------------
    // Registered at startup from configuration or a repository. A changed
    // definition is a new digest and therefore a new key; registered
    // definitions are never mutated in place, so registering the same
    // (id, digest) again is the idempotent no-op (true: the definition is
    // registered after the call) and a changed digest simply coexists as
    // another key. Only an identity that cannot name a definition refuses.
    bool register_definition(const std::string& id, const std::string& digest) {
        if (id.empty() || digest.empty()) return false;
        definitions_.emplace(id, digest);
        return true;
    }
    bool definition_registered(const std::string& id, const std::string& digest) const {
        return definitions_.count(DefinitionKey{id, digest}) != 0;
    }

    // --- Process-local read-only spatial cache -------------------------------
    // Taking a reference is what makes an entry eligible to load, and the
    // reference lives exactly as long as the instance that took it. Acquiring
    // a Missing entry starts its load (Loading); acquiring a Loading or Ready
    // entry only counts the reference. An unknown definition has no source to
    // load from and is refused.
    Result acquire(const std::string& id, const std::string& digest) {
        if (!definition_registered(id, digest)) return {false, "unknown definition"};
        auto& entry = cache_[{id, digest}];
        ++entry.refs;
        if (entry.state == SceneCacheState::Missing) entry.state = SceneCacheState::Loading;
        return {};
    }
    SceneCacheState cache_state(const std::string& id, const std::string& digest) const {
        const auto it = cache_.find({id, digest});
        return it == cache_.end() ? SceneCacheState::Missing : it->second.state;
    }
    // The loader resolved the data: Loading -> Ready, immutable from now on.
    // Every instance preparing on this key lands ready — its definition
    // reference resolved. An entry whose references all left before the load
    // landed evicts immediately (eviction needs refcount zero and no load in
    // flight; the load is no longer in flight).
    Result resolve(const std::string& id, const std::string& digest) {
        const DefinitionKey k{id, digest};
        auto it = cache_.find(k);
        if (it == cache_.end() || it->second.state != SceneCacheState::Loading)
            return {false, "no load in flight"};
        it->second.state = SceneCacheState::Ready;
        promote(k);
        evict_unreferenced(k);
        return {};
    }
    // The loader failed. The failure is recorded for the requests that asked
    // — every instance preparing on this key fails and cleans up — never for
    // the key: it is Missing again, retryable by another instance's prepare,
    // and a Ready entry can never be poisoned (only a load in flight can
    // fail).
    Result fail(const std::string& id, const std::string& digest) {
        const DefinitionKey k{id, digest};
        auto it = cache_.find(k);
        if (it == cache_.end() || it->second.state != SceneCacheState::Loading)
            return {false, "no load in flight"};
        it->second.state = SceneCacheState::Missing;
        const auto waiting = preparing(k);
        evict_unreferenced(k);
        for (const auto& name : waiting) fail_instance(name, "definition load failed");
        return {};
    }
    // A reference leaves. The entry may be evicted only when its reference
    // count is zero and no load is in flight for the same key — a Loading
    // entry with no references still waits for its resolve/fail before it can
    // be dropped, so a late reference never resurrects freed data.
    void release(const std::string& id, const std::string& digest) {
        const DefinitionKey k{id, digest};
        auto it = cache_.find(k);
        if (it == cache_.end()) return;
        if (it->second.refs > 0) --it->second.refs;
        evict_unreferenced(k);
    }

    // --- Scene instances ------------------------------------------------------
    // creating -> preparing -> ready | failed (docs/scene-lifecycle.md "Scene
    // instance states"). Allocation lands `creating`; no participant may enter
    // until `ready`. An allocation that cannot even name a registered
    // definition is the creating -> failed edge. A duplicate name is refused.
    Result create(const std::string& name, const std::string& id, const std::string& digest,
                  std::uint64_t budget_ms) {
        if (name.empty()) return {false, "empty instance name"};
        if (instances_.count(name)) return {false, "instance already exists"};
        Instance in;
        in.definition = {id, digest};
        in.budget_ms = budget_ms;
        if (!definition_registered(id, digest)) {
            in.state = SceneInstanceState::Failed;
            in.failure = "unknown definition";
        }
        instances_.emplace(name, std::move(in));
        return {};
    }
    // creating -> preparing: the instance takes its definition reference,
    // which may trigger a spatial-cache load (a second instance of a
    // definition already Ready resolves immediately — no map data is
    // transferred and nothing is loaded again).
    Result begin_prepare(const std::string& name) {
        Instance* in = find(name);
        if (!in) return {false, "no such instance"};
        if (in->state != SceneInstanceState::Creating)
            return {false, std::string("instance is ") + to_string(in->state)};
        if (!definition_registered(in->definition.first, in->definition.second)) {
            fail_instance(name, "unknown definition");
            return {};
        }
        const auto acquired = acquire(in->definition.first, in->definition.second);
        if (!acquired) {
            fail_instance(name, acquired.reason);
            return {};
        }
        in->held = true;
        in->since = clock_();
        in->state = cache_state(in->definition.first, in->definition.second) == SceneCacheState::Ready
            ? SceneInstanceState::Ready
            : SceneInstanceState::Preparing;
        return {};
    }
    // The only state that accepts entry. There is no "ready with missing
    // data" state: every other state refuses with an explicit failure and
    // never accepts a participant to wait.
    Result enter(const std::string& name, std::uint64_t participant) {
        Instance* in = find(name);
        if (!in) return {false, "no such instance"};
        if (in->state != SceneInstanceState::Ready)
            return {false, std::string("instance is ") + to_string(in->state)};
        in->participants.insert(participant);
        return {};
    }
    Result leave(const std::string& name, std::uint64_t participant) {
        Instance* in = find(name);
        if (!in) return {false, "no such instance"};
        in->participants.erase(participant);
        return {};
    }
    // Node tick: a prepare that does not complete inside its budget is a
    // failure, not a success with holes — the instance lands failed and
    // cleaned up, and no path publishes a ready scene with data it does not
    // have.
    void tick() {
        std::vector<std::string> expired;
        const auto now = clock_();
        for (const auto& [name, in] : instances_)
            if (in.state == SceneInstanceState::Preparing
                && now - in.since > std::chrono::milliseconds(in.budget_ms))
                expired.push_back(name);
        for (const auto& name : expired) fail_instance(name, "prepare timed out");
    }
    // The ordered cleanup (stop accepting entry -> stop timers -> evacuate or
    // return every participant -> free instance state -> release the
    // definition reference): steps 1-3 are the closing side's discipline,
    // which the model enforces by refusing a close while a participant
    // remains — a temporary close is attempted only when the instance is
    // empty or after an explicit evacuation moved every participant out. Steps
    // 4-5 run here: the instance state goes and the definition reference is
    // released, with the cache deciding whether the entry can be evicted.
    // Neither idleness nor age closes anything: closing is an explicit
    // decision, and resident instances are excluded from it.
    Result close(const std::string& name) {
        Instance* in = find(name);
        if (!in) return {false, "no such instance"};
        if (!in->participants.empty()) return {false, "instance still has participants"};
        if (in->state == SceneInstanceState::Failed)
            return {false, "a failed instance is reaped, not closed"};
        release_definition(name, *in);
        instances_.erase(name);
        return {};
    }
    // The failure-path cleanup sweep: a failed prepare runs the same order
    // minus the participant steps (none entered), and must not leave a
    // half-created instance registered — its definition reference was already
    // released at the failure, so reaping only unregisters.
    void reap_failed() {
        for (auto it = instances_.begin(); it != instances_.end();) {
            if (it->second.state == SceneInstanceState::Failed) it = instances_.erase(it);
            else ++it;
        }
    }
    bool has(const std::string& name) const { return instances_.count(name) != 0; }
    SceneInstanceState state(const std::string& name) const {
        const auto it = instances_.find(name);
        return it == instances_.end() ? SceneInstanceState::Failed : it->second.state;
    }
    // The recorded failure reason (empty for an instance that never failed).
    std::string failure(const std::string& name) const {
        const auto it = instances_.find(name);
        return it == instances_.end() ? std::string() : it->second.failure;
    }
    std::size_t participants(const std::string& name) const {
        const auto it = instances_.find(name);
        return it == instances_.end() ? 0 : it->second.participants.size();
    }

private:
    using DefinitionKey = std::pair<std::string, std::string>; // (definition id, digest)
    struct CacheEntry {
        SceneCacheState state = SceneCacheState::Missing;
        std::size_t refs = 0;
    };
    struct Instance {
        SceneInstanceState state = SceneInstanceState::Creating;
        DefinitionKey definition;
        std::uint64_t budget_ms = 0;
        std::chrono::steady_clock::time_point since;
        bool held = false; // the definition reference begin_prepare took
        std::set<std::uint64_t> participants;
        std::string failure;
    };
    Instance* find(const std::string& name) {
        const auto it = instances_.find(name);
        return it == instances_.end() ? nullptr : &it->second;
    }
    // Eviction: reference-driven only. No memory threshold, timer or idle rule
    // evicts an entry that an instance still references, and a load in flight
    // holds the entry until its resolve/fail lands.
    void evict_unreferenced(const DefinitionKey& key) {
        const auto it = cache_.find(key);
        if (it == cache_.end()) return;
        if (it->second.refs == 0 && it->second.state != SceneCacheState::Loading) cache_.erase(it);
    }
    std::vector<std::string> preparing(const DefinitionKey& key) const {
        std::vector<std::string> out;
        for (const auto& [name, in] : instances_)
            if (in.state == SceneInstanceState::Preparing && in.definition == key)
                out.push_back(name);
        return out;
    }
    void promote(const DefinitionKey& key) {
        for (auto& [name, in] : instances_)
            if (in.state == SceneInstanceState::Preparing && in.definition == key)
                in.state = SceneInstanceState::Ready;
    }
    void release_definition(const std::string& name, Instance& in) {
        if (!in.held) return;
        in.held = false;
        release(in.definition.first, in.definition.second);
    }
    // A failed instance releases its definition reference at the failure (the
    // ordered cleanup's step 5 runs on the failure path too); the cache entry
    // may be evicted the moment its last reference leaves. The instance stays
    // registered as failed until reap_failed, so the failure is observable
    // before the half-created instance is unregistered.
    void fail_instance(const std::string& name, std::string reason) {
        Instance* in = find(name);
        if (!in || in->state == SceneInstanceState::Failed) return;
        in->failure = std::move(reason);
        in->participants.clear(); // none may have entered (only ready accepts)
        in->state = SceneInstanceState::Failed;
        release_definition(name, *in);
    }

    std::function<std::chrono::steady_clock::time_point()> clock_;
    std::set<DefinitionKey> definitions_;
    std::map<DefinitionKey, CacheEntry> cache_;
    std::map<std::string, Instance> instances_;
};

} // namespace mistlane
