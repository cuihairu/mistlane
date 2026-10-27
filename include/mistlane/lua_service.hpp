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
#include "mistlane/cross.hpp"
#include "mistlane/http_service.hpp"
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
class LuaService {
public:
    struct AsyncContext { lua_State* state = nullptr; };
    explicit LuaService(const std::string& path, ServiceContext context = {}, std::shared_ptr<RedisRuntime> redis = {},
                        WebsocketSink* websocket_sink = nullptr, GameSessionSink* session_sink = nullptr,
                        PlacementSink* placement_sink = nullptr);
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
    void start();
    void stop();
    void tick();
    void shutdown();
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
    void websocket_callback(const char* name, std::uint64_t id, std::string_view payload, const std::string& path);
    static int leaderboard_add(lua_State*);
    static int leaderboard_get(lua_State*);
    static int leaderboard_top(lua_State*);
    static int leaderboard_remove(lua_State*);
    void callback(const char* name);
    void check(int status);
    lua_State* state_ = nullptr;
    int module_ = LUA_NOREF;
    ServiceContext context_;
    WebsocketSink* websocket_sink_ = nullptr;
    GameSessionSink* session_sink_ = nullptr;
    PlacementSink* placement_sink_ = nullptr;
    bool started_ = false, stopped_ = false, shutdown_ = false;
    std::shared_ptr<AsyncContext> async_context_;
    std::shared_ptr<RedisRuntime> redis_;
};
}
