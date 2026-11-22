// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace mistlane {

// Avatar identity deliberately reuses the participant-context tuple so one
// shape crosses the MLC1 handshake, the routing record and the MLD1
// submission without translation (docs/avatar-lifecycle.md "Object split";
// the tuple is the same as the cross-return-protocol participant context).
struct AvatarKey {
    std::string realm, character;
    std::uint64_t generation = 0;
    bool operator<(const AvatarKey& o) const {
        if (realm != o.realm) return realm < o.realm;
        if (character != o.character) return character < o.character;
        return generation < o.generation;
    }
    bool valid() const { return !realm.empty() && !character.empty() && generation != 0; }
    bool operator==(const AvatarKey&) const = default;
};

// docs/avatar-lifecycle.md "Oracle state machine": the seven states of the
// expanded oracle. `closing` of the five-state page is superseded by
// `reclaiming` (corrigendum C1); `visiting` is the cross-participation lease
// face (C2). Only the framework moves an avatar between states.
enum class AvatarState { Loading, Active, Suspended, Visiting, Transferring, Reclaiming, Closed };

inline const char* to_string(AvatarState s) {
    switch (s) {
        case AvatarState::Loading: return "loading";
        case AvatarState::Active: return "active";
        case AvatarState::Suspended: return "suspended";
        case AvatarState::Visiting: return "visiting";
        case AvatarState::Transferring: return "transferring";
        case AvatarState::Reclaiming: return "reclaiming";
        case AvatarState::Closed: return "closed";
    }
    return "unknown";
}

// The rejection codes the oracle's transition function names per state
// (docs/avatar-lifecycle.md "Authoritative migration table"): a refused event
// halts with the state's code and no state change. Operational errors (no such
// avatar, duplicate spawn, hook failure) are the runtime's own and do not
// correspond to a state refusal.
enum class AvatarError {
    Ok = 0,
    Loading,        // AVATAR_LOADING: handler targeted a loading avatar
    AlreadyAttached,// AVATAR_ALREADY_ATTACHED: duplicate attach on active
    Suspended,      // AVATAR_SUSPENDED: participation refused while suspended
    Visiting,       // AVATAR_VISITING: durable write refused while visiting
    Transferring,   // AVATAR_TRANSFERRING: participation refused mid-transfer
    Reclaiming,     // AVATAR_RECLAIMING: participation refused during reclaim
    Closed,         // AVATAR_CLOSED: event on a destroyed avatar
    // Operational errors.
    NoAvatar, InvalidIdentity, Exists, NoSession,
    LoadFailed, EnterRefused, SaveFailed, TransferAborted, TransferInFailed,
};

inline const char* to_string(AvatarError e) {
    switch (e) {
        case AvatarError::Ok: return "ok";
        case AvatarError::Loading: return "AVATAR_LOADING";
        case AvatarError::AlreadyAttached: return "AVATAR_ALREADY_ATTACHED";
        case AvatarError::Suspended: return "AVATAR_SUSPENDED";
        case AvatarError::Visiting: return "AVATAR_VISITING";
        case AvatarError::Transferring: return "AVATAR_TRANSFERRING";
        case AvatarError::Reclaiming: return "AVATAR_RECLAIMING";
        case AvatarError::Closed: return "AVATAR_CLOSED";
        case AvatarError::NoAvatar: return "no avatar";
        case AvatarError::InvalidIdentity: return "invalid avatar identity";
        case AvatarError::Exists: return "avatar already exists";
        case AvatarError::NoSession: return "no session id";
        case AvatarError::LoadFailed: return "avatar_load failed";
        case AvatarError::EnterRefused: return "avatar_enter refused";
        case AvatarError::SaveFailed: return "avatar_save failed";
        case AvatarError::TransferAborted: return "avatar_transfer_out failed";
        case AvatarError::TransferInFailed: return "avatar_transfer_in failed";
    }
    return "unknown";
}

// Where a loading avatar came from (docs/avatar-lifecycle.md: the load source
// attribute the transition function reads to decide whether a new generation
// may resume after a tombstone).
enum class LoadSource { Home, TransferIn };

// The Lua hook surface (docs/avatar-runtime.md "The Lua hook surface",
// unchanged in signature and failure semantics; docs/avatar-lifecycle.md
// "Hook surface per state" assigns which state each hook is valid in). The
// script-backed implementation is a later slice (wired through LuaService).
// The opaque `avatar_ref` is the script's avatar table handle, returned by
// on_load and carried back so the same avatar reaches every subsequent hook.
class AvatarHooks {
public:
    virtual ~AvatarHooks() = default;
    // avatar_load (context, state) -> avatar table. Valid in loading; entry to
    // active only on success, failure -> closed, no visible avatar.
    virtual bool on_load(const AvatarKey&, std::string& avatar_ref) = 0;
    // avatar_enter (context). Valid at the loading -> active edge; entry
    // refused with an explicit reason.
    virtual bool on_enter(const AvatarKey&, const std::string& avatar_ref, std::string& reason) = 0;
    // avatar_leave (context). Valid at active -> suspended and
    // visiting -> transferring; never blocks the detach.
    virtual void on_leave(const AvatarKey&, const std::string& avatar_ref) = 0;
    // avatar_tick (context). Valid in active and visiting (cross-local only
    // when visiting); logged, the avatar stays.
    virtual void on_tick(const AvatarKey&, const std::string& avatar_ref) = 0;
    // avatar_save (context) -> state table. The single forced-save entry; valid
    // in active, suspended, visiting and reclaiming. Nothing else clears dirty
    // state or triggers a save by side effect.
    virtual bool on_save(const AvatarKey&, const std::string& avatar_ref) = 0;
    // avatar_transfer_out (context) -> state snapshot. Valid at the
    // active -> transferring edge; abort returns to active.
    virtual bool on_transfer_out(const AvatarKey&, const std::string& avatar_ref) = 0;
    // avatar_transfer_in (context, state). Valid at the loading (transfer-in)
    // edge; failure closes on this host and the source keeps its avatar.
    virtual bool on_transfer_in(const AvatarKey&, const std::string& avatar_ref) = 0;
    // avatar_detach (context). Valid at the active -> suspended edge; logged,
    // the avatar goes suspended regardless.
    virtual void on_detach(const AvatarKey&, const std::string& avatar_ref) = 0;
    // avatar_fail (context, reason). Valid in any state; best effort — it may
    // not run during a VM replacement, and its own failure is logged with the
    // original standing.
    virtual void on_fail(const AvatarKey&, const std::string& avatar_ref, std::string_view reason) = 0;
};

// The framework-side avatar runtime implementing the seven-state oracle of
// docs/avatar-lifecycle.md: one live avatar per character (single-writer), at
// most one bound per session per host, framework-driven transitions with hooks
// at the edges, and a total transition function — every event not listed for a
// state is refused with that state's rejection code and no state change. This
// is the model half; session lifecycle, tick and stop wiring is a later slice.
//
// The loading -> active edge splits by load source: a home load runs
// avatar_load and stops in loading (loaded, not yet entered) until the session
// attaches; a transfer-in load runs avatar_load then avatar_transfer_in and is
// active (unbound) — the destination hosts it host-to-host.
class AvatarRuntime {
public:
    struct Result {
        AvatarError code = AvatarError::Ok;
        std::string reason;
        bool ok() const { return code == AvatarError::Ok; }
    };

    explicit AvatarRuntime(AvatarHooks& hooks, std::function<std::chrono::steady_clock::time_point()> clock = {})
        : hooks_(hooks), clock_(clock ? std::move(clock) : [] { return std::chrono::steady_clock::now(); }) {}

    // E_SPAWN/E_LOAD: creates an avatar at this host and runs avatar_load. A
    // failed load publishes no visible avatar (closed); a character that
    // already holds a live avatar (any state but closed) is refused — one
    // active per character. Home loads stop in loading until attach(); a
    // transfer-in load completes to active (unbound) via avatar_transfer_in.
    Result load(AvatarKey key, std::uint64_t grace_ms, LoadSource source = LoadSource::Home) {
        if (!key.valid()) return {AvatarError::InvalidIdentity, "invalid avatar identity"};
        for (const auto& [k, x] : avatars_)
            if (k.realm == key.realm && k.character == key.character && x.state != AvatarState::Closed)
                return {AvatarError::Exists, "a live avatar already exists for this character"};
        avatars_.erase(key);
        Entry e;
        e.state = AvatarState::Loading;
        e.grace_ms = grace_ms;
        e.source = source;
        avatars_.emplace(key, std::move(e));
        return finish_load(key);
    }

    // E_ATTACH: binds a session to an avatar. On loading this drives the
    // avatar_enter edge to active; an active but unbound avatar (a transfer-in
    // landing) binds; a suspended avatar within grace reattaches to active
    // without a reload. Every refusal carries the state's rejection code and
    // a second attach on an already-bound active avatar refuses
    // AVATAR_ALREADY_ATTACHED.
    Result attach(AvatarKey key, std::uint64_t session_id) {
        if (session_id == 0) return {AvatarError::NoSession, "no session id"};
        Entry* e = find(key);
        if (!e) return {AvatarError::NoAvatar, "no avatar"};
        if (session_bound_elsewhere(session_id)) return {AvatarError::AlreadyAttached, "session already bound"};
        switch (e->state) {
            case AvatarState::Loading: {
                std::string reason;
                if (!hooks_.on_enter(key, e->ref, reason)) {
                    e->state = AvatarState::Closed;
                    return {AvatarError::EnterRefused, reason.empty() ? "avatar_enter refused" : reason};
                }
                bind(e, session_id);
                e->state = AvatarState::Active;
                return {AvatarError::Ok, ""};
            }
            case AvatarState::Active:
                if (e->bound) return {AvatarError::AlreadyAttached, "avatar already attached"};
                bind(e, session_id);
                return {AvatarError::Ok, ""};
            case AvatarState::Suspended:
                if (clock_() - e->suspended_at > std::chrono::milliseconds(e->grace_ms)) {
                    e->state = AvatarState::Reclaiming;
                    return {AvatarError::Suspended, "avatar grace period expired"};
                }
                if (e->bound) return {AvatarError::AlreadyAttached, "avatar already attached"};
                bind(e, session_id);
                e->state = AvatarState::Active;
                return {AvatarError::Ok, ""};
            default:
                return {state_refusal(e->state), std::string("avatar is ") + to_string(e->state)};
        }
    }

    // E_DETACH: detaches the bound session. active -> suspended runs
    // avatar_leave then avatar_detach (the grace period starts); visiting ->
    // transferring runs avatar_leave. A session that is not this avatar's
    // binding is a no-op.
    void detach(AvatarKey key, std::uint64_t session_id) {
        Entry* e = find(key);
        if (!e || !e->bound || e->session != session_id) return;
        hooks_.on_leave(key, e->ref);
        if (e->state == AvatarState::Active) {
            hooks_.on_detach(key, e->ref);
            e->bound = false;
            e->session = 0;
            e->state = AvatarState::Suspended;
            e->suspended_at = clock_();
        } else if (e->state == AvatarState::Visiting) {
            e->bound = false;
            e->session = 0;
            e->state = AvatarState::Transferring;
        }
    }

    // Node tick: avatar_tick for every active and visiting avatar, then moves
    // any suspended avatar whose grace period expired to reclaiming.
    void tick() {
        for (auto& [key, e] : avatars_)
            if (e.state == AvatarState::Active || e.state == AvatarState::Visiting)
                hooks_.on_tick(key, e.ref);
        std::vector<AvatarKey> expired;
        for (const auto& [key, e] : avatars_)
            if (e.state == AvatarState::Suspended &&
                clock_() - e.suspended_at > std::chrono::milliseconds(e.grace_ms))
                expired.push_back(key);
        for (const auto& key : expired) find(key)->state = AvatarState::Reclaiming;
    }

    // avatar_save: the single forced-save entry, valid in active, suspended,
    // visiting and reclaiming. In reclaiming the save must be durable before
    // closed; a failure stays in reclaiming (retry or surfaced).
    Result save(AvatarKey key) {
        Entry* e = find(key);
        if (!e) return {AvatarError::NoAvatar, "no avatar"};
        if (e->state != AvatarState::Active && e->state != AvatarState::Suspended &&
            e->state != AvatarState::Visiting && e->state != AvatarState::Reclaiming)
            return {state_refusal(e->state), std::string("avatar is ") + to_string(e->state)};
        if (!hooks_.on_save(key, e->ref)) return {AvatarError::SaveFailed, "avatar_save failed"};
        return {AvatarError::Ok, ""};
    }

    // E_TRANSFER_REQ: active -> transferring (avatar_transfer_out); an abort
    // returns to active.
    Result transfer_out(AvatarKey key) {
        Entry* e = find(key);
        if (!e) return {AvatarError::NoAvatar, "no avatar"};
        if (e->state != AvatarState::Active)
            return {state_refusal(e->state), std::string("avatar is ") + to_string(e->state)};
        e->state = AvatarState::Transferring;
        if (!hooks_.on_transfer_out(key, e->ref)) {
            e->state = AvatarState::Active;
            return {AvatarError::TransferAborted, "avatar_transfer_out failed: transfer aborted"};
        }
        return {AvatarError::Ok, ""};
    }

    // E_TRANSFER_ABORT: transferring -> active (pre-tombstone failure).
    void transfer_abort(AvatarKey key) {
        Entry* e = find(key);
        if (e && e->state == AvatarState::Transferring) e->state = AvatarState::Active;
    }

    // E_TOMBSTONE_ACCEPT: the destination accepted. The source avatar leaves
    // transferring and reloads as a transfer-in (loading source transfer-in),
    // running avatar_load then avatar_transfer_in. E_TRANSFER_ABORT is the
    // rejected half (transfer_abort).
    Result transfer_accepted(AvatarKey key) {
        Entry* e = find(key);
        if (!e) return {AvatarError::NoAvatar, "no avatar"};
        if (e->state != AvatarState::Transferring)
            return {state_refusal(e->state), std::string("avatar is ") + to_string(e->state)};
        e->state = AvatarState::Loading;
        e->source = LoadSource::TransferIn;
        return finish_load(key);
    }

    // E_CROSS_ACCEPT: active -> visiting (cross participation admitted).
    Result cross_accept(AvatarKey key) {
        Entry* e = find(key);
        if (!e) return {AvatarError::NoAvatar, "no avatar"};
        if (e->state != AvatarState::Active)
            return {state_refusal(e->state), std::string("avatar is ") + to_string(e->state)};
        e->state = AvatarState::Visiting;
        return {AvatarError::Ok, ""};
    }

    // E_CROSS_LEAVE: visiting -> transferring (the lease is relinquished).
    Result cross_leave(AvatarKey key) {
        Entry* e = find(key);
        if (!e) return {AvatarError::NoAvatar, "no avatar"};
        if (e->state != AvatarState::Visiting)
            return {state_refusal(e->state), std::string("avatar is ") + to_string(e->state)};
        e->state = AvatarState::Transferring;
        return {AvatarError::Ok, ""};
    }

    // E_RETURN_COMPLETE: visiting -> active (the change-set was acknowledged).
    Result return_complete(AvatarKey key) {
        Entry* e = find(key);
        if (!e) return {AvatarError::NoAvatar, "no avatar"};
        if (e->state != AvatarState::Visiting)
            return {state_refusal(e->state), std::string("avatar is ") + to_string(e->state)};
        e->state = AvatarState::Active;
        return {AvatarError::Ok, ""};
    }

    // E_DESTROY / E_OPERATOR_CLOSE: active or suspended -> reclaiming.
    Result reclaim(AvatarKey key) {
        Entry* e = find(key);
        if (!e) return {AvatarError::NoAvatar, "no avatar"};
        if (e->state != AvatarState::Active && e->state != AvatarState::Suspended)
            return {state_refusal(e->state), std::string("avatar is ") + to_string(e->state)};
        e->state = AvatarState::Reclaiming;
        return {AvatarError::Ok, ""};
    }

    // Reclaim done: the ordered reclaim (final save, scene release, AOI leave,
    // ref release) finished. The final avatar_save must succeed before closed;
    // E_SAVE_FAIL keeps the avatar in reclaiming.
    Result reclaim_done(AvatarKey key) {
        Entry* e = find(key);
        if (!e) return {AvatarError::NoAvatar, "no avatar"};
        if (e->state != AvatarState::Reclaiming)
            return {state_refusal(e->state), std::string("avatar is ") + to_string(e->state)};
        if (!hooks_.on_save(key, e->ref)) return {AvatarError::SaveFailed, "final save failed; still reclaiming"};
        e->state = AvatarState::Closed;
        return {AvatarError::Ok, ""};
    }

    // avatar_fail: best effort, valid in any state; the original consequence
    // stands.
    void fail(AvatarKey key, std::string_view reason) {
        Entry* e = find(key);
        if (e) hooks_.on_fail(key, e->ref, reason);
    }

    bool has(const AvatarKey& key) const { return avatars_.count(key) != 0; }
    AvatarState state(const AvatarKey& key) const {
        const Entry* e = find(key);
        return e ? e->state : AvatarState::Closed;
    }
    LoadSource source(const AvatarKey& key) const {
        const Entry* e = find(key);
        return e ? e->source : LoadSource::Home;
    }
    std::uint64_t session_of(const AvatarKey& key) const {
        const Entry* e = find(key);
        return e ? e->session : 0;
    }
    std::size_t active_count() const {
        std::size_t n = 0;
        for (const auto& [k, e] : avatars_) if (e.state == AvatarState::Active) ++n;
        return n;
    }

private:
    struct Entry {
        std::string ref;
        AvatarState state = AvatarState::Loading;
        LoadSource source = LoadSource::Home;
        std::uint64_t session = 0;
        bool bound = false;
        std::uint64_t grace_ms = 0;
        std::chrono::steady_clock::time_point suspended_at;
    };
    Entry* find(const AvatarKey& key) {
        auto it = avatars_.find(key);
        return it == avatars_.end() ? nullptr : &it->second;
    }
    const Entry* find(const AvatarKey& key) const {
        auto it = avatars_.find(key);
        return it == avatars_.end() ? nullptr : &it->second;
    }
    bool session_bound_elsewhere(std::uint64_t session) const {
        for (const auto& [k, x] : avatars_) if (x.bound && x.session == session) return true;
        return false;
    }
    static AvatarError state_refusal(AvatarState s) {
        switch (s) {
            case AvatarState::Loading: return AvatarError::Loading;
            case AvatarState::Active: return AvatarError::AlreadyAttached;
            case AvatarState::Suspended: return AvatarError::Suspended;
            case AvatarState::Visiting: return AvatarError::Visiting;
            case AvatarState::Transferring: return AvatarError::Transferring;
            case AvatarState::Reclaiming: return AvatarError::Reclaiming;
            case AvatarState::Closed: return AvatarError::Closed;
        }
        return AvatarError::Closed;
    }
    static void bind(Entry* e, std::uint64_t session) {
        e->bound = true;
        e->session = session;
    }
    // avatar_load, then the load-source completion: a transfer-in load runs
    // avatar_transfer_in and lands active (unbound); a home load stops in
    // loading (loaded, not yet entered) and waits for the session attach. A
    // failure closes without publishing.
    Result finish_load(AvatarKey key) {
        Entry* e = find(key);
        std::string ref;
        if (!hooks_.on_load(key, ref)) {
            e->state = AvatarState::Closed;
            return {AvatarError::LoadFailed, "avatar_load failed: no avatar published"};
        }
        e->ref = std::move(ref);
        if (e->source == LoadSource::TransferIn) {
            if (!hooks_.on_transfer_in(key, e->ref)) {
                e->state = AvatarState::Closed;
                return {AvatarError::TransferInFailed, "avatar_transfer_in failed: avatar closed"};
            }
            e->state = AvatarState::Active;
        }
        return {AvatarError::Ok, ""};
    }

    AvatarHooks& hooks_;
    std::function<std::chrono::steady_clock::time_point()> clock_;
    std::map<AvatarKey, Entry> avatars_;
};

} // namespace mistlane
