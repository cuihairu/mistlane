// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
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
// may resume after a tombstone). Cross is a cross participation: the character
// visits this host, so the load resolves into `visiting` (the lease face), not
// `active` — the home authority stays at the origin realm.
enum class LoadSource { Home, TransferIn, Cross };

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
    // active only on success, failure -> closed, no visible avatar. `state` is
    // the loaded record (opaque bytes the home LOAD returned) or empty when no
    // record has landed yet.
    virtual bool on_load(const AvatarKey&, std::string_view state, std::string& avatar_ref) = 0;
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
    // active -> transferring edge; abort returns to active. The snapshot is the
    // avatar's final state as an opaque byte string (the same representation a
    // home LOAD carries to avatar_load) — the source caller submits it as the
    // final MLD1 change before the fence. `snapshot` is written only when the
    // hook accepts the transfer; an abort leaves it untouched.
    virtual bool on_transfer_out(const AvatarKey&, const std::string& avatar_ref,
                                 std::string& snapshot) = 0;
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
    // Cross-visit event hooks (docs/cross-server-lifecycle.md; the registry's
    // event family, module-table functions). These are the crossServer's
    // session-object face of the oracle, not oracle transitions — defaults are
    // vacuous so a host that does not declare them behaves as today.
    // onCrossVisitAdmit: after the network quotas and the verified ticket,
    // before the lease is granted; a denial refuses the join (fail closed).
    virtual bool on_cross_admit(const AvatarKey&) { return true; }
    // onCrossVisitStart: the visiting avatar was created (notifies, never vetoes).
    virtual void on_cross_start(const AvatarKey&) {}
    // onCrossVisitEnd: the visiting avatar closed (returned or terminated);
    // notifies, the close proceeds.
    virtual void on_cross_end(const AvatarKey&) {}
};

// The framework-side avatar runtime implementing the seven-state oracle of
// docs/avatar-lifecycle.md: one live avatar per character (single-writer), at
// most one bound per session per host, framework-driven transitions with hooks
// at the edges, and a total transition function — every event not listed for a
// state is refused with that state's rejection code and no state change. This
// is the model half; session lifecycle, tick and stop wiring is a later slice.
//
// The loading resolution splits by load source: a home load runs avatar_load
// and stops in loading (loaded, not yet entered) until the session attaches,
// which drives the avatar_enter edge to active; a transfer-in load runs
// avatar_load then avatar_transfer_in and is active (unbound) — the destination
// hosts it host-to-host; a cross visit (admit_visit) runs the admission gate,
// avatar_load, binds the joining session and lands visiting (the lease face —
// no avatar_enter, the framework's verified admission is the entry gate).
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
    // `scene` is the avatar's scene reference at landing (empty = unset): a
    // transfer-in arrives holding the scene its transfer's destination owner
    // named (seeded from the coordinator's DESTINED answer), a home login
    // arrives with none until the runtime assigns one.
    Result load(AvatarKey key, std::uint64_t grace_ms, LoadSource source = LoadSource::Home,
                std::string_view state = {}, std::string_view scene = {}) {
        if (!key.valid()) return {AvatarError::InvalidIdentity, "invalid avatar identity"};
        for (const auto& [k, x] : avatars_)
            if (k.realm == key.realm && k.character == key.character && x.state != AvatarState::Closed)
                return {AvatarError::Exists, "a live avatar already exists for this character"};
        avatars_.erase(key);
        Entry e;
        e.state = AvatarState::Loading;
        e.grace_ms = grace_ms;
        e.source = source;
        e.scene = std::string(scene);
        avatars_.emplace(key, std::move(e));
        return finish_load(key, state);
    }

    // Cross admission + visiting create, one step (docs/cross-server-lifecycle.md
    // "Create"): after the network quotas and the verified ticket — both already
    // settled by the session plane — the onCrossVisitAdmit gate runs (denied or
    // raising refuses the join, fail closed), then avatar_load creates the
    // visiting avatar, the joining session binds, and the avatar lands visiting
    // (the lease face). onCrossVisitStart notifies after the create. No
    // avatar_enter fires: the loading -> active edge is the home entry only. A
    // failed load publishes nothing and refuses the join.
    Result admit_visit(AvatarKey key, std::uint64_t session_id, std::uint64_t grace_ms) {
        if (session_id == 0) return {AvatarError::NoSession, "no session id"};
        if (!hooks_.on_cross_admit(key))
            return {AvatarError::EnterRefused, "onCrossVisitAdmit denied the cross visit"};
        auto loaded = load(key, grace_ms, LoadSource::Cross);
        if (!loaded.ok()) return loaded;
        Entry* e = find(key);
        bind(e, session_id);
        e->state = AvatarState::Visiting;
        hooks_.on_cross_start(key);
        return {AvatarError::Ok, ""};
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

    // Reclaim completion sweep (the node tick's second pass, after tick()):
    // every reclaiming avatar runs the ordered reclaim — the final avatar_save
    // (the single forced-save entry; docs/avatar-lifecycle.md segment 6) and
    // then closed. A failed final save keeps the avatar in reclaiming (retried
    // on the next sweep, E_SAVE_FAIL per the migration table); the operator
    // sees the failure through the save hook's own logging. This is the
    // framework driving the reclaim to completion, not the model-test manual
    // reclaim_done — the model's tick() only moves suspended to reclaiming so
    // the per-step contract stays testable.
    void sweep_reclaims() {
        std::vector<AvatarKey> reclaiming;
        for (const auto& [key, e] : avatars_)
            if (e.state == AvatarState::Reclaiming) reclaiming.push_back(key);
        for (const auto& key : reclaiming) reclaim_done(key);
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
    // returns to active. The snapshot is the hook's state snapshot (opaque
    // bytes), written only on acceptance — the source caller submits it as the
    // final change before the fence. A hook that accepts without producing a
    // snapshot leaves it empty, which the caller may refuse as malformed.
    Result transfer_out(AvatarKey key, std::string& snapshot) {
        Entry* e = find(key);
        if (!e) return {AvatarError::NoAvatar, "no avatar"};
        if (e->state != AvatarState::Active)
            return {state_refusal(e->state), std::string("avatar is ") + to_string(e->state)};
        e->state = AvatarState::Transferring;
        if (!hooks_.on_transfer_out(key, e->ref, snapshot)) {
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

    // Same-process scene switch (docs/avatar-runtime.md: the avatar stays
    // `active`; only its instance reference changes. No save, no state
    // round-trip — nothing travels, and the scene-lifecycle no-repeat rule
    // applies to the avatar too). This is runtime data, not an oracle
    // transition: only the scene reference changes, the state machine is
    // untouched. A non-active avatar has no hosted scene to move, so the
    // switch is refused with that state's rejection code.
    Result switch_scene(AvatarKey key, std::string_view scene) {
        Entry* e = find(key);
        if (!e) return {AvatarError::NoAvatar, "no avatar"};
        if (e->state != AvatarState::Active)
            return {state_refusal(e->state), std::string("avatar is ") + to_string(e->state)};
        e->scene = std::string(scene);
        return {AvatarError::Ok, ""};
    }

    // Abnormal crossServer termination (peer down, admission revoked, lease
    // lost): the visiting avatar closes here with a best-effort save;
    // onCrossVisitEnd notifies and the close proceeds. Unacknowledged changes
    // carry no durability claim — the sender queue owns them. Valid only in
    // visiting; a visiting avatar that already left cross scope belongs to the
    // return flow.
    Result close_visit(AvatarKey key) {
        Entry* e = find(key);
        if (!e) return {AvatarError::NoAvatar, "no avatar"};
        if (e->state != AvatarState::Visiting)
            return {state_refusal(e->state), std::string("avatar is ") + to_string(e->state)};
        hooks_.on_save(key, e->ref); // best effort; a failure does not block the close
        hooks_.on_cross_end(key);
        e->state = AvatarState::Closed;
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

    // E_RETURN_RELEASE: transferring -> closed (the source released). This is
    // the return flow's terminal on the crossServer: the visit relinquished the
    // lease (E_CROSS_LEAVE put it in transferring) and every change has since
    // been acknowledged durably, so the avatar is no longer a participant of
    // record and closes. Unlike close_visit (abnormal termination, best-effort
    // save) this runs no save — the durable changes already landed via MLD1 —
    // but fires onCrossVisitEnd for a cross-source avatar (the cross scope
    // ends here); a host-to-host transfer avatar (source home or transfer-in)
    // has no cross scope to end. Valid only in transferring; a transfer that
    // already aborted to active or was accepted to loading is not the source's.
    Result release(AvatarKey key) {
        Entry* e = find(key);
        if (!e) return {AvatarError::NoAvatar, "no avatar"};
        if (e->state != AvatarState::Transferring)
            return {state_refusal(e->state), std::string("avatar is ") + to_string(e->state)};
        if (e->source == LoadSource::Cross) hooks_.on_cross_end(key);
        e->state = AvatarState::Closed;
        return {AvatarError::Ok, ""};
    }

    // Discard a transfer-in avatar the host cannot keep (its READY was refused
    // and the gap was superseded): the incoming avatar closes on this host with
    // no save — nothing was dirtied between load and READY, and the durable
    // record stays at data. Only an active, unbound transfer-in avatar is
    // discarded; anything else is untouched.
    void discard(AvatarKey key) {
        Entry* e = find(key);
        if (!e || e->state != AvatarState::Active || e->source != LoadSource::TransferIn)
            return;
        e->state = AvatarState::Closed;
    }

    // E_DESTROY / E_OPERATOR_CLOSE: active or suspended -> reclaiming.
    Result reclaim(AvatarKey key) {        Entry* e = find(key);
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

    // Node retirement face (docs/avatar-runtime.md's reload rule): every avatar
    // in a replaced or stopping VM gets a best-effort avatar_save (valid states
    // only — a transferring avatar's final snapshot already left with
    // avatar_transfer_out, and a loading avatar never published) and then
    // closes. A visiting avatar that closes fires onCrossVisitEnd. This is the
    // framework retiring the whole VM, not a per-state lifecycle transition, so
    // no edge is driven; the dirty claim of an unsaved record belongs to the
    // sender queue and the data side.
    void retire_all() {
        for (auto& [key, e] : avatars_) {
            if (e.state == AvatarState::Active || e.state == AvatarState::Suspended ||
                e.state == AvatarState::Visiting || e.state == AvatarState::Reclaiming)
                hooks_.on_save(key, e.ref);
            if (e.state == AvatarState::Visiting) hooks_.on_cross_end(key);
            e.state = AvatarState::Closed;
        }
    }

    // avatar_fail: best effort, valid in any state; the original consequence
    // stands.
    void fail(AvatarKey key, std::string_view reason) {
        Entry* e = find(key);
        if (e) hooks_.on_fail(key, e->ref, reason);
    }

    bool has(const AvatarKey& key) const { return avatars_.count(key) != 0; }
    // The live (non-closed) avatar for a character, if any. A script transfer
    // request names only realm + character, so the caller recovers the
    // generation the oracle keys on.
    std::optional<AvatarKey> live_key(std::string_view realm, std::string_view character) const {
        for (const auto& [k, e] : avatars_)
            if (k.realm == realm && k.character == character && e.state != AvatarState::Closed)
                return k;
        return std::nullopt;
    }
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
    // The avatar's scene reference (empty when unset or absent).
    std::string scene(const AvatarKey& key) const {
        const Entry* e = find(key);
        return e ? e->scene : std::string();
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
        // The avatar's scene reference (docs/avatar-runtime.md "Same-process
        // scene switch": the instance reference the avatar holds; empty when
        // unset). This is runtime data, not an oracle transition — the switch
        // keeps the avatar active and changes only this reference.
        std::string scene;
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
    // failure closes without publishing. `state` is the loaded record handed to
    // avatar_load (empty when none landed).
    Result finish_load(AvatarKey key, std::string_view state = {}) {
        Entry* e = find(key);
        std::string ref;
        if (!hooks_.on_load(key, state, ref)) {
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
