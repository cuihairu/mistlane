// SPDX-License-Identifier: Apache-2.0
#include "mistlane/avatar_runtime.hpp"
#include "mistlane/lua_service.hpp"
#include "test_common.hpp"
namespace mistlane::test {
namespace {

// Recording sink: every hook appends one line, failure switches are togglable
// per scenario, and the same avatar_ref must come back in every later hook.
struct RecordingHooks final : AvatarHooks {
    std::vector<std::string> calls;
    bool fail_load = false, fail_enter = false, fail_save = false;
    bool fail_transfer_out = false, fail_transfer_in = false;
    std::string ref = "avatar-table";
    bool on_load(const AvatarKey&, std::string_view state, std::string& out) override {
        calls.push_back(state.empty() ? "load" : "load:" + std::string(state));
        out = ref;
        return !fail_load;
    }
    bool on_enter(const AvatarKey&, const std::string& r, std::string& reason) override {
        calls.push_back("enter:" + r);
        if (fail_enter) { reason = "entry denied"; return false; }
        return true;
    }
    void on_leave(const AvatarKey&, const std::string& r) override { calls.push_back("leave:" + r); }
    void on_tick(const AvatarKey&, const std::string& r) override { calls.push_back("tick:" + r); }
    bool on_save(const AvatarKey&, const std::string& r) override {
        calls.push_back("save:" + r);
        return !fail_save;
    }
    bool on_transfer_out(const AvatarKey&, const std::string& r) override {
        calls.push_back("transfer_out:" + r);
        return !fail_transfer_out;
    }
    bool on_transfer_in(const AvatarKey&, const std::string& r) override {
        calls.push_back("transfer_in:" + r);
        return !fail_transfer_in;
    }
    void on_detach(const AvatarKey&, const std::string& r) override { calls.push_back("detach:" + r); }
    void on_fail(const AvatarKey&, const std::string&, std::string_view why) override {
        calls.push_back("fail:" + std::string(why));
    }
    bool saw(const std::string& name) const {
        for (const auto& c : calls)
            if (c == name || c.rfind(name + ":", 0) == 0) return true;
        return false;
    }
    std::size_t count(const std::string& name) const {
        std::size_t n = 0;
        for (const auto& c : calls)
            if (c == name || c.rfind(name + ":", 0) == 0) ++n;
        return n;
    }
};

const AvatarKey key{"realm-a", "hero", 7};
const AvatarKey same_character{"realm-a", "hero", 9};
const AvatarKey other{"realm-a", "rival", 3};

// Deterministic injected clock: tests advance time explicitly.
struct FakeClock {
    std::chrono::steady_clock::time_point now{};
    std::function<std::chrono::steady_clock::time_point()> fn() { return [this] { return now; }; }
};

} // namespace

// The seven-state oracle of docs/avatar-lifecycle.md as an in-process model:
// every accepted transition, every refusal code, and the hook edges. A fake
// sink records the calls; a fake clock drives grace expiry deterministically.
void avatar_lifecycle() {
    // --- Home load -> loading -> attach (avatar_enter) -> active ------------
    {
        RecordingHooks hooks;
        FakeClock clock;
        AvatarRuntime runtime(hooks, clock.fn());
        auto r = runtime.load(key, 100, LoadSource::Home);
        require(r.ok(), "home load refused: " + r.reason);
        require(runtime.state(key) == AvatarState::Loading, "a loaded home avatar is not in loading");
        require(!hooks.saw("enter"), "avatar_enter ran before the session attached");
        r = runtime.attach(key, 7);
        require(r.ok(), "attach refused: " + r.reason);
        require(runtime.state(key) == AvatarState::Active, "the entered avatar is not active");
        require(runtime.session_of(key) == 7, "the session was not bound");
        require(hooks.calls.size() == 2 && hooks.calls[0] == "load" && hooks.calls[1] == "enter:avatar-table",
                "the load edge did not run avatar_load then avatar_enter with the carried ref");

        // A duplicate attach refuses AVATAR_ALREADY_ATTACHED.
        r = runtime.attach(key, 8);
        require(r.code == AvatarError::AlreadyAttached, "duplicate attach did not refuse ALREADY_ATTACHED");
        require(runtime.state(key) == AvatarState::Active, "a refusal moved the avatar");

        // Tick reaches active avatars only.
        runtime.tick();
        require(hooks.count("tick") == 1, "tick did not reach the active avatar");

        // save is the single forced-save entry and works while active.
        r = runtime.save(key);
        require(r.ok(), "active save refused: " + r.reason);
        std::cout << "avatar.lifecycle home_load: load -> enter -> active, already-attached refused [PASS]\n";
    }

    // --- Loaded record reaches avatar_load -----------------------------------
    {
        RecordingHooks hooks;
        FakeClock clock;
        AvatarRuntime runtime(hooks, clock.fn());
        auto r = runtime.load(key, 100, LoadSource::Home, "hp=10 mp=5");
        require(r.ok(), "record load refused: " + r.reason);
        require(hooks.saw("load:hp=10 mp=5"), "avatar_load did not receive the loaded record");
        // An empty state stays the record-less load.
        r = runtime.load(other, 100);
        require(r.ok(), "recordless load refused: " + r.reason);
        require(hooks.saw("load"), "a recordless load passed a non-empty state");
        std::cout << "avatar.lifecycle load_state: record reaches avatar_load [PASS]\n";
    }

    // --- Load failure: no visible avatar ------------------------------------
    {
        RecordingHooks hooks;
        hooks.fail_load = true;
        FakeClock clock;
        AvatarRuntime runtime(hooks, clock.fn());
        auto r = runtime.load(key, 100);
        require(r.code == AvatarError::LoadFailed, "a failed load did not report LoadFailed");
        require(runtime.state(key) == AvatarState::Closed, "a failed load did not close");
        require(!hooks.saw("enter"), "a failed load still ran avatar_enter");
        r = runtime.attach(key, 7);
        require(r.code == AvatarError::Closed, "attach on a load-failed avatar did not refuse AVATAR_CLOSED");
        // E_SPAWN from closed: a fresh load of the same character is allowed.
        hooks.fail_load = false;
        r = runtime.load(key, 100);
        require(r.ok(), "re-spawn from closed refused: " + r.reason);
        std::cout << "avatar.lifecycle load_failed: closed, no publish, re-spawn allowed [PASS]\n";
    }

    // --- Enter refusal: entry denied, no visible avatar ---------------------
    {
        RecordingHooks hooks;
        hooks.fail_enter = true;
        FakeClock clock;
        AvatarRuntime runtime(hooks, clock.fn());
        require(runtime.load(key, 100).ok(), "load refused");
        auto r = runtime.attach(key, 7);
        require(r.code == AvatarError::EnterRefused && r.reason == "entry denied",
                "enter refusal did not surface its reason");
        require(runtime.state(key) == AvatarState::Closed, "a refused entry did not close");
        require(!hooks.saw("tick"), "a refused entry published a tickable avatar");
        std::cout << "avatar.lifecycle enter_refused: explicit reason, closed [PASS]\n";
    }

    // --- Single-writer: one live avatar per character ------------------------
    {
        RecordingHooks hooks;
        FakeClock clock;
        AvatarRuntime runtime(hooks, clock.fn());
        require(runtime.load(key, 100).ok() && runtime.attach(key, 7).ok(), "first load/attach failed");
        auto r = runtime.load(same_character, 100);
        require(r.code == AvatarError::Exists, "a second generation of a live character was accepted");
        // A different character is fine; one session per host refuses a second bind.
        require(runtime.load(other, 100).ok(), "a different character was refused");
        r = runtime.attach(other, 7);
        require(r.code == AvatarError::AlreadyAttached, "a session bound twice on one host was accepted");
        std::cout << "avatar.lifecycle single_writer: exists refused, session bound once [PASS]\n";
    }

    // --- Detach -> suspended: pending save obligations still run -------------
    {
        RecordingHooks hooks;
        FakeClock clock;
        AvatarRuntime runtime(hooks, clock.fn());
        require(runtime.load(key, 100).ok() && runtime.attach(key, 7).ok(), "setup failed");
        runtime.detach(key, 7);
        require(runtime.state(key) == AvatarState::Suspended, "detach did not suspend");
        require(hooks.calls.size() == 4 && hooks.calls[2] == "leave:avatar-table" &&
                hooks.calls[3] == "detach:avatar-table",
                "detach did not run avatar_leave then avatar_detach");
        runtime.detach(key, 99);  // not this avatar's binding: no-op
        require(hooks.calls.size() == 4, "a foreign session detach ran hooks");
        auto r = runtime.save(key);
        require(r.ok(), "suspended save (pending obligation) refused: " + r.reason);
        // Suspended never accepts new participation; ticks do not run.
        runtime.tick();
        require(!hooks.saw("tick"), "a suspended avatar was ticked");
        std::cout << "avatar.lifecycle detach: leave+detach, suspended saves still run [PASS]\n";
    }

    // --- Reattach within grace -> active without a reload --------------------
    {
        RecordingHooks hooks;
        FakeClock clock;
        AvatarRuntime runtime(hooks, clock.fn());
        require(runtime.load(key, 100).ok() && runtime.attach(key, 7).ok(), "setup failed");
        runtime.detach(key, 7);
        const std::size_t before = hooks.calls.size();
        const std::size_t loads = hooks.count("load");
        clock.now += std::chrono::milliseconds(50);
        auto r = runtime.attach(key, 21);
        require(r.ok(), "reattach within grace refused: " + r.reason);
        require(runtime.state(key) == AvatarState::Active && runtime.session_of(key) == 21,
                "reattach did not return to active with the new session");
        require(hooks.calls.size() == before, "reattach reloaded the avatar (extra hooks ran)");
        require(hooks.count("load") == loads, "reattach ran avatar_load again");
        std::cout << "avatar.lifecycle reattach: grace window, no reload [PASS]\n";
    }

    // --- Grace expiry -> reclaiming -> closed (final save must succeed) ------
    {
        RecordingHooks hooks;
        FakeClock clock;
        AvatarRuntime runtime(hooks, clock.fn());
        require(runtime.load(key, 100).ok() && runtime.attach(key, 7).ok(), "setup failed");
        runtime.detach(key, 7);
        clock.now += std::chrono::milliseconds(150);
        runtime.tick();
        require(runtime.state(key) == AvatarState::Reclaiming, "grace expiry did not move to reclaiming");
        auto r = runtime.attach(key, 21);
        require(r.code == AvatarError::Reclaiming, "attach on reclaiming did not refuse AVATAR_RECLAIMING");
        hooks.fail_save = true;
        r = runtime.reclaim_done(key);
        require(r.code == AvatarError::SaveFailed, "a failed final save closed the avatar");
        require(runtime.state(key) == AvatarState::Reclaiming, "E_SAVE_FAIL left reclaiming");
        hooks.fail_save = false;
        r = runtime.reclaim_done(key);
        require(r.ok(), "reclaim done refused: " + r.reason);
        require(runtime.state(key) == AvatarState::Closed, "reclaim done did not close");
        require(hooks.count("save") >= 2, "the final save did not run");
        std::cout << "avatar.lifecycle grace_expiry: reclaiming, save-fail retry, closed [PASS]\n";
    }

    // --- Reclaim sweep completes the ordered reclaim --------------------------
    {
        RecordingHooks hooks;
        FakeClock clock;
        AvatarRuntime runtime(hooks, clock.fn());
        require(runtime.load(key, 100).ok() && runtime.attach(key, 7).ok(), "setup failed");
        runtime.detach(key, 7);
        clock.now += std::chrono::milliseconds(150);
        runtime.tick();  // grace expired -> reclaiming
        require(runtime.state(key) == AvatarState::Reclaiming, "grace expiry did not move to reclaiming");
        runtime.sweep_reclaims();
        require(runtime.state(key) == AvatarState::Closed, "the sweep did not close the reclaimed avatar");
        require(hooks.count("save") >= 1, "the final save did not run");
        std::cout << "avatar.lifecycle reclaim_sweep: final save then closed [PASS]\n";
    }
    {
        RecordingHooks hooks;
        FakeClock clock;
        AvatarRuntime runtime(hooks, clock.fn());
        require(runtime.load(key, 100).ok() && runtime.attach(key, 7).ok(), "setup failed");
        runtime.detach(key, 7);
        clock.now += std::chrono::milliseconds(150);
        runtime.tick();
        hooks.fail_save = true;
        runtime.sweep_reclaims();
        require(runtime.state(key) == AvatarState::Reclaiming, "E_SAVE_FAIL left reclaiming");
        hooks.fail_save = false;
        runtime.sweep_reclaims();
        require(runtime.state(key) == AvatarState::Closed, "the retry sweep did not close");
        std::cout << "avatar.lifecycle reclaim_sweep: save-fail retry [PASS]\n";
    }

    // --- Transfer out / abort / accepted reload -------------------------------
    {
        RecordingHooks hooks;
        FakeClock clock;
        AvatarRuntime runtime(hooks, clock.fn());
        require(runtime.load(key, 100).ok() && runtime.attach(key, 7).ok(), "setup failed");
        auto r = runtime.transfer_out(key);
        require(r.ok(), "transfer out refused: " + r.reason);
        require(runtime.state(key) == AvatarState::Transferring, "transfer out did not move to transferring");
        require(hooks.saw("transfer_out"), "avatar_transfer_out did not run");
        r = runtime.save(key);
        require(r.code == AvatarError::Transferring, "save mid-transfer did not refuse AVATAR_TRANSFERRING");
        r = runtime.attach(key, 8);
        require(r.code == AvatarError::Transferring, "attach mid-transfer did not refuse AVATAR_TRANSFERRING");
        runtime.transfer_abort(key);
        require(runtime.state(key) == AvatarState::Active, "transfer abort did not return to active");
        // The accepted half: the source reloads as a transfer-in.
        require(runtime.transfer_out(key).ok(), "second transfer out refused");
        r = runtime.transfer_accepted(key);
        require(r.ok(), "transfer accepted refused: " + r.reason);
        require(runtime.state(key) == AvatarState::Active, "the accepted reload did not land active");
        require(runtime.source(key) == LoadSource::TransferIn, "the reload did not record transfer-in");
        require(hooks.count("load") == 2 && hooks.saw("transfer_in"),
                "the accepted reload did not run avatar_load + avatar_transfer_in");
        std::cout << "avatar.lifecycle transfer: out -> abort/accepted, refusal codes [PASS]\n";
    }

    // --- Transfer-out hook failure: aborted, back to active -------------------
    {
        RecordingHooks hooks;
        hooks.fail_transfer_out = true;
        FakeClock clock;
        AvatarRuntime runtime(hooks, clock.fn());
        require(runtime.load(key, 100).ok() && runtime.attach(key, 7).ok(), "setup failed");
        auto r = runtime.transfer_out(key);
        require(r.code == AvatarError::TransferAborted, "a failed transfer_out did not report abort");
        require(runtime.state(key) == AvatarState::Active, "a failed transfer_out did not return to active");
        std::cout << "avatar.lifecycle transfer_out_failed: aborted, avatar stays active [PASS]\n";
    }

    // --- Transfer-in load: destination hosts it host-to-host ------------------
    {
        RecordingHooks hooks;
        hooks.fail_transfer_in = true;
        FakeClock clock;
        AvatarRuntime runtime(hooks, clock.fn());
        auto r = runtime.load(key, 100, LoadSource::TransferIn);
        require(r.code == AvatarError::TransferInFailed, "a failed transfer_in did not report failure");
        require(runtime.state(key) == AvatarState::Closed, "the incoming avatar did not close on this host");
        hooks.fail_transfer_in = false;
        r = runtime.load(key, 100, LoadSource::TransferIn);
        require(r.ok(), "transfer-in load refused: " + r.reason);
        require(runtime.state(key) == AvatarState::Active, "transfer-in load did not land active");
        require(runtime.session_of(key) == 0, "transfer-in bound a session by itself");
        require(hooks.saw("load") && hooks.saw("transfer_in") && !hooks.saw("enter"),
                "transfer-in did not run avatar_load + avatar_transfer_in without avatar_enter");
        auto a = runtime.attach(key, 5);
        require(a.ok(), "attaching a session to the landed avatar refused: " + a.reason);
        std::cout << "avatar.lifecycle transfer_in: lands active unbound, failure closes [PASS]\n";
    }

    // --- Visiting: the cross-participation lease -------------------------------
    {
        RecordingHooks hooks;
        FakeClock clock;
        AvatarRuntime runtime(hooks, clock.fn());
        require(runtime.load(key, 100).ok() && runtime.attach(key, 7).ok(), "setup failed");
        auto r = runtime.cross_accept(key);
        require(r.ok() && runtime.state(key) == AvatarState::Visiting, "cross accept did not move to visiting");
        runtime.tick();
        require(hooks.count("tick") >= 1, "a visiting avatar was not ticked (cross-local)");
        r = runtime.save(key);
        require(r.ok(), "cross-local save refused: " + r.reason);
        r = runtime.transfer_out(key);
        require(r.code == AvatarError::Visiting, "transfer out from visiting did not refuse AVATAR_VISITING");
        // Return complete: back to home authority.
        r = runtime.return_complete(key);
        require(r.ok() && runtime.state(key) == AvatarState::Active, "return complete did not restore active");
        // Leaving cross scope: visiting -> transferring.
        require(runtime.cross_accept(key).ok(), "second cross accept refused");
        r = runtime.cross_leave(key);
        require(r.ok() && runtime.state(key) == AvatarState::Transferring,
                "cross leave did not move to transferring");
        // A session detach while visiting runs avatar_leave (visiting -> transferring).
        runtime.transfer_abort(key);
        require(runtime.state(key) == AvatarState::Active, "abort did not return the avatar");
        require(runtime.cross_accept(key).ok(), "third cross accept refused");
        runtime.detach(key, 7);
        require(runtime.state(key) == AvatarState::Transferring, "detach from visiting did not transfer");
        require(hooks.count("leave") >= 1, "avatar_leave did not run on the visiting detach");
        std::cout << "avatar.lifecycle visiting: lease face, tick/save, leave/return [PASS]\n";
    }

    // --- Reclaim: destroy from active, refusal codes elsewhere ----------------
    {
        RecordingHooks hooks;
        FakeClock clock;
        AvatarRuntime runtime(hooks, clock.fn());
        require(runtime.load(key, 100).ok() && runtime.attach(key, 7).ok(), "setup failed");
        auto r = runtime.reclaim(key);
        require(r.ok() && runtime.state(key) == AvatarState::Reclaiming, "destroy did not move to reclaiming");
        r = runtime.reclaim(key);
        require(r.code == AvatarError::Reclaiming, "reclaim on reclaiming did not refuse");
        r = runtime.reclaim_done(key);
        require(r.ok() && runtime.state(key) == AvatarState::Closed, "reclaim did not finish");
        r = runtime.reclaim(other);
        require(r.code == AvatarError::NoAvatar, "reclaim of an absent avatar did not refuse");
        // Refusals carry the state's code with no state change.
        require(runtime.load(key, 100).ok() && runtime.attach(key, 9).ok(), "re-setup failed");
        require(runtime.cross_accept(key).ok(), "cross accept refused");
        r = runtime.reclaim(key);
        require(r.code == AvatarError::Visiting, "reclaim from visiting did not refuse AVATAR_VISITING");
        require(runtime.state(key) == AvatarState::Visiting, "a refusal moved the avatar");
        runtime.fail(key, "budget exceeded");
        require(hooks.saw("fail"), "avatar_fail did not run");
        std::cout << "avatar.lifecycle reclaim: destroy path, refusal codes, avatar_fail [PASS]\n";
    }
}

// The Lua hook surface: LuaService implements AvatarHooks and drives the
// runtime's callbacks into the service table's avatar_* functions (docs/
// avatar-runtime.md "The Lua hook surface"). This pins the bridge, not the
// oracle — avatar.lifecycle already pins the seven-state transitions with a
// recording sink. A script opts into avatars by declaring avatar_load; without
// it avatar_runtime() is null and no lifecycle runs. Each scenario constructs
// a fresh LuaService (a fresh VM) so module state never leaks between them.
void avatar_lua() {
    Context c;
    const auto script = c.path("avatar.lua");
    // The avatar table published by avatar_load is carried back into every
    // later hook; the hooks assert the handle they receive agrees with the
    // context, so a broken handle fails the scenario instead of passing. The
    // character-conditioned branches drive the three failure modes: a nil load
    // (no avatar published), a refused entry (explicit reason) and a raising
    // hook (fail closed).
    write(script, R"avatar(local calls = {}
local function note(name) calls[#calls+1] = name end
local function hook(name, context, avatar)
  assert(avatar ~= nil and avatar.name == context.character
         and avatar.generation == context.generation, "avatar handle and context must agree")
  note(name .. ':' .. avatar.name)
end
return {
  message = function() return table.concat(calls, '|') end,
  avatar_load = function(context, state)
    assert(state == nil or type(state) == 'string', "avatar_load state is a string or nil")
    note(state ~= nil and ('load:' .. context.character .. ':' .. state)
         or ('load:' .. context.character))
    if context.character == 'ghost' then return nil end
    return { name = context.character, generation = context.generation }
  end,
  avatar_enter = function(context, avatar)
    if avatar.name == 'blocked' then return false, 'entry denied' end
    if avatar.name == 'broken' then error('boom') end
    hook('enter', context, avatar)
    return true
  end,
  avatar_tick = function(context, avatar) hook('tick', context, avatar) end,
  avatar_save = function(context, avatar) hook('save', context, avatar) return true end,
  avatar_leave = function(context, avatar) hook('leave', context, avatar) end,
  avatar_detach = function(context, avatar) hook('detach', context, avatar) end,
  avatar_transfer_out = function(context, avatar) hook('transfer_out', context, avatar) return { level = 1 } end,
  avatar_transfer_in = function(context, avatar) hook('transfer_in', context, avatar) return true end,
  avatar_fail = function(context, avatar, reason) note('fail:' .. reason) end,
  -- Cross-visit event hooks are context-only (the registry's event family):
  -- the gate admits, the start/end hooks notify and never veto.
  onCrossVisitAdmit = function(context)
    note('admit:' .. context.character)
    return context.character ~= 'refused'
  end,
  onCrossVisitStart = function(context) note('start:' .. context.character) end,
  onCrossVisitEnd = function(context) note('end:' .. context.character) end,
}
)avatar");
    // avatar_load is the opt-in gate: a script without it hosts no avatars.
    const auto plain = c.path("plain.lua");
    write(plain, "return {message=function(p) return p end}");
    {
        LuaService plain_service(plain.string());
        require(plain_service.avatar_runtime() == nullptr, "a script without avatar_load opts out of avatars");
    }
    LuaService service(script.string());
    require(service.avatar_runtime() != nullptr, "avatar_load enables the avatar runtime");
    auto& runtime = *service.avatar_runtime();
    const auto seen = [&] { return service.message("read").value(); };
    const auto contains = [&](std::string_view needle) { return seen().find(needle) != std::string::npos; };
    const AvatarKey hero{"realm-a", "hero", 7};
    const AvatarKey other{"realm-a", "rival", 3};

    // A home load stops in loading until the session attach drives the
    // avatar_enter edge; the load publishes the avatar table.
    require(runtime.load(hero, 60000).code == AvatarError::Ok, "avatar load");
    require(runtime.state(hero) == AvatarState::Loading, "home load stops in loading");
    require(contains("load:hero"), "avatar_load ran");
    require(runtime.attach(hero, 42).code == AvatarError::Ok, "avatar attach");
    require(runtime.state(hero) == AvatarState::Active, "attach lands active");
    require(contains("enter:hero"), "avatar_enter receives the avatar table");

    // The node tick drives avatar_tick for active avatars.
    service.start();
    service.tick();
    require(contains("tick:hero"), "avatar_tick fires on the node tick");

    // A second character is independent; the same table reaches it.
    require(runtime.load(other, 60000).code == AvatarError::Ok, "second avatar load");
    require(runtime.attach(other, 43).code == AvatarError::Ok, "second avatar attach");
    require(runtime.active_count() == 2, "two active avatars");

    // A loaded record reaches avatar_load as the state string; a recordless
    // load passes nil.
    {
        const AvatarKey recorded{"realm-a", "recorded", 1};
        require(runtime.load(recorded, 60000, LoadSource::Home, "hp=10").code == AvatarError::Ok,
                "recorded avatar load");
        require(contains("load:recorded:hp=10"), "avatar_load did not receive the record");
    }

    // The forced save is the single save entry and runs from the model.
    require(runtime.save(hero).code == AvatarError::Ok, "avatar save");
    require(contains("save:hero"), "avatar_save ran");

    // A refused entry carries the script's reason and closes the avatar.
    const AvatarKey blocked{"realm-a", "blocked", 1};
    require(runtime.load(blocked, 60000).code == AvatarError::Ok, "blocked avatar load");
    const auto refused = runtime.attach(blocked, 44);
    require(refused.code == AvatarError::EnterRefused, "avatar_enter refusal");
    equal(refused.reason, "entry denied");
    require(runtime.state(blocked) == AvatarState::Closed, "a refused entry closes the avatar");

    // A raising hook fails closed the same way: no visible avatar.
    const AvatarKey broken{"realm-a", "broken", 1};
    require(runtime.load(broken, 60000).code == AvatarError::Ok, "broken avatar load");
    require(runtime.attach(broken, 45).code == AvatarError::EnterRefused,
            "a raising avatar_enter fails closed");
    require(runtime.state(broken) == AvatarState::Closed, "a raising entry closes the avatar");
    require(!contains("enter:broken"), "a raising hook records nothing");

    // A nil load publishes no avatar.
    const AvatarKey ghost{"realm-a", "ghost", 1};
    require(runtime.load(ghost, 60000).code == AvatarError::LoadFailed, "a nil avatar_load refuses");
    require(runtime.state(ghost) == AvatarState::Closed, "a failed load publishes no visible avatar");
    require(contains("load:ghost"), "avatar_load ran before returning nil");

    // Detach runs leave then detach and suspends; reattach within grace
    // returns to active without a reload or a second enter.
    runtime.detach(hero, 42);
    require(runtime.state(hero) == AvatarState::Suspended, "detach suspends");
    require(contains("leave:hero") && contains("detach:hero"), "leave and detach ran");
    const auto before = seen();
    require(runtime.attach(hero, 42).code == AvatarError::Ok, "reattach within grace");
    require(runtime.state(hero) == AvatarState::Active, "reattach returns active");
    equal(seen(), before); // reattach within grace runs no hook

    // Transfer: out snapshots and moves to transferring; the destination
    // reload runs load again then transfer_in and lands active unbound.
    require(runtime.transfer_out(hero).code == AvatarError::Ok, "avatar transfer out");
    require(runtime.state(hero) == AvatarState::Transferring, "transfer out moves to transferring");
    require(contains("transfer_out:hero"), "avatar_transfer_out ran");
    require(runtime.transfer_accepted(hero).code == AvatarError::Ok, "avatar transfer accepted");
    require(runtime.state(hero) == AvatarState::Active, "transfer in lands active");
    require(runtime.source(hero) == LoadSource::TransferIn, "transfer in is a transfer-in load");
    require(contains("transfer_in:hero"), "avatar_transfer_in ran");

    // One live avatar per character: a second generation of the same
    // character refuses while the first lives.
    require(runtime.load(AvatarKey{"realm-a", "hero", 8}, 60000).code == AvatarError::Exists,
            "a second generation of a live character refuses");

    // Reclaim: the final save must succeed before closed.
    require(runtime.reclaim(hero).code == AvatarError::Ok, "avatar reclaim");
    require(runtime.reclaim_done(hero).code == AvatarError::Ok, "avatar reclaim done");
    require(runtime.state(hero) == AvatarState::Closed, "reclaim done closes the avatar");
    require(contains("save:hero"), "the final save ran");

    // A cross visit: the admission gate runs, avatar_load creates the visiting
    // avatar, the session binds and the avatar lands visiting — without
    // avatar_enter, the lease face of the oracle.
    const AvatarKey visitor{"home-realm", "wanderer", 5};
    require(runtime.admit_visit(visitor, 50, 60000).code == AvatarError::Ok, "cross visit admitted");
    require(runtime.state(visitor) == AvatarState::Visiting, "a cross visit lands visiting");
    require(runtime.session_of(visitor) == 50, "the visiting avatar binds the session");
    require(runtime.source(visitor) == LoadSource::Cross, "the load source is cross");
    require(contains("admit:wanderer") && contains("load:wanderer") && contains("start:wanderer"),
            "admit, load and start ran");
    require(!contains("enter:wanderer"), "a cross visit never runs avatar_enter");

    // A denied admit refuses the join and creates no avatar (fail closed).
    const AvatarKey gate_refused{"home-realm", "refused", 5};
    require(runtime.admit_visit(gate_refused, 51, 60000).code == AvatarError::EnterRefused,
            "a denied admit refuses the cross visit");
    require(runtime.state(gate_refused) == AvatarState::Closed, "a denied admit publishes nothing");
    require(contains("admit:refused"), "the gate hook ran before the denial");

    // Abnormal termination closes the visit with a best-effort save and the
    // end notification, and the single-writer rule still holds afterwards.
    require(runtime.close_visit(visitor).code == AvatarError::Ok, "cross visit closed");
    require(runtime.state(visitor) == AvatarState::Closed, "the visit closes");
    require(contains("save:wanderer") && contains("end:wanderer"), "save and end ran");
    require(runtime.admit_visit(visitor, 52, 60000).code == AvatarError::Ok, "the character may rejoin");
    require(runtime.load(visitor, 60000).code == AvatarError::Exists, "a live visit refuses a home load");

    // Node retirement (reload / graceful stop): every live avatar gets a
    // best-effort save then closes; a live visiting avatar also fires the end
    // notification.
    const AvatarKey survivor{"realm-a", "survivor", 2};
    require(runtime.load(survivor, 60000).code == AvatarError::Ok, "survivor load");
    require(runtime.attach(survivor, 53).code == AvatarError::Ok, "survivor attach");
    const AvatarKey resident{"home-realm", "resident", 4};
    require(runtime.admit_visit(resident, 54, 60000).code == AvatarError::Ok, "resident visit");
    service.stop();
    require(runtime.state(survivor) == AvatarState::Closed
                && runtime.state(resident) == AvatarState::Closed,
            "retirement closes every live avatar");
    require(contains("save:survivor") && contains("save:resident"),
            "retirement saves each live avatar");
    require(contains("end:resident"), "retirement fires onCrossVisitEnd for a visiting avatar");
    std::cout << "avatar.lua: script-backed hook surface, opt-in gate, fail-closed refusal, cross visit, "
                 "retirement [PASS]\n";
}

// The session plane wiring: a crossServer join with a participant context
// creates the visiting avatar at the host (gate, load, start), business frames
// flow while the visit is live, the disconnect closes it (best-effort save and
// the end notification), a refused admission rejects the join, and a reload
// retires what the old VM owned so the fresh VM admits the character again.
void avatar_session() {
    Context c;
    const auto script = c.path("avatar_session.lua");
    write(script, R"avatar(local calls = {}
local function note(name) calls[#calls+1] = name end
return {
  message = function(payload, ctx)
    if payload == "who" then return ctx.character .. "," .. tostring(ctx.generation) end
    return table.concat(calls, "|")
  end,
  avatar_load = function(context, state)
    note("load:" .. context.character)
    if context.character == "refused" then return nil end
    return { name = context.character, generation = context.generation }
  end,
  avatar_tick = function(context, avatar) note("tick:" .. avatar.name) end,
  avatar_save = function(context, avatar) note("save:" .. avatar.name) return true end,
  avatar_leave = function(context, avatar) note("leave:" .. avatar.name) end,
  onCrossVisitAdmit = function(context)
    note("admit:" .. context.character)
    return context.character ~= "refused"
  end,
  onCrossVisitStart = function(context) note("start:" .. context.character) end,
  onCrossVisitEnd = function(context) note("end:" .. context.character) end,
}
)avatar");
    const auto cross = port();
    auto& node = c.node("crossServer", cross, {"--script", script.string()});
    const auto send_frame = [](RawSocket& raw, std::string_view payload) {
        std::string frame(4, '\0');
        auto length = static_cast<std::uint32_t>(payload.size());
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(length & 255); length >>= 8; }
        raw.send(frame+std::string(payload));
    };
    const auto read_frame = [](RawSocket& raw) {
        std::size_t length = 0;
        for (unsigned char ch : raw.receive(4)) length = (length << 8) | ch;
        return raw.receive(length);
    };
    const auto join = [&](RawSocket& raw, std::string_view character) {
        send_frame(raw, "MLC1 1 arena g r1 ch " + std::string(character) + " gen 5 ret home g r1");
        return read_frame(raw);
    };
    const auto trace = [&](RawSocket& raw) {
        send_frame(raw, "read");
        return read_frame(raw);
    };
    const auto has = [](std::string_view haystack, std::string_view needle) {
        return haystack.find(needle) != std::string_view::npos;
    };

    // A live visit: the join admits, loads and starts; business frames carry
    // the participant; no avatar_enter (the lease face).
    {
        RawSocket raw(cross);
        equal(join(raw, "wanderer"), "MLC1 OK 1");
        const auto seen = trace(raw);
        require(has(seen, "admit:wanderer") && has(seen, "load:wanderer") && has(seen, "start:wanderer"),
                "the join admitted, loaded and started the visit");
        require(!has(seen, "enter:"), "a cross visit never runs avatar_enter");
        send_frame(raw, "who");
        equal(read_frame(raw), "wanderer,5");
    }
    // The disconnect closes the visit: the same VM's trace now shows the save
    // and the end notification (the teardown is async, so poll until it lands).
    // The reader is a second, distinct character — every session on the host
    // must join first, and the trace lives in the shared VM, not per session.
    {
        RawSocket raw(cross);
        equal(join(raw, "observer"), "MLC1 OK 1");
        eventually([&] {
            send_frame(raw, "read");
            return has(read_frame(raw), "save:wanderer");
        }, 5s, "the disconnect did not save and close the visit");
        require(has(trace(raw), "end:wanderer"), "the visit end notification ran");
    }
    // The character may rejoin after the close; a denied gate refuses its join.
    {
        RawSocket raw(cross);
        equal(join(raw, "wanderer"), "MLC1 OK 1");
    }
    {
        RawSocket raw(cross);
        const auto reply = join(raw, "refused");
        require(reply.starts_with("MLC1 ERR "), "a denied visit refuses the join");
    }
    // A reload retires the old VM's visit (save + end + closed), and the fresh
    // VM admits the character again with an empty trace.
    node.signal(SIGHUP);
    node.ready("script reloaded");
    {
        RawSocket raw(cross);
        equal(join(raw, "wanderer"), "MLC1 OK 1");
        equal(trace(raw), "admit:wanderer|load:wanderer|start:wanderer");
    }
    node.signal(SIGTERM);
    require(node.wait() == 0, "crossServer graceful shutdown failed");
    std::cout << "avatar.session: crossServer join -> visiting avatar, business, disconnect, reload [PASS]\n";
}

// The home login path on the avatar-host session: a join whose participant
// enters its own realm (realm == return_realm) hosts the character's home
// avatar — avatar_load then avatar_enter drive the join to active (bound),
// business frames flow while it is live, a refused load or a denied entry
// rejects the join, the disconnect detaches the avatar (avatar_leave then
// avatar_detach, the grace window opens), and a reconnect within grace
// reattaches to active without a reload.
void avatar_home() {
    Context c;
    const auto script = c.path("avatar_home.lua");
    write(script, R"avatar(local calls = {}
local function note(name) calls[#calls+1] = name end
return {
  message = function(payload, ctx)
    if payload == "who" then return ctx.character .. "," .. tostring(ctx.generation) end
    return table.concat(calls, "|")
  end,
  avatar_load = function(context, state)
    note("load:" .. context.character)
    if context.character == "refused" then return nil end
    return { name = context.character, generation = context.generation }
  end,
  avatar_enter = function(context, avatar)
    note("enter:" .. avatar.name)
    return context.character ~= "denied"
  end,
  avatar_tick = function(context, avatar) note("tick:" .. avatar.name) end,
  avatar_save = function(context, avatar) note("save:" .. avatar.name) return true end,
  avatar_leave = function(context, avatar) note("leave:" .. avatar.name) end,
  avatar_detach = function(context, avatar) note("detach:" .. avatar.name) end,
}
)avatar");
    const auto cross = port();
    auto& node = c.node("crossServer", cross, {"--script", script.string()});
    const auto send_frame = [](RawSocket& raw, std::string_view payload) {
        std::string frame(4, '\0');
        auto length = static_cast<std::uint32_t>(payload.size());
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(length & 255); length >>= 8; }
        raw.send(frame+std::string(payload));
    };
    const auto read_frame = [](RawSocket& raw) {
        std::size_t length = 0;
        for (unsigned char ch : raw.receive(4)) length = (length << 8) | ch;
        return raw.receive(length);
    };
    // The home join declares the participant entering its own realm: the same
    // origin and return realm. Anonymous joins keep working (no avatar).
    const auto join = [&](RawSocket& raw, std::string_view character) {
        if (character.empty()) {
            send_frame(raw, "MLC1 1 home g r1");
            return read_frame(raw);
        }
        send_frame(raw, "MLC1 1 home g r1 ch " + std::string(character) + " gen 5 ret home g r1");
        return read_frame(raw);
    };
    const auto trace = [&](RawSocket& raw) {
        send_frame(raw, "read");
        return read_frame(raw);
    };
    const auto has = [](std::string_view haystack, std::string_view needle) {
        return haystack.find(needle) != std::string_view::npos;
    };

    // The home join loads and enters: active and bound. Business frames carry
    // the participant (the home face runs avatar_enter, unlike a cross visit).
    {
        RawSocket raw(cross);
        equal(join(raw, "hero"), "MLC1 OK 1");
        const auto seen = trace(raw);
        require(has(seen, "load:hero") && has(seen, "enter:hero"),
                "the home join loaded and entered the avatar");
        send_frame(raw, "who");
        equal(read_frame(raw), "hero,5");
    }
    // The disconnect detaches: the trace shows leave then detach (the grace
    // window opens), read through a second joined session because the trace
    // lives in the shared VM and the teardown is async.
    {
        RawSocket raw(cross);
        equal(join(raw, "observer"), "MLC1 OK 1");
        eventually([&] {
            send_frame(raw, "read");
            return has(read_frame(raw), "detach:hero");
        }, 5s, "the disconnect did not run avatar_leave then avatar_detach");
    }
    // A reconnect within grace reattaches to active without a reload: the
    // character's load and enter each ran once (the first join); the reattach
    // adds none, and the session carries the character again.
    {
        RawSocket raw(cross);
        equal(join(raw, "hero"), "MLC1 OK 1");
        const auto seen = trace(raw);
        require(has(seen, "detach:hero"), "the detach is missing from the trace");
        const auto count = [&](std::string_view needle) {
            std::size_t n = 0;
            for (std::size_t pos = seen.find(needle); pos != std::string::npos;
                 pos = seen.find(needle, pos + needle.size())) ++n;
            return n;
        };
        require(count("load:hero") == 1 && count("enter:hero") == 1,
                "the reconnect reloaded or re-entered the avatar");
        send_frame(raw, "who");
        equal(read_frame(raw), "hero,5");
    }
    // A refused load (nil avatar_load) and a denied entry (avatar_enter false)
    // reject the join — entry is never accepted without a live avatar.
    {
        RawSocket raw(cross);
        const auto reply = join(raw, "refused");
        require(reply.starts_with("MLC1 ERR "), "a failed avatar_load refused the join");
    }
    {
        RawSocket raw(cross);
        const auto reply = join(raw, "denied");
        require(reply.starts_with("MLC1 ERR "), "a denied avatar_enter refused the join");
    }
    // An anonymous join keeps working and hosts no avatar: it adds no load or
    // enter to the (shared, accumulating) trace.
    {
        const auto count = [&](std::string_view haystack, std::string_view needle) {
            std::size_t n = 0;
            for (std::size_t pos = haystack.find(needle); pos != std::string::npos;
                 pos = haystack.find(needle, pos + needle.size())) ++n;
            return n;
        };
        RawSocket before(cross);
        equal(join(before, "probe"), "MLC1 OK 1");
        const auto prior = trace(before);
        RawSocket raw(cross);
        equal(join(raw, ""), "MLC1 OK 1");
        send_frame(raw, "read");
        const auto seen = read_frame(raw);
        require(count(seen, "load:") == count(prior, "load:")
                    && count(seen, "enter:") == count(prior, "enter:"),
                "an anonymous join hosted an avatar");
    }
    node.signal(SIGTERM);
    require(node.wait() == 0, "crossServer graceful shutdown failed");
    std::cout << "avatar.home: home join -> active avatar, business, disconnect, grace reattach [PASS]\n";
}
// The home join with a --data endpoint defers its reply behind the one-shot
// MLD1 LOAD (the async record fetch): a committed record reaches avatar_load as
// the state argument, MLD1 EMPTY (a first login) reaches it as nil, and a data
// endpoint that cannot answer refuses the join instead of hosting an avatar
// whose record was never established. The deferred path must load, attach,
// reply and carry the character exactly like the synchronous join does.
void avatar_home_record() {
    Context c;
    const auto script = c.path("avatar_home_record.lua");
    write(script, R"avatar(local calls = {}
return {
  message = function(payload)
    if payload == "read" then return table.concat(calls, "|") end
    return "noop"
  end,
  avatar_load = function(context, state)
    calls[#calls+1] = "load:" .. context.character .. "=" .. tostring(state)
    return { name = context.character, generation = context.generation }
  end,
  avatar_enter = function(context, avatar) return true end,
}
)avatar");
    write(c.path("echo.lua"), echo);
    const auto data = port(), cross = port(), dead = port(), broken = port();
    auto& store = c.node("data", data, {"--script", c.path("echo.lua").string()});
    auto& host = c.node("crossServer", cross, {"--script", script.string(), "--data", endpoint(data)});
    auto& refused = c.node("crossServer", broken, {"--script", script.string(), "--data", endpoint(dead)});
    const auto send_frame = [](RawSocket& raw, std::string_view payload) {
        std::string frame(4, '\0');
        auto length = static_cast<std::uint32_t>(payload.size());
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(length & 255); length >>= 8; }
        raw.send(frame+std::string(payload));
    };
    const auto read_frame = [](RawSocket& raw) {
        std::size_t length = 0;
        for (unsigned char ch : raw.receive(4)) length = (length << 8) | ch;
        return raw.receive(length);
    };
    const auto join = [&](RawSocket& raw, std::string_view character) {
        send_frame(raw, "MLC1 1 home g r1 ch " + std::string(character) + " gen 5 ret home g r1");
        return read_frame(raw);
    };
    const auto trace = [&](RawSocket& raw) {
        send_frame(raw, "read");
        return read_frame(raw);
    };
    const auto has = [](std::string_view haystack, std::string_view needle) {
        return haystack.find(needle) != std::string_view::npos;
    };
    // Seed one committed record; the unseeded character stays EMPTY.
    {
        RawSocket raw(data);
        send_frame(raw, "MLD1 SUBMIT home hero gen 5 seq 1 hp=10 mp=5");
        require(read_frame(raw) == "MLD1 RECEIVED gen 5 seq 1", "the seed submit was not received");
    }
    // The deferred home join fetches the record: avatar_load sees the opaque
    // STATE payload and the join completes with the character bound.
    {
        RawSocket raw(cross);
        equal(join(raw, "hero"), "MLC1 OK 1");
        const auto seen = trace(raw);
        require(has(seen, "load:hero=hp=10 mp=5"),
                "avatar_load did not receive the fetched record state");
    }
    // A first login (MLD1 EMPTY) loads with nil state — not a refusal.
    {
        RawSocket raw(cross);
        equal(join(raw, "ghost"), "MLC1 OK 1");
        const auto seen = trace(raw);
        require(has(seen, "load:ghost=nil"), "an EMPTY record did not reach avatar_load as nil");
    }
    // A data endpoint that cannot answer refuses the join: a login whose
    // record cannot be established must not host the avatar.
    {
        RawSocket raw(broken);
        const auto reply = join(raw, "hero");
        require(reply.starts_with("MLC1 ERR "), "an unreachable data endpoint did not refuse the join");
    }
    store.signal(SIGTERM);
    host.signal(SIGTERM);
    refused.signal(SIGTERM);
    std::cout << "avatar.home.record: deferred home join feeds the MLD1 LOAD record "
        "into avatar_load, EMPTY is nil, unreachable refuses [PASS]\n";
}
} // namespace mistlane::test
