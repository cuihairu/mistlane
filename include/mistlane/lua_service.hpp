// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <lua.hpp>
#include "mistlane/avatar_runtime.hpp"
#include "mistlane/cross.hpp"
#include "mistlane/entity_runtime.hpp"
#include "mistlane/http_service.hpp"
#include "mistlane/migration.hpp"
#include <boost/redis/connection.hpp>
#include <boost/asio/io_context.hpp>

namespace mistlane {
struct ServiceContext {
    std::string role = "gameServer", transport = "tcp";
    unsigned epoch = 1;
    // What the listeners of this node require from the script: framed game
    // sessions (tcp/ws/wss) and/or the Lua HTTP service (http/https).
    bool game_sessions = true, http_service = false;
};
// Push adapter for Lua websocket sessions; implemented by the node. All calls
// run on the node's event loop thread, so implementations need no locking.
class WebsocketSink {
public:
    virtual ~WebsocketSink() = default;
    // Returns "" on success or a short machine-readable reason for Lua.
    virtual std::string websocket_send(std::uint64_t id, std::string_view payload) = 0;
};
// Push adapter for framed game sessions, addressed by session id.
class GameSessionSink {
public:
    virtual ~GameSessionSink() = default;
    // Returns "" on success or a short machine-readable reason for Lua.
    virtual std::string session_send(std::uint64_t id, std::string_view payload) = 0;
    // Queues the payload for every live session; returns how many accepted it.
    virtual std::size_t session_broadcast(std::string_view payload) = 0;
};
// Placement lookup adapter (registry client); implemented by the node. All
// calls run on the node's event loop thread, so implementations need no locking.
class PlacementSink {
public:
    virtual ~PlacementSink() = default;
    // Asks the coordinator registry for one placement and fires the callback
    // once on the event loop thread: address non-empty on success, otherwise
    // address empty and reason a short machine-readable word.
    virtual void place(std::string_view role, std::string_view namespace_, std::string_view scope,
        std::function<void(std::string_view address, std::string_view reason)> answer) = 0;
};
// Home-data generation authority adapter (login): asks the MLD1 data endpoint
// for the data-issued hosting generation of a participant, one question per
// connection. The data endpoint is the sole generation authority, so the login
// entry flow binds the granted generation into the ticket rather than choosing
// one. Implemented by the node; calls run on the event loop thread.
class GrantSink {
public:
    virtual ~GrantSink() = default;
    // Answers once on the event loop thread: the granted generation on
    // success, otherwise 0 and a short machine-readable word ("disabled" for
    // no data endpoint, "unreachable"/"refused"/"INVALID"/... /"timeout").
    virtual void grant(std::string_view realm, std::string_view character,
        std::function<void(std::uint64_t generation, std::string_view reason)> answer) = 0;
};
// Cross-return sender adapter (crossServer): the framework owns the MLD1
// ordered retry queue, so a script only seeds a participant's durable cursor
// and enqueues changes; the sender connects to home data, submits in sequence
// and drains each change only after a store-backed ACK durable. All calls run
// on the node's event loop thread, so implementations need no locking.
class ReturnSink {
public:
    virtual ~ReturnSink() = default;
    // Seeds the durable cursor from LOAD; returns "" or a short reason.
    virtual std::string seed(std::string_view realm, std::string_view character) = 0;
    // Enqueues one change for ordered return; returns "" or a short reason.
    virtual std::string enqueue(std::string_view realm, std::string_view character,
        std::uint64_t generation, std::uint64_t sequence, std::string_view payload) = 0;
    // Whether every change queued for this participant has been durably
    // acknowledged: no pending, in-flight or pre-load-held change remains. A
    // participant never seeded holds nothing and is drained. This is the
    // graceful return's condition — leaving the cross scope is complete only
    // when the final change-set is acknowledged durably.
    virtual bool drained(std::string_view realm, std::string_view character) const = 0;
};
// Source-side transfer adapter (the host that holds the avatar): drives the
// coordinator's MLR1 MIG sequence and the home data endpoint's final MLD1
// SUBMIT/FENCE, then releases the avatar once the destination accepted and the
// route committed. The script only names realm + character, the destination
// owner and the source scene; the generation is recovered from the runtime.
// Implemented by the node; calls run on the node's event loop thread.
class TransferSink {
public:
    virtual ~TransferSink() = default;
    // Answers once on the event loop thread: an empty error on success (the
    // avatar released), otherwise a short machine-readable word ("disabled" for
    // no avatar runtime or no coordinator/data endpoint, "no avatar", or the
    // coordinator/data refusal — "CONFLICT"/"STALE"/"ORDER"/"timeout"/...).
    virtual void transfer(std::string_view realm, std::string_view character,
        MigrationOwner destination, std::string_view source_scene,
        std::function<void(std::string error)> answer) = 0;
};
// Script-declared business gauge adapter: mistlane.metrics.gauge publishes a
// named gauge onto the node's Prometheus registry (rooms, queue depths — the
// game's own vocabulary). Implemented by Diagnostics; called on the node's
// event-loop thread only, so implementations need no locking.
class ScriptMetricsSink {
public:
    virtual ~ScriptMetricsSink() = default;
    virtual void script_gauge(std::string_view name, double value) = 0;
};
class RedisRuntime {
public:
    RedisRuntime(boost::asio::io_context& io, std::string host, std::uint16_t port);
    ~RedisRuntime();
    void start();
    void stop();
    bool acquire_request();
    void release_request();
    std::shared_ptr<boost::redis::connection> connection() const { return connection_; }
private:
    std::shared_ptr<boost::redis::connection> connection_;
    boost::redis::config config_;
    std::size_t outstanding_ = 0;
};
// One VM per node, only accessed from that node's io_context thread.
//
// LuaService also implements the Avatar hook surface (docs/avatar-runtime.md
// "The Lua hook surface"): the service table's avatar_load/enter/leave/tick/
// save/transfer_out/transfer_in/detach/fail functions are the framework's
// callbacks at the avatar lifecycle edges. The script opts into avatars by
// declaring avatar_load; without it avatar_runtime() returns null and the node
// never drives a lifecycle for a session. The runtime is owned here so it is
// replaced with the VM on a SIGHUP reload — reload is a node retirement, not
// an avatar migration (docs/avatar-runtime.md), so avatars do not survive it.
class LuaService : public AvatarHooks {
public:
    struct AsyncContext { lua_State* state = nullptr; };
    explicit LuaService(const std::string& path, ServiceContext context = {}, std::shared_ptr<RedisRuntime> redis = {},
                        WebsocketSink* websocket_sink = nullptr, GameSessionSink* session_sink = nullptr,
                        PlacementSink* placement_sink = nullptr, EntityRuntime* entity_runtime = nullptr,
                        const TicketIssuer* ticket_issuer = nullptr, ReturnSink* return_sink = nullptr,
                        ScriptMetricsSink* metrics_sink = nullptr, GrantSink* grant_sink = nullptr,
                        TransferSink* transfer_sink = nullptr);
    ~LuaService();
    LuaService(const LuaService&) = delete;
    LuaService& operator=(const LuaService&) = delete;
    // Returns the handler's reply, or nothing when the handler returns nil.
    std::optional<std::string> message(std::string_view payload, const CrossContext* context = nullptr);
    void validate_http();
    WebResponse http(const WebRequest& request);
    // Lua websocket lifecycle (optional callbacks; failures close the session).
    void websocket_open(std::uint64_t id, const std::string& path);
    void websocket_message(std::uint64_t id, std::string_view payload);
    void websocket_close(std::uint64_t id);
    // Binds the script's entity implementation table (service.entities[type]
    // [method] = function(args, ctx)) onto the dispatcher, clearing any prior
    // bindings first so a script reload can never leave a closure into a closed
    // state. Unknown entity or method names refuse the whole load (fail-closed);
    // a missing implementation is not an error here — the dispatcher answers
    // NO_IMPLEMENTATION for it. Returns false with a reason on refusal.
    bool bind_entity_handlers(EntityDispatcher& dispatcher, const EntityContract& contract, std::string& error);
    void start();
    void stop();
    void tick();
    void shutdown();
    // The script-backed avatar runtime of this VM, or null when the script did
    // not declare avatar_load (the opt-in gate): a node drives load/attach/
    // detach/save/reclaim from the session lifecycle only when this is non-null.
    AvatarRuntime* avatar_runtime() { return avatar_enabled_ ? &avatar_runtime_ : nullptr; }
private:
    static int stream_create_group(lua_State*);
    static int stream_publish(lua_State*);
    static int stream_read(lua_State*);
    static int stream_ack(lua_State*);
    static int stream_claim(lua_State*);
    static int websocket_send(lua_State*);
    static int session_send(lua_State*);
    static int session_broadcast(lua_State*);
    static void push_stream_api(lua_State*, LuaService*);
    static void push_websocket_api(lua_State*, LuaService*);
    static void push_session_api(lua_State*, LuaService*);
    static int placement_call(lua_State*);
    static void push_placement_api(lua_State*, LuaService*);
    static int grant_call(lua_State*);
    static void push_grant_api(lua_State*, LuaService*);
    static int transfer_call(lua_State*);
    static void push_transfer_api(lua_State*, LuaService*);
    void websocket_callback(const char* name, std::uint64_t id, std::string_view payload, const std::string& path);
    static int leaderboard_add(lua_State*);
    static int leaderboard_get(lua_State*);
    static int leaderboard_top(lua_State*);
    static int leaderboard_remove(lua_State*);
    static int entity_spawn(lua_State*);
    static int entity_destroy(lua_State*);
    static int entity_properties(lua_State*);
    static int entity_observe(lua_State*);
    static int entity_depart(lua_State*);
    static int entity_flush(lua_State*);
    static int entity_set_owner(lua_State*);
    static int entity_state_inspect(lua_State*);
    static void push_entity_api(lua_State*, EntityRuntime*);
    static int ticket_issue(lua_State*);
    static void push_ticket_api(lua_State*, LuaService*);
    static int return_seed(lua_State*);
    static int return_enqueue(lua_State*);
    static void push_return_api(lua_State*, LuaService*);
    static int metrics_gauge(lua_State*);
    static void push_metrics_api(lua_State*, LuaService*);
    EntityDispatcher::Handler make_entity_handler(const std::string& type, const std::string& name,
                                                  unsigned method_id, ContractArgs returns,
                                                  const EntityContract* shapes) const;
    void callback(const char* name);
    void check(int status);
    // One avatar hook dispatch: calls module.<name>(context[, avatar][, reason])
    // through the same traceback and instruction budget as every other
    // callback, and decodes the results here so a hook never leaves a value on
    // the stack for the caller to read after the stack guard unwinds.
    struct AvatarHookOutcome {
        bool present = false; // the service table names the hook
        bool raised = false;  // the hook is malformed or raised a Lua error
        bool truthy = false;  // truthiness of the first return value
        bool has_text = false;// the first return value is a string
        std::string text;     // the first return value, when it is a string
        std::string reason;   // second return value, when it is a string
        std::string ref;      // avatar_load: the encoded avatar table handle
    };
    AvatarHookOutcome avatar_hook(const char* name, const AvatarKey& key, const std::string& ref,
                                  bool with_avatar, std::string_view reason, bool store_avatar,
                                  std::string_view state = {});
    bool on_load(const AvatarKey& key, std::string_view state, std::string& avatar_ref) override;
    bool on_enter(const AvatarKey& key, const std::string& avatar_ref, std::string& reason) override;
    void on_leave(const AvatarKey& key, const std::string& avatar_ref) override;
    void on_tick(const AvatarKey& key, const std::string& avatar_ref) override;
    bool on_save(const AvatarKey& key, const std::string& avatar_ref) override;
    bool on_transfer_out(const AvatarKey& key, const std::string& avatar_ref,
                         std::string& snapshot) override;
    bool on_transfer_in(const AvatarKey& key, const std::string& avatar_ref) override;
    void on_detach(const AvatarKey& key, const std::string& avatar_ref) override;
    void on_fail(const AvatarKey& key, const std::string& avatar_ref, std::string_view reason) override;
    bool on_cross_admit(const AvatarKey& key) override;
    void on_cross_start(const AvatarKey& key) override;
    void on_cross_end(const AvatarKey& key) override;
    // Node retirement: best-effort avatar_save then close for every avatar the
    // VM owns, before the VM is destroyed (stop() and the destructor both reach
    // it; idempotent). The scripted stop hook runs first, then the avatars flush.
    void retire_avatars();
    lua_State* state_ = nullptr;
    int module_ = LUA_NOREF;
    ServiceContext context_;
    WebsocketSink* websocket_sink_ = nullptr;
    GameSessionSink* session_sink_ = nullptr;
    PlacementSink* placement_sink_ = nullptr;
    GrantSink* grant_sink_ = nullptr;
    TransferSink* transfer_sink_ = nullptr;
    EntityRuntime* entity_runtime_ = nullptr;
    const TicketIssuer* ticket_issuer_ = nullptr;
    ReturnSink* return_sink_ = nullptr;
    ScriptMetricsSink* metrics_sink_ = nullptr;
    bool started_ = false, stopped_ = false, shutdown_ = false;
    std::shared_ptr<AsyncContext> async_context_;
    std::shared_ptr<RedisRuntime> redis_;
    // Declared last: the runtime only stores the hooks reference, and every
    // other member is initialized before it. It is replaced with the VM on a
    // reload (see the class comment), so its hooks always name the live state.
    AvatarRuntime avatar_runtime_;
    bool avatar_enabled_ = false;
};
}
