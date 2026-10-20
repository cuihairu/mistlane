// SPDX-License-Identifier: Apache-2.0
#include "test_support.hpp"
#include "test_fixtures.hpp"
#include "test_resources.hpp"
#include "mistlane/api.hpp"
#include "mistlane/cross.hpp"
#include "mistlane/entity_contract.hpp"
#include "mistlane/entity_dispatch.hpp"
#include "mistlane/entity_envelope.hpp"
#include "mistlane/entity_runtime.hpp"
#include "mistlane/entity_property.hpp"
#include "mistlane/entity_lua.hpp"
#include "mistlane/entity_sync.hpp"
#include "mistlane/entity_persist.hpp"
#include "mistlane/record.hpp"
#include "mistlane/migration.hpp"
#include "mistlane/http_client.hpp"
#include "mistlane/token_bucket.hpp"
#include "mistlane/transport.hpp" // max_frame bounds the flood sizing
#include <curl/curl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <malloc.h>
#include <numeric>
#include <future>
#include <sstream>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <openssl/evp.h>
#include <lua.hpp>

namespace mistlane::test {
void rpc_lifecycle() {
    RpcHarness rpc;
    require(rpc.request(10, 50), "RPC request was not registered");
    auto paired=rpc.respond(10, {{"ok", true}});
    require(paired.state==RpcHarness::State::COMPLETED && std::get<bool>(paired.response.at("ok").data), "request/response correlation failed");
    std::cout << "rpc.lifecycle pairing: response matched correlation 10 [PASS]\n";
    require(rpc.request(11, 60), "timeout request was not registered");
    rpc.advance(60);
    require(rpc.state(11)==RpcHarness::State::TIMED_OUT, "deadline did not time out request");
    std::cout << "rpc.lifecycle timeout: TIMED_OUT [PASS]\n";
    auto late=rpc.respond(11);
    require(late.error==EntityError::TIMEOUT, "late response was not discarded as TIMEOUT");
    std::cout << "rpc.lifecycle late_response: TIMEOUT [PASS]\n";
    require(rpc.request(12, 100), "duplicate response request was not registered");
    require(rpc.respond(12).state==RpcHarness::State::COMPLETED, "first response failed");
    require(rpc.respond(12).error==EntityError::CONFLICT, "duplicate response was not CONFLICT");
    std::cout << "rpc.lifecycle duplicate_response: CONFLICT [PASS]\n";
    require(rpc.request(13, 100), "disconnect request was not registered");
    rpc.disconnect();
    require(rpc.state(13)==RpcHarness::State::DISCONNECTED && rpc.respond(13).error==EntityError::CANCELLED,
            "disconnect did not cancel in-flight request");
    std::cout << "rpc.lifecycle disconnect_inflight: CANCELLED [PASS]\n";
    require(rpc.respond(999).error==EntityError::STALE, "unknown response was not discarded");
    std::cout << "rpc.lifecycle unknown_response: STALE [PASS]\n";
    const EntityContract contract(R"(<Contracts><Entity name="Avatar" id="1"><ServerMethods><Method name="get" id="2" mode="request" caller="server"><Arg name="account" id="0" type="INT32"/><Returns><Field name="level" id="1" type="UINT16"/></Returns></Method></ServerMethods></Entity></Contracts>)");
    EntityRuntime runtime(contract);
    const EntityTarget target{1,1,3,42};
    runtime.dispatcher().bind("Avatar", "server", 2, [](const EntityCaller&,const EntityTarget&,const EntityValues&) { return EntityValues{{"level",std::int64_t{7}}}; });
    require(runtime.spawn(target), "entity spawn failed");
    EntityInvocation invocation{target,{"server",9},"server",2,true,{{"account",std::int64_t{4}}}};
    auto routed=runtime.deliver(invocation, 1);
    require(routed.ok && std::get<std::int64_t>(routed.response.at("level").data)==7, "entity route/dispatch/response failed");
    require(runtime.deliver(invocation, 1).error==EntityError::CONFLICT, "duplicate delivery not deterministic");
    require(runtime.deliver(EntityInvocation{{1,1,4,42},{"server",9},"server",2,true,{{"account",std::int64_t{4}}}}, 2).error==EntityError::STALE,
            "wrong incarnation not STALE");
    require(runtime.destroy(target) && runtime.deliver(invocation, 3).error==EntityError::UNROUTED, "destroyed entity still routed");
    std::cout << "entity.runtime route_dispatch_response: typed level=7 [PASS]\n";
    std::cout << "entity.runtime duplicate_delivery: CONFLICT [PASS]\n";
    std::cout << "entity.runtime stale_incarnation: STALE [PASS]\n";
}
void entity_envelope() {
    const EntityEnvelope envelope{7, 1, 3, 42, kEntitySideServer, 2, 0x1122334455667788ULL, 5, 250, "payload"};
    const auto bytes = encode_entity_envelope(envelope);
    require(bytes.size() == kEntityEnvelopeHeader + envelope.payload.size(), "envelope size is not fixed header plus payload");
    std::ostringstream hex;
    hex << std::hex << std::setfill('0');
    for (const auto c : bytes) hex << std::setw(2) << static_cast<int>(static_cast<unsigned char>(c));
    require(hex.str() == "0000000700000001000000000000002a00000003010002112233445566778800000005000000fa7061796c6f6164",
            "envelope canonical bytes are not stable");
    auto decoded = decode_entity_envelope(bytes);
    require(decoded && *decoded == envelope, "envelope round trip lost fields");
    std::cout << "entity.envelope canonical_bytes: fixed big-endian header [PASS]\n";
    for (const std::size_t cut : {std::size_t{0}, std::size_t{1}, std::size_t{19}, std::size_t{20}, std::size_t{22},
                                  std::size_t{23}, std::size_t{30}, std::size_t{31}, std::size_t{34}, std::size_t{35},
                                  std::size_t{38}})
        require(!decode_entity_envelope(bytes.substr(0, cut)), "truncated envelope decoded");
    auto bad_side = envelope;
    bad_side.side = 2;
    require(!decode_entity_envelope(encode_entity_envelope(bad_side)), "unknown side byte decoded");
    std::cout << "entity.envelope shape: truncation and unknown side rejected [PASS]\n";
    const EntityEnvelope wide{0xffffffffu, 63, 0xffffffffu, -3, kEntitySideClient, 254, 0xffffffffffffffffULL,
                              0xfffffffeu, 0xffffffffu, {}};
    auto wide_decoded = decode_entity_envelope(encode_entity_envelope(wide));
    require(wide_decoded && *wide_decoded == wide && wide_decoded->entity_id == -3, "extreme field values did not round trip");
    require(to_entity_invocation(*wide_decoded, {"client", 9}).side == "client", "client side did not map to invocation");
    std::cout << "entity.envelope extremes: i64 entity id and u64 correlation round trip [PASS]\n";
    auto returns = decode_entity_reply(encode_entity_reply({false, EntityError::INVALID, {}, "returns"}).value());
    require(returns && !returns->is_error && returns->payload == "returns", "returns reply did not round trip");
    require(!encode_entity_reply({true, EntityError::STALE, "not current", {}}), "only INVALID may carry a reason");
    auto stale = decode_entity_reply(encode_entity_reply({true, EntityError::STALE, {}, {}}).value());
    require(stale && stale->is_error && stale->error == EntityError::STALE, "error reply did not round trip");
    auto explained = decode_entity_reply(encode_entity_reply({true, EntityError::INVALID, "bad shape", {}}).value());
    require(explained && explained->reason == "bad shape", "INVALID reason did not round trip");
    require(!decode_entity_reply("\x05" "x"), "unknown reply kind decoded");
    require(!decode_entity_reply("\x01\xc8"), "unknown error code decoded");
    require(!decode_entity_reply("\x01"), "truncated error reply decoded");
    require(!decode_entity_reply(""), "empty reply decoded");
    std::cout << "entity.envelope reply: one of returns or error [PASS]\n";
    const EntityContract contract(R"(<Contracts><Entity name="Avatar" id="1"><ServerMethods><Method name="get" id="2" mode="request" caller="server"><Arg name="account" id="0" type="INT32"/><Returns><Field name="level" id="1" type="UINT16"/></Returns></Method></ServerMethods></Entity></Contracts>)");
    EntityRuntime runtime(contract);
    const EntityTarget target{1, 1, 3, 42};
    runtime.dispatcher().bind("Avatar", "server", 2, [](const EntityCaller&,const EntityTarget&,const EntityValues&) { return EntityValues{{"level",std::int64_t{7}}}; });
    require(runtime.spawn(target, 5), "entity spawn with hosting generation failed");
    const EntityEnvelope internal{1, 1, 3, 42, kEntitySideServer, 2, 10, 5, 0, {}};
    auto invocation = to_entity_invocation(internal, {"server", 9});
    invocation.arguments = {{"account", std::int64_t{4}}};
    auto routed = runtime.deliver(invocation, internal.correlation_id);
    require(routed.ok && std::get<std::int64_t>(routed.response.at("level").data) == 7, "current-generation internal call did not route");
    auto late = internal;
    late.hosting_generation = 4;
    require(runtime.deliver(to_entity_invocation(late, {"server", 9}), 11).error == EntityError::STALE, "older hosting generation was not fenced");
    require(runtime.deliver(to_entity_invocation(late, {"server", 9}), 11).error == EntityError::STALE, "fenced call registered a delivery slot");
    auto retry = runtime.deliver(invocation, 11);
    require(retry.ok && std::get<std::int64_t>(retry.response.at("level").data) == 7, "fence consumed the correlation slot");
    auto early = internal;
    early.hosting_generation = 6;
    require(runtime.deliver(to_entity_invocation(early, {"server", 9}), 13).error == EntityError::STALE, "unknown hosting generation was not fenced");
    auto client_origin = invocation;
    client_origin.hosting_generation = 0;
    require(runtime.deliver(client_origin, 14).ok, "zero hosting generation was fenced");
    std::cout << "entity.envelope fence: internal generation asserted, zero bypasses [PASS]\n";
    std::cout << "entity.envelope fence_before_slot: STALE does not consume correlation [PASS]\n";
}
// Controlled property store over the contract descriptor (item 102, property
// half): defaults from the descriptor, typed writes through the dispatcher's
// type rules, a read-only reserved proxy for retired ids, an audience view
// for client surfaces, and a Lua binding that exposes the same store through
// live nested proxies for struct fields and array cells.
void entity_properties() {
    const EntityContract contract(R"(<Contracts><Types>
        <Item><itemId id="0" type="UINT32"/><count id="1" type="UINT32" default="2"/></Item>
        <Inventory><capacity id="0" type="UINT16" default="10"/><items id="1" type="ARRAY" of="Item"/></Inventory>
        </Types>
        <Entity name="Avatar" id="1">
          <Properties>
            <level id="1" type="UINT16" default="1" flags="OWN_CLIENT"/>
            <name id="2" type="STRING" flags="OWN_CLIENT"/>
            <gold id="3" type="INT32" default="-5" flags="ALL_CLIENTS"/>
            <active id="4" type="BOOL" default="true"/>
            <bag id="5" type="Item"/>
            <tags id="6" type="ARRAY" of="UINT32"/>
            <blob id="7" type="BYTES"/>
            <inventory id="8" type="Inventory"/>
          </Properties>
          <Reserved><Property id="9"/></Reserved>
        </Entity>
        <Entity name="Monster" id="2">
          <Properties><hp id="1" type="UINT32" flags="ALL_CLIENTS"/></Properties>
        </Entity></Contracts>)");
    PropertyStore store(contract, 1);
    // Defaults come from the descriptor: explicit defaults win, otherwise the
    // type rules apply (zero/false/empty string, empty array, per-field struct).
    const auto* level = store.get("level");
    require(level && std::get<std::int64_t>(level->data) == 1, "explicit UINT16 default lost");
    require(store.get("name") && std::get<std::string>(store.get("name")->data).empty(), "STRING default is not empty");
    require(std::get<std::int64_t>(store.get("gold")->data) == -5, "negative INT32 default lost");
    require(std::get<bool>(store.get("active")->data) == true, "BOOL default lost");
    const auto* bag = store.get("bag");
    require(bag && std::get<std::int64_t>(std::get<EntityObject>(bag->data).at("count").data) == 2 &&
            std::get<std::int64_t>(std::get<EntityObject>(bag->data).at("itemId").data) == 0,
            "struct default did not fill per-field defaults");
    require(store.get("tags") && std::get<EntityArray>(store.get("tags")->data).empty(), "ARRAY default is not empty");
    require(std::get<std::string>(store.get("blob")->data).empty(), "BYTES default is not empty");
    std::cout << "entity.properties defaults: descriptor-driven [PASS]\n";
    // Controlled writes: every value passes the dispatcher's type rules.
    require(store.set("level", std::int64_t{65535}), "valid UINT16 write refused");
    require(!store.set("level", std::int64_t{65536}) && store.error().find("UINT16") != std::string::npos,
            "UINT16 overflow accepted: "+store.error());
    require(!store.set("level", std::string("oops")) && store.error().find("level") != std::string::npos,
            "kind mismatch accepted");
    require(!store.set("nope", std::int64_t{1}) && store.error().find("not a declared property") != std::string::npos,
            "unknown property accepted");
    require(!store.set("tags", std::string("not-an-array")), "scalar into ARRAY accepted");
    require(store.set("tags", EntityArray{std::int64_t{0}, std::int64_t{1}, std::int64_t{2}}),
            "valid array write refused");
    require(!store.set("tags", EntityArray{std::int64_t{0}, std::string("x")}),
            "wrong array element accepted");
    require(store.set("bag", EntityObject{{"itemId", std::int64_t{7}}, {"count", std::int64_t{3}}}),
            "valid struct write refused");
    require(!store.set("bag", EntityObject{{"itemId", std::int64_t{7}}}), "incomplete struct accepted");
    require(!store.set("bag", EntityObject{{"itemId", std::int64_t{7}}, {"count", std::int64_t{3}}, {"extra", std::int64_t{1}}}),
            "extra struct field accepted");
    require(std::get<EntityArray>(store.get("tags")->data).size() == 3, "refused writes must not land");
    std::cout << "entity.properties typed writes: range/shape/unknown refusals [PASS]\n";
    // Read-only reserved proxy: retired ids stay addressable, never writable.
    require(store.reserved(9), "reserved id not reported");
    require(!store.reserved(1), "live id reported as reserved");
    require(store.by_id(9) == nullptr, "reserved id served a value");
    require(!store.set_by_id(9, std::int64_t{1}) && store.error().find("read-only") != std::string::npos,
            "write to a reserved id accepted: "+store.error());
    require(store.by_id(1) == store.get("level"), "id-addressed access disagrees with name access");
    require(store.set_by_id(1, std::int64_t{42}), "declared id write refused");
    require(std::get<std::int64_t>(store.get("level")->data) == 42, "id-addressed write lost");
    require(!store.set_by_id(77, std::int64_t{1}), "unknown id write accepted");
    std::cout << "entity.properties reserved proxy: addressable, read-only [PASS]\n";
    // Audience view: OWN_CLIENT+ALL_CLIENTS for the owner, ALL_CLIENTS for
    // others, SERVER_ONLY for neither.
    const auto owner = store.client_view(true);
    const auto guest = store.client_view(false);
    require(owner.count("level") == 1 && owner.count("name") == 1 && owner.count("gold") == 1,
            "owner view lost a client-visible property");
    require(owner.count("active") == 0 && owner.count("bag") == 0 && owner.count("tags") == 0 &&
            owner.count("blob") == 0, "SERVER_ONLY properties leaked into the owner view");
    require(guest.count("gold") == 1 && guest.count("level") == 0 && guest.count("name") == 0,
            "guest view crossed the OWN_CLIENT boundary");
    std::cout << "entity.properties audience view: owner/guest filtering [PASS]\n";
    // One runtime for every entity type: Monster shares the machinery.
    PropertyStore monster(contract, 2);
    require(std::get<std::int64_t>(monster.get("hp")->data) == 0, "Monster default lost");
    require(monster.set("hp", std::int64_t{100}), "Monster write refused");
    require(std::get<std::int64_t>(store.get("level")->data) == 42, "stores share state");
    require(store.set("level", std::int64_t{50}), "write refused");
    require(std::get<std::int64_t>(monster.get("hp")->data) == 100, "Avatar write reached Monster");
    bool unknown_type = false;
    try { PropertyStore(contract, 99); } catch (const std::invalid_argument&) { unknown_type = true; }
    require(unknown_type, "unknown entity type id was accepted");
    std::cout << "entity.properties per-type stores: Avatar/Monster share one runtime [PASS]\n";
    // Lua binding: the same store, same rules, table-style access.
    lua_State* L = luaL_newstate();
    require(L != nullptr, "cannot open Lua for the property binding");
    luaL_openlibs(L);
    require(push_entity_properties(L, &store), "property binding refused a bindable store");
    lua_setglobal(L, "props");
    const auto eval = [](lua_State* state, const char* code) {
        if (luaL_dostring(state, code) != LUA_OK) {
            const char* message = lua_tostring(state, -1);
            lua_pop(state, 1);
            return std::string("ERR:")+std::string(message ? message : "unknown Lua error");
        }
        std::string result = "nil";
        if (lua_isboolean(state, -1)) result = lua_toboolean(state, -1) ? "true" : "false";
        else if (lua_isinteger(state, -1)) result = std::to_string(lua_tointeger(state, -1));
        else if (lua_isstring(state, -1)) { std::size_t n=0; const char* p=lua_tolstring(state,-1,&n); result.assign(p, n); }
        lua_pop(state, 1);
        return result;
    };
    const auto first_read = eval(L, "return props.level");
    require(first_read == "50", "Lua read did not see the C++ value: got "+first_read);
    require(eval(L, "props.level = 5; return props.level") == "5", "Lua write did not stick");
    require(std::get<std::int64_t>(store.get("level")->data) == 5, "Lua write missed the store");
    require(eval(L, "return props.gold") == "-5", "negative default misread from Lua");
    require(eval(L, "props.active = false; return props.active") == "false", "BOOL round trip failed");
    require(eval(L, "props.name = 'hero'; return props.name") == "hero", "STRING round trip failed");
    require(eval(L, "props.tags = {1, 2, 3}; return #props.tags") == "3", "array write did not land");
    require(std::get<EntityArray>(store.get("tags")->data).size() == 3, "Lua array write missed the store");
    require(eval(L, "props.bag = {itemId = 7, count = 9}; return props.bag.count") == "9", "struct write did not land");
    require(eval(L, "props.level = 70000").find("UINT16") != std::string::npos, "Lua range violation silenced");
    require(eval(L, "props.missing = 1").find("not a declared property") != std::string::npos, "Lua unknown write silenced");
    require(eval(L, "return props.missing").find("not a declared property") != std::string::npos, "Lua unknown read silenced");
    require(eval(L, "props.tags = {1, 'x'}").find("tags") != std::string::npos, "Lua array element violation silenced");
    require(eval(L, "props.bag = {itemId = 7}").find("bag") != std::string::npos, "Lua struct shape violation silenced");
    require(eval(L, "props.names = {}").find("read-only") != std::string::npos, "method key was writable");
    require(eval(L, "return #props:names()") == "8", "names() lost a property");
    require(eval(L, "return props:reserved()[1]") == "9", "reserved() lost the retired id");
    require(eval(L, "return props:view(true).level") == "5", "owner view lost a visible property");
    require(eval(L, "return props:view(true).gold") == "-5", "owner view lost ALL_CLIENTS");
    require(eval(L, "local v = props:view(true); return v.active") == "nil", "owner view leaked a SERVER_ONLY property");
    require(eval(L, "local v = props:view(false); return v.gold") == "-5", "guest view lost an ALL_CLIENTS property");
    require(eval(L, "local v = props:view(false); return v.level") == "nil", "guest view crossed OWN_CLIENT");
    // Nested controlled proxies: struct fields and array cells address the
    // live store instead of copies, so nothing nested is ever detached, and
    // arrays grow only through the explicit :insert/:append/:remove methods.
    store.take_changes();
    require(eval(L, "props.bag.itemId = 11; return props.bag.itemId") == "11", "nested struct field write did not land");
    require(std::get<std::int64_t>(store.get_path("bag", {TreeStep{false, "itemId", 0}})->data) == 11,
            "nested Lua write missed the store");
    require(eval(L, "props.tags[2] = 42; return props.tags[2]") == "42", "array cell write did not land");
    require(std::get<std::int64_t>(store.get_path("tags", {TreeStep{true, "", 1}})->data) == 42,
            "array cell Lua write missed the store");
    require(eval(L, "props.tags:append(7); return #props.tags") == "4", ":append did not grow the array");
    require(eval(L, "props.tags:insert(1, 99); return props.tags[1]") == "99", ":insert did not shift elements");
    require(eval(L, "props.tags:remove(1); return #props.tags") == "4", ":remove did not shrink the array");
    require(eval(L, "return props.tags[1]") == "1", ":remove lost the shifted element");
    require(eval(L, "return props.tags[9]") == "nil", "absent array cells must read as nil");
    require(eval(L, "props.inventory.items:append({itemId = 3, count = 1}); return #props.inventory.items") == "1",
            "array-of-struct append did not land");
    const TreePath deep{TreeStep{false, "items", 0}, TreeStep{true, "", 0}, TreeStep{false, "count", 0}};
    require(eval(L, "props.inventory.items[1].count = 5; return props.inventory.items[1].count") == "5",
            "three-level nested write did not land");
    require(std::get<std::int64_t>(store.get_path("inventory", deep)->data) == 5,
            "three-level Lua write missed the store");
    require(eval(L, "props.bag.nope = 1").find("unknown") != std::string::npos, "unknown nested field silenced");
    require(eval(L, "props.bag.count = 'x'").find("count") != std::string::npos, "nested field type violation silenced");
    require(eval(L, "props.tags[9] = 1").find("range") != std::string::npos, "out-of-range array write silenced");
    require(eval(L, "props.tags[1] = nil").find("remove") != std::string::npos, "nil array write silenced");
    require(eval(L, "props.tags.grow = 1").find("index") != std::string::npos, "string key on array silenced");
    // The journal sees Lua writes exactly like C++ ones, in application order.
    const auto journal = store.take_changes();
    require(journal.size() == 7, "nested Lua writes did not journal in application order");
    require(journal[0].kind == TreeChange::Kind::SET && journal[0].property == "bag" &&
            journal[0].path == TreePath{TreeStep{false, "itemId", 0}} &&
            std::get<std::int64_t>(journal[0].value.data) == 11, "nested SET entry lost");
    require(journal[2].kind == TreeChange::Kind::INSERT && journal[2].property == "tags" && journal[2].position == 3,
            "append did not journal at the end");
    require(journal[4].kind == TreeChange::Kind::REMOVE && journal[4].property == "tags" && journal[4].position == 0,
            "remove did not journal at the head");
    require(journal[5].kind == TreeChange::Kind::INSERT && journal[5].property == "inventory" &&
            journal[5].path == TreePath{TreeStep{false, "items", 0}} && journal[5].position == 0 &&
            std::get<std::int64_t>(std::get<EntityObject>(journal[5].value.data).at("itemId").data) == 3,
            "nested INSERT entry lost its payload");
    require(journal[6].kind == TreeChange::Kind::SET && journal[6].path == deep &&
            std::get<std::int64_t>(journal[6].value.data) == 5, "deep SET entry lost");
    std::cout << "entity.properties nested proxies: fields, cells, growth, journal [PASS]\n";
    lua_close(L);
    std::cout << "entity.properties lua binding: table access, shared refusals, views [PASS]\n";
    // A property named like a binding method key must be refused at push time,
    // not silently shadowed.
    const EntityContract clash(R"(<Contracts><Entity name="Weird" id="1"><Properties><view id="1" type="UINT16"/></Properties></Entity></Contracts>)");
    PropertyStore weird(clash, 1);
    lua_State* W = luaL_newstate();
    require(W != nullptr, "cannot open Lua for the clash probe");
    require(!push_entity_properties(W, &weird), "a method-key property was silently shadowed");
    lua_close(W);
    std::cout << "entity.properties binding clash: refused at push [PASS]\n";
    // Safe Lua replacement under an unchanged contract (item 118's
    // foundation): the store, journal and revision are the entity — a
    // rebuilt Lua state rebinds fresh proxies over the same storage, prior
    // writes survive it, and the journal keeps its order across the swap.
    store.set("level", std::int64_t{60});
    lua_State* R = luaL_newstate();
    require(R != nullptr, "cannot open Lua for the replacement probe");
    luaL_openlibs(R);
    require(push_entity_properties(R, &store), "replacement binding refused");
    lua_setglobal(R, "props");
    const auto after_swap = eval(R, "return props.level");
    require(after_swap == "60", "the replaced state lost C++-side writes: got "+after_swap);
    require(eval(R, "props.level = 61; return props.level") == "61", "write through the replaced state failed");
    require(std::get<std::int64_t>(store.get("level")->data) == 61, "the replaced state missed the store");
    require(eval(R, "return props.tags[1] .. ':' .. #props.inventory.items") == "1:1",
            "nested state did not survive the replacement");
    lua_close(R);
    // The old state's handles died with it; the store still stands and the
    // journal collapsed the swap-spanning writes to the newest value.
    const auto across = store.take_changes();
    require(across.size() == 1 && across[0].property == "level" && across[0].path.empty() &&
            std::get<std::int64_t>(across[0].value.data) == 61,
            "journal lost the writes spanning the state replacement");
    std::cout << "entity.properties lua replacement: state rebuilt, entity intact [PASS]\n";
}
// Bounded value tree over the controlled store (item 106 core, the
// prerequisite of the nested-delta items): mutations through explicit paths
// journal ordered change entries, a whole-property set absorbs the entries
// recorded inside the replaced subtree (path merge across structural
// changes), structural ops hand out generation-counted container handles
// that die on the next structural change, and take_changes/rebaseline drain
// or drop the backlog.
void entity_tree() {
    const EntityContract contract(R"(<Contracts><Types>
        <Item><itemId id="0" type="UINT32"/><count id="1" type="UINT32" default="1"/></Item>
        <Inventory><capacity id="0" type="UINT16"/><items id="1" type="ARRAY" of="Item"/></Inventory>
        </Types>
        <Entity name="Avatar" id="1">
          <Properties>
            <level id="1" type="UINT16" default="1" flags="OWN_CLIENT"/>
            <inventory id="2" type="Inventory"/>
            <tags id="3" type="ARRAY" of="UINT32"/>
          </Properties>
        </Entity></Contracts>)");
    using Kind = TreeChange::Kind;
    PropertyStore store(contract, 1);
    const auto field = [](const char* name) { return TreeStep{false, name, 0}; };
    const auto index = [](std::size_t at) { return TreeStep{true, std::string(), at}; };
    // Nested writes through explicit paths: arrays grow only by ordered
    // inserts, struct fields must exist, and every change lands in the
    // journal in application order.
    require(store.insert_at("inventory", {field("items")}, 0,
                EntityValue(EntityObject{{"itemId", std::int64_t{7}}, {"count", std::int64_t{1}}})) &&
            store.insert_at("inventory", {field("items")}, 1,
                EntityValue(EntityObject{{"itemId", std::int64_t{9}}, {"count", std::int64_t{2}}})),
            "ordered inserts into a nested array refused");
    require(store.set_path("inventory", {field("items"), index(1), field("count")}, std::int64_t{5}),
            "nested struct-in-array write refused");
    const auto* count = store.get_path("inventory", {field("items"), index(1), field("count")});
    require(count && std::get<std::int64_t>(count->data) == 5, "nested write did not land");
    require(store.insert_at("tags", {}, std::size_t{0}, std::int64_t{30}) &&
            store.insert_at("tags", {}, std::size_t{1}, std::int64_t{31}) &&
            store.insert_at("tags", {}, std::size_t{2}, std::int64_t{32}),
            "scalar array inserts refused");
    require(store.set_path("tags", {index(1)}, std::int64_t{9}), "array element write refused");
    auto changes = store.take_changes();
    require(changes.size() == 7, "journal lost an entry");
    require(changes[0].kind == Kind::INSERT && changes[0].property == "inventory" &&
            changes[0].path.size() == 1 && changes[0].position == 0, "first insert entry wrong");
    require(changes[2].kind == Kind::SET && changes[2].path.size() == 3 &&
            std::get<std::int64_t>(changes[2].value.data) == 5, "nested set entry wrong");
    require(changes[6].kind == Kind::SET && changes[6].path.size() == 1 &&
            std::get<std::int64_t>(changes[6].value.data) == 9, "array element entry wrong");
    require(store.take_changes().empty(), "take_changes did not drain the journal");
    std::cout << "entity.tree path writes: ordered journal [PASS]\n";
    // Path merge: a whole-property set absorbs every entry recorded inside
    // the replaced subtree; a later nested change under an already-set
    // ancestor stays behind it.
    require(store.set_path("tags", {index(1)}, std::int64_t{90}), "second element write refused");
    require(store.set_path("tags", {}, EntityArray{std::int64_t{1}, std::int64_t{2}, std::int64_t{3}}),
            "whole-property write refused");
    require(store.set_path("tags", {index(1)}, std::int64_t{91}), "nested write after whole refused");
    changes = store.take_changes();
    require(changes.size() == 2 && changes[0].kind == Kind::SET && changes[0].path.empty() &&
            changes[1].path.size() == 1, "path merge did not absorb subtree entries");
    require(store.set_path("tags", {index(1)}, std::int64_t{92}) &&
            store.set_path("tags", {index(1)}, std::int64_t{93}), "same-path writes refused");
    changes = store.take_changes();
    require(changes.size() == 1 && std::get<std::int64_t>(changes[0].value.data) == 93,
            "same-path set did not collapse");
    require(store.set("level", std::int64_t{2}), "plain set refused");
    changes = store.take_changes();
    require(changes.size() == 1 && changes[0].property == "level" && changes[0].path.empty(),
            "plain set did not journal");
    std::cout << "entity.tree path merge: subtree absorption, same-path collapse [PASS]\n";
    // Structural changes hand out container handles that die on the next
    // structural change of that property; nested writes keep them alive.
    auto handle = store.handle("tags");
    require(store.valid(handle), "fresh handle invalid");
    require(store.insert_at("tags", {}, std::size_t{3}, std::int64_t{33}), "append refused");
    require(!store.valid(handle), "structural change did not invalidate the handle");
    handle = store.handle("tags");
    require(store.set_path("tags", {index(0)}, std::int64_t{5}), "nested write refused");
    require(store.valid(handle), "nested write invalidated a handle");
    require(store.set("tags", EntityArray{std::int64_t{1}}), "whole set refused");
    require(!store.valid(handle), "whole-property set did not invalidate the handle");
    std::cout << "entity.tree handles: structural invalidation only [PASS]\n";
    // Boundedness: arrays grow only by insert, indexes are bounds-checked,
    // wrong kinds and unknown fields are refused without landing.
    require(!store.insert_at("tags", {}, std::size_t{5}, std::int64_t{1}), "insert beyond size+1 accepted");
    require(!store.remove_at("tags", {}, std::size_t{9}), "remove beyond size accepted");
    require(!store.set_path("tags", {index(9)}, std::int64_t{1}), "out-of-range element write accepted");
    require(!store.set_path("inventory", {field("capacity")}, std::string("big")),
            "wrong kind into struct field accepted");
    require(!store.set_path("inventory", {field("nope")}, std::int64_t{1}), "unknown field accepted");
    require(!store.set_path("nope", {}, std::int64_t{1}), "unknown property accepted");
    require(!store.set_path("inventory", {field("items"), index(0), field("itemId")}, std::int64_t{-1}),
            "negative element write accepted: "+store.error());
    require(store.error().find("itemId") != std::string::npos, "nested refusal did not name the path");
    require(std::get<std::int64_t>(store.get_path("inventory", {field("items"), index(0), field("itemId")})->data) == 7,
            "refused nested write landed");
    std::cout << "entity.tree bounds: indexes, kinds, unknown fields, no partial land [PASS]\n";
    // Copy assignment: values handed out are snapshots; a later write never
    // rewrites a copy, and no container is shared across properties.
    const auto snapshot = *store.get_path("inventory", {field("items"), index(0)});
    require(store.set_path("inventory", {field("items"), index(0), field("count")}, std::int64_t{77}),
            "write after snapshot refused");
    require(std::get<std::int64_t>(std::get<EntityObject>(snapshot.data).at("count").data) == 1,
            "snapshot aliased the stored value");
    require(std::get<EntityObject>(store.get("inventory")->data).at("items").data.index() ==
            std::get<EntityObject>(store.get("inventory")->data).at("items").data.index(),
            "container kind unstable");
    store.rebaseline();
    require(store.take_changes().empty(), "rebaseline left journal entries");
    std::cout << "entity.tree snapshots: copy assignment, rebaseline [PASS]\n";
}
// Baseline/delta delivery with observer filtering (items 104/105): observer
// entry establishes a visible full baseline; later drains of the journal
// travel as wire-shaped deltas (property id, field-id/0-based-index path,
// operation, value) numbered consecutively per observer; the replica applies
// a batch atomically or refuses the whole batch when sequences gap, and a
// resync re-establishes the baseline. SERVER_ONLY never leaves the server and
// OWN_CLIENT reaches owners only.
void entity_sync() {
    const EntityContract contract(R"(<Contracts><Types>
        <Item><itemId id="0" type="UINT32"/><count id="1" type="UINT32" default="2"/></Item>
        <Inventory><capacity id="0" type="UINT16" default="10"/><items id="1" type="ARRAY" of="Item"/></Inventory>
        </Types>
        <Entity name="Avatar" id="1">
          <Properties>
            <level id="1" type="UINT16" default="1" flags="OWN_CLIENT"/>
            <gold id="2" type="INT32" default="-5" flags="ALL_CLIENTS"/>
            <active id="3" type="BOOL" default="true"/>
            <tags id="4" type="ARRAY" of="UINT32" flags="ALL_CLIENTS"/>
            <bag id="5" type="Item"/>
            <inventory id="6" type="Inventory"/>
          </Properties>
        </Entity></Contracts>)");
    PropertyStore store(contract, 1);
    const auto field = [](const char* name) { return TreeStep{false, name, 0}; };
    const auto index = [](std::size_t at) { return TreeStep{true, std::string(), at}; };
    store.set("level", std::int64_t{5});
    store.set("gold", std::int64_t{100});
    store.set("bag", EntityObject{{"itemId", std::int64_t{7}}, {"count", std::int64_t{9}}});
    // Observer entry: one visible baseline per audience, nothing else.
    PropertyChannel channel(&store);
    require(channel.observe("owner", true) && channel.observe("guest", false), "observer entry refused");
    require(!channel.observe("guest", false), "duplicate observer accepted");
    const auto base = channel.deliver();
    require(base.size() == 2, "observer entry did not deliver two baselines");
    const auto& guest_base = base[0].kind == SyncMessage::Kind::BASELINE ? base[0] : base[1];
    const auto& owner_base = base[0].kind == SyncMessage::Kind::BASELINE ? base[1] : base[0];
    require(guest_base.kind == SyncMessage::Kind::BASELINE && owner_base.kind == SyncMessage::Kind::BASELINE,
            "entry delivered something other than baselines");
    require(guest_base.baseline.count("gold") == 1 && guest_base.baseline.count("tags") == 1 &&
            guest_base.baseline.count("level") == 0 && guest_base.baseline.count("bag") == 0 &&
            guest_base.baseline.count("active") == 0 && guest_base.baseline.count("inventory") == 0,
            "guest baseline crossed an audience boundary");
    require(owner_base.baseline.count("level") == 1 && owner_base.baseline.count("gold") == 1 &&
            owner_base.baseline.count("tags") == 1 && owner_base.baseline.count("bag") == 0 &&
            owner_base.baseline.count("inventory") == 0,
            "owner baseline lost a visible property or leaked a private one");
    SyncReceiver owner(contract, 1), guest(contract, 1);
    owner.rebaseline(owner_base.baseline);
    guest.rebaseline(guest_base.baseline);
    require(std::get<std::int64_t>(owner.replica().get("level")->data) == 5 &&
            std::get<std::int64_t>(guest.replica().get("level")->data) == 1,
            "baseline did not establish the replica state");
    std::cout << "entity.sync baseline: entry per audience [PASS]\n";
    // Delta filtering: one drain, per-observer batches, per-observer sequences.
    store.set_path("bag", {field("itemId")}, std::int64_t{11});          // SERVER_ONLY: nobody
    store.set("level", std::int64_t{6});                                  // OWN_CLIENT: owner
    store.insert_at("tags", {}, 0, std::int64_t{99});                     // ALL_CLIENTS: both
    store.set("gold", std::int64_t{200});                                 // ALL_CLIENTS: both
    const auto first = channel.deliver();
    require(first.size() == 2, "delta drain did not reach both observers");
    const SyncMessage* owner_batch = nullptr;
    const SyncMessage* guest_batch = nullptr;
    for (const auto& message : first)
        (message.deltas.size() == 3 ? owner_batch : guest_batch) = &message;
    require(owner_batch && guest_batch, "audience filtering lost a batch");
    require(owner_batch->deltas.size() == 3 && guest_batch->deltas.size() == 2,
            "delta batches did not respect the audience");
    require(owner_batch->sequence == 1 && guest_batch->sequence == 1 &&
            owner_batch->deltas.back().sequence == 3 && guest_batch->deltas.back().sequence == 2,
            "per-observer sequences are not consecutive from one");
    const auto& insert = guest_batch->deltas[0];
    require(insert.kind == SyncDelta::Kind::INSERT && insert.property_id == 4 &&
            insert.path.empty() && insert.position == 0,
            "structural delta lost its wire shape (property id, path, 0-based position)");
    const auto& gold_set = guest_batch->deltas[1];
    require(gold_set.kind == SyncDelta::Kind::SET && gold_set.property_id == 2 &&
            std::get<std::int64_t>(gold_set.value.data) == 200,
            "scalar delta lost its wire shape");
    require(owner.apply(owner_batch->deltas) && guest.apply(guest_batch->deltas), "valid batches refused");
    require(std::get<std::int64_t>(owner.replica().get("gold")->data) == 200 &&
            std::get<std::int64_t>(guest.replica().get("gold")->data) == 200,
            "deltas did not land on the replicas");
    require(std::get<std::int64_t>(guest.replica().get("level")->data) == 1 &&
            std::get<std::int64_t>(std::get<EntityObject>(guest.replica().get("bag")->data).at("itemId").data) == 0,
            "undelivered properties leaked into the guest replica");
    require(std::get<EntityArray>(guest.replica().get("tags")->data).size() == 1,
            "structural delta did not reach the replica");
    std::cout << "entity.sync deltas: audience filter, wire shape, sequences [PASS]\n";
    // Coalescing carries over: two consecutive scalar SETs deliver as one.
    store.set("gold", std::int64_t{1});
    store.set("gold", std::int64_t{2});
    const auto second = channel.deliver();
    for (const auto& message : second)
        require(message.deltas.size() == 1 &&
                std::get<std::int64_t>(message.deltas[0].value.data) == 2,
                "consecutive scalar writes did not coalesce to the latest value");
    require(guest.apply(second.front().deltas), "coalesced batch refused");
    require(std::get<std::int64_t>(guest.replica().get("gold")->data) == 2, "coalesced value lost");
    require(channel.deliver().empty(), "an empty journal still delivered");
    std::cout << "entity.sync coalescing: latest value per batch [PASS]\n";
    // Version filtering: a dropped message leaves a gap; the next batch is
    // refused whole, without partial landing, and a resync re-establishes
    // state.
    store.set("gold", std::int64_t{300});
    channel.deliver();  // the guest misses this batch
    store.set("gold", std::int64_t{400});
    const auto gapped = channel.deliver();
    const SyncMessage* guest_gap = nullptr;
    for (const auto& message : gapped)
        if (message.deltas.size() == 1) guest_gap = &message;
    require(guest_gap != nullptr, "gap drain lost the guest batch");
    require(!guest.apply(guest_gap->deltas), "a sequence gap was applied");
    require(guest.stale(), "gap did not mark the replica stale");
    require(std::get<std::int64_t>(guest.replica().get("gold")->data) == 2,
            "a refused batch landed partially");
    const auto resync = channel.resync("guest");
    require(resync.kind == SyncMessage::Kind::BASELINE &&
            std::get<std::int64_t>(resync.baseline.find("gold")->second.data) == 400,
            "resync baseline did not reflect current state");
    guest.rebaseline(resync.baseline);
    require(std::get<std::int64_t>(guest.replica().get("gold")->data) == 400, "resync lost state");
    store.set("gold", std::int64_t{500});
    const auto after = channel.deliver();
    require(after.size() == 2, "post-resync drain lost an observer");
    const SyncMessage* guest_next = nullptr;
    for (const auto& message : after)
        if (message.deltas.size() == 1 && message.deltas[0].sequence == 1) guest_next = &message;
    require(guest_next != nullptr, "resync did not restart the observer sequence");
    require(guest.apply(guest_next->deltas), "post-resync batch refused");
    require(std::get<std::int64_t>(guest.replica().get("gold")->data) == 500, "post-resync value lost");
    std::cout << "entity.sync version filter: gap refusal, atomicity, resync [PASS]\n";
    // A gap inside one batch refuses the whole batch: the valid first delta
    // must not land. The owner is at sequence 3 (the coalesced batch was
    // applied only by the guest), so a head at sequence 4 is admissible and
    // only the tail's jump to 6 betrays the tear.
    const std::vector<SyncDelta> torn = [] {
        std::vector<SyncDelta> batch;
        SyncDelta head;
        head.property_id = 2; head.kind = SyncDelta::Kind::SET;
        head.value = EntityValue(std::int64_t{999}); head.sequence = 4;
        batch.push_back(head);
        SyncDelta tail = head;
        tail.property_id = 1; tail.sequence = 6;
        batch.push_back(tail);
        return batch;
    }();
    require(!owner.apply(torn), "an internally torn batch was applied");
    require(std::get<std::int64_t>(owner.replica().get("gold")->data) == 200 && owner.sequence() == 3,
            "the valid half of a torn batch landed");
    // Departure removes the stream; re-entry establishes a new baseline.
    require(!channel.depart("ghost"), "unknown departure accepted");
    require(channel.depart("guest") && !channel.depart("guest"), "departure did not remove the observer");
    store.set("gold", std::int64_t{600});
    const auto post = channel.deliver();
    require(!post.empty(), "delta drain lost the remaining observer");
    for (const auto& message : post)
        require(message.deltas.size() == 1 && message.deltas[0].property_id == 2,
                "a departed observer still received deltas");
    require(channel.observe("guest", false), "re-entry refused");
    const auto reentry = channel.deliver();
    const SyncMessage* guest_reentry = nullptr;
    for (const auto& message : reentry)
        if (message.kind == SyncMessage::Kind::BASELINE) guest_reentry = &message;
    require(guest_reentry != nullptr &&
            std::get<std::int64_t>(guest_reentry->baseline.find("gold")->second.data) == 600,
            "re-entry did not establish a current baseline");
    std::cout << "entity.sync observers: departure, re-entry baseline [PASS]\n";
}
// Bounded internal-RPC queue: capacity refusals are typed, deadlines and
// cancellations surface, and a non-idempotent timeout can never be re-armed
// through the table — the outcome is unknown, so retry lives behind an
// explicit idempotent declaration and a fresh correlation id.
void entity_rpc() {
    RpcQueue queue(2);
    require(!queue.call(0, 100) && queue.error() == EntityError::INVALID, "zero correlation admitted");
    require(!queue.call(5, 0) && queue.error() == EntityError::INVALID, "zero deadline admitted");
    require(queue.call(1, 100) && queue.call(2, 100), "bounded queue refused its capacity");
    require(!queue.call(3, 100) && queue.error() == EntityError::CAPACITY, "overflow was not CAPACITY");
    require(queue.state(1) == RpcQueue::State::PENDING, "refused call disturbed an admitted call");
    require(!queue.call(1, 100) && queue.error() == EntityError::INVALID, "duplicate live correlation admitted");
    const auto done = queue.complete(1, {{"level", std::int64_t{7}}});
    require(done.state == RpcQueue::State::OK && std::get<std::int64_t>(done.response.at("level").data) == 7,
            "completion lost the response");
    require(queue.take(1).state == RpcQueue::State::OK, "retirement did not free the slot");
    require(queue.call(3, 100), "freed slot was not reusable");
    std::cout << "entity.rpc capacity: typed refusals, take frees slots [PASS]\n";

    RpcQueue timed(4);
    require(timed.call(10, 50) && timed.call(11, 500), "deadline section refused admission");
    timed.advance(50);
    require(timed.state(10) == RpcQueue::State::TIMED_OUT, "deadline did not burn the call");
    const auto late = timed.complete(10);
    require(late.error == EntityError::TIMEOUT, "late response was not discarded");
    require(timed.state(10) == RpcQueue::State::TIMED_OUT, "late response resurrected the call");
    require(timed.complete(999).error == EntityError::STALE, "unknown correlation was not STALE");
    require(timed.cancel(11), "pending call was not cancelled");
    require(!timed.cancel(999) && timed.error() == EntityError::INVALID, "unknown cancellation was silent");
    const auto cancelled = timed.complete(11);
    require(cancelled.state == RpcQueue::State::CANCELLED && cancelled.error == EntityError::CANCELLED,
            "cancelled call accepted a response");
    std::cout << "entity.rpc deadline_cancel: TIMEOUT discard, CANCELLED [PASS]\n";

    RpcQueue guarded(4);
    require(guarded.call(20, 50, false), "non-idempotent admission failed");
    guarded.advance(50);
    require(!guarded.retry(20, 21, 100), "timed-out non-idempotent call was re-armed");
    require(guarded.error() == EntityError::INVALID, "non-idempotent retry must be a typed refusal");
    require(!guarded.has(21), "refused retry allocated an admission");
    require(guarded.state(20) == RpcQueue::State::TIMED_OUT, "burned admission was not surfaced");
    require(guarded.take(20).state == RpcQueue::State::TIMED_OUT, "timed-out call was not collectable");
    require(!guarded.has(20), "take did not retire the entry");
    std::cout << "entity.rpc no_silent_retry: non-idempotent timeout stays surfaced [PASS]\n";

    RpcQueue re(4);
    require(re.call(30, 50, true), "idempotent admission failed");
    re.advance(50);
    require(re.retry(30, 31, 200), "idempotent timeout was not allowed a fresh admission");
    require(!re.has(30) && re.state(31) == RpcQueue::State::PENDING, "retry kept the old admission alive");
    require(!re.retry(31, 32, 200), "retry of a pending call was allowed");
    re.advance(200);
    require(!re.retry(31, 31, 300), "retry under the same correlation was allowed");
    require(re.retry(31, 32, 300), "second retry lost the idempotent marker");
    std::cout << "entity.rpc idempotent_retry: fresh correlation only [PASS]\n";

    const EntityContract contract(R"(<Contracts><Entity name="Avatar" id="1"><ServerMethods><Method name="get" id="2" mode="request" caller="server"><Arg name="account" id="0" type="INT32"/><Returns><Field name="level" id="1" type="UINT16"/></Returns></Method></ServerMethods></Entity></Contracts>)");
    EntityRuntime runtime(contract);
    const EntityTarget target{1, 1, 3, 42};
    runtime.dispatcher().bind("Avatar", "server", 2, [](const EntityCaller&, const EntityTarget&, const EntityValues&) {
        return EntityValues{{"level", std::int64_t{7}}};
    });
    require(runtime.spawn(target), "entity spawn failed");
    const EntityInvocation invocation{target, {"server", 9}, "server", 2, true, {{"account", std::int64_t{4}}}};
    RpcQueue link(3);
    require(link.call(40, 100, false), "first admission failed");
    require(runtime.deliver(invocation, 40).ok, "first attempt did not dispatch");
    link.advance(100);
    require(link.state(40) == RpcQueue::State::TIMED_OUT, "lost reply did not time out");
    require(runtime.deliver(invocation, 40).error == EntityError::CONFLICT,
            "burned correlation re-applied silently");
    require(!link.retry(40, 41, 100) && link.error() == EntityError::INVALID,
            "non-idempotent timeout was re-armed end-to-end");
    require(link.call(41, 300, true), "explicit fresh admission failed");
    const auto second = runtime.deliver(invocation, 41);
    require(second.ok, "fresh correlation did not dispatch");
    require(link.complete(41, second.response).state == RpcQueue::State::OK, "fresh attempt did not complete");
    std::cout << "entity.rpc runtime: burned id CONFLICT, fresh id completes [PASS]\n";
}
// Home-data persistence snapshot model: only persistent descriptor properties
// leave the store, a confirmed commit — never the send — clears dirty, and a
// conditional save carries the hosting generation and the observed revision so
// a save against a moved-on record refuses instead of overwriting.
void entity_persist() {
    const EntityContract contract(R"(<Contracts><Types>
        <Item><itemId id="0" type="UINT32"/><count id="1" type="UINT32" default="2"/></Item>
        </Types>
        <Entity name="Avatar" id="1">
          <Properties>
            <level id="1" type="UINT16" default="1" persistent="true"/>
            <gold id="2" type="UINT32" persistent="true"/>
            <bag id="3" type="Item" persistent="true"/>
            <title id="4" type="STRING"/>
          </Properties>
        </Entity></Contracts>)");
    PropertyStore store(contract, 1);
    PersistentStore p(store);

    const auto unassigned = p.save(1, 0);
    require(!unassigned.ok && unassigned.error == EntityError::NOT_READY, "save before assignment was not refused");
    require(p.assume(1) && !p.assume(1), "generation was not monotone on assume");
    require(p.set("level", EntityValue(std::int64_t{9})), "persistent write failed");
    require(p.set("title", EntityValue(std::string("knight"))), "transient write failed");
    require(p.revision() == 1, "a transient write advanced the persistent revision");
    require(p.set_path("bag", TreePath{TreeStep{false, "count", 0}}, EntityValue(std::int64_t{5})),
            "nested persistent write failed");
    require(p.revision() == 2 && p.dirty(), "nested persistent write was not marked dirty");
    require(!p.set("level", EntityValue(std::string("x"))), "malformed value was accepted");
    require(p.revision() == 2, "a refused write advanced the revision");
    std::cout << "entity.persist dirty: persistent-only revision, automatic flagging [PASS]\n";

    const auto stale_gen = p.save(2, p.revision());
    require(!stale_gen.ok && stale_gen.error == EntityError::STALE && stale_gen.reason == "generation",
            "a save under a foreign generation was not STALE");
    const auto stale_rev = p.save(1, 1);
    require(!stale_rev.ok && stale_rev.error == EntityError::CONFLICT && stale_rev.reason == "revision",
            "lost-update save was not CONFLICT");
    const auto snapshot = p.save(1, p.revision());
    require(snapshot.ok && snapshot.receipt.generation == 1 && snapshot.receipt.revision == 2,
            "save receipt lost its fence values");
    require(snapshot.snapshot.size() == 3 && !snapshot.snapshot.count("title"),
            "the snapshot leaked a non-persistent property");
    require(p.dirty(), "a send cleared dirty instead of a confirm");
    require(!p.confirm(999), "a foreign save id was confirmed");
    require(p.confirm(snapshot.receipt.save_id), "the honest confirm was refused");
    require(!p.dirty() && p.revision() == 2, "confirm did not land the committed revision");
    std::cout << "entity.persist save_gate: generation/revision fences, single entry [PASS]\n";

    require(p.set("gold", EntityValue(std::int64_t{100})), "post-commit write failed");
    const auto in_flight = p.save(1, p.revision());
    require(in_flight.ok, "a save after commit was refused");
    const auto second = p.save(1, p.revision());
    require(!second.ok && second.error == EntityError::CONFLICT && second.reason == "in-flight",
            "a second save overlapped an unconfirmed one");
    p.set("level", EntityValue(std::int64_t{10}));
    require(p.confirm(in_flight.receipt.save_id), "the pipelined confirm was refused");
    require(p.dirty(), "confirm cleared writes made after the save");
    std::cout << "entity.persist in_flight: one unconfirmed save, dirty survives send [PASS]\n";

    PropertyStore migrated_store(contract, 1);
    PersistentStore migrated(migrated_store);
    const auto clean = migrated.restore(1, 0, snapshot.snapshot);
    require(clean.ok && migrated.generation() == 1 && migrated.revision() == 0 && !migrated.dirty(),
            "a clean restore did not re-establish the record");
    const auto no_owner = migrated.restore(1, 1, {{"title", EntityValue(std::string("knight"))}});
    require(!no_owner.ok && no_owner.refused == "title", "a non-persistent property rode the snapshot");
    require(migrated.generation() == 1, "a refused restore advanced the assignment");
    require(migrated.assume(2), "a higher generation was not accepted");
    const auto stale_restore = migrated.restore(1, 0, snapshot.snapshot);
    require(!stale_restore.ok && stale_restore.error == EntityError::STALE, "a lower generation restore was accepted");
    const EntityContract grown(R"(<Contracts><Types>
        <Item><itemId id="0" type="UINT32"/><count id="1" type="UINT32" default="2"/></Item>
        </Types>
        <Entity name="Avatar" id="1">
          <Properties>
            <level id="1" type="UINT16" default="1" persistent="true"/>
            <gold id="2" type="UINT32" persistent="true"/>
            <bag id="3" type="Item" persistent="true"/>
            <mails id="4" type="UINT32" persistent="true"/>
          </Properties>
        </Entity></Contracts>)");
    PropertyStore grown_store(grown, 1);
    PersistentStore grown_p(grown_store);
    const auto converted = grown_p.restore(2, 2, snapshot.snapshot);
    require(converted.ok && converted.defaulted.size() == 1 && converted.defaulted[0] == "mails",
            "a grown contract did not default the new persistent property");
    require(grown_p.set("mails", EntityValue(std::int64_t{3})) && grown_p.revision() == 3,
            "the defaulted property was not writable");
    std::cout << "entity.persist restore: version conversion, whole-load refusal [PASS]\n";
}
namespace {
constexpr std::string_view echo = "return {message=function(p) return p end}";
class RawSocket {
public:
    explicit RawSocket(unsigned port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        require(fd_ >= 0, "test socket failed");
        timeval timeout{3, 0};
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        sockaddr_in address{};
        address.sin_family = AF_INET; address.sin_port = htons(static_cast<std::uint16_t>(port));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            ::close(fd_); throw std::runtime_error("raw TCP connect failed");
        }
    }
    ~RawSocket() { ::close(fd_); }
    void send(std::string_view value) {
        while (!value.empty()) {
            auto n = ::send(fd_, value.data(), value.size(), MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            require(n > 0, "raw TCP send failed");
            value.remove_prefix(static_cast<std::size_t>(n));
        }
    }
    std::string receive(std::size_t size) {
        std::string result(size, '\0');
        std::size_t offset = 0;
        while (offset < size) {
            auto n = ::recv(fd_, result.data()+offset, size-offset, 0);
            if (n < 0 && errno == EINTR) continue;
            require(n > 0, "raw TCP closed or timed out");
            offset += static_cast<std::size_t>(n);
        }
        return result;
    }
    std::string receive_line() {
        std::string result;
        for (;;) {
            char ch;
            auto n = ::recv(fd_, &ch, 1, 0);
            if (n < 0 && errno == EINTR) continue;
            require(n > 0, "raw TCP closed or timed out");
            if (ch == '\n') break;
            result.push_back(ch);
        }
        return result;
    }
    bool closed() {
        char ch;
        auto n = ::recv(fd_, &ch, 1, 0);
        return n == 0 || (n < 0 && errno == ECONNRESET);
    }
    bool closed_now() {
        char ch;
        auto n = ::recv(fd_, &ch, 1, MSG_DONTWAIT);
        return n == 0 || (n < 0 && errno == ECONNRESET);
    }
    // True when at least one byte has arrived without blocking.
    bool pending() {
        int bytes = 0;
        return ::ioctl(fd_, FIONREAD, &bytes) == 0 && bytes > 0;
    }
private:
    int fd_;
};
struct Http {
    long status = 0;
    std::string body, headers;
};
Http http(const std::string& url, const std::string& method = "GET",
          const std::string& payload = {}, const std::string& ca = {}, bool metadata = false) {
    static const int runtime = [] { require(curl_global_init(CURL_GLOBAL_DEFAULT) == 0, "curl initialization failed"); return 1; }();
    (void)runtime;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), curl_easy_cleanup);
    require(static_cast<bool>(handle), "curl handle failed");
    Http result;
    curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    if (!ca.empty()) curl_easy_setopt(handle.get(), CURLOPT_CAINFO, ca.c_str());
    auto append = +[](char* bytes, std::size_t width, std::size_t count, void* context) -> std::size_t {
        auto& target = *static_cast<std::string*>(context);
        if (width && count > (1024*1024)/width) return 0;
        const auto n = width*count;
        if (target.size()+n > 1024*1024) return 0;
        target.append(bytes, n); return n;
    };
    curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, append);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(handle.get(), CURLOPT_HEADERFUNCTION, append);
    curl_easy_setopt(handle.get(), CURLOPT_HEADERDATA, &result.headers);
    if (method == "POST") {
        curl_easy_setopt(handle.get(), CURLOPT_POST, 1L);
        curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, payload.data());
        curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(payload.size()));
    }
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(
        metadata ? curl_slist_append(nullptr, "x-test: yes") : nullptr, curl_slist_free_all);
    if (headers) curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers.get());
    const auto error = curl_easy_perform(handle.get());
    if (error != CURLE_OK) throw std::runtime_error(curl_easy_strerror(error));
    curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &result.status);
    return result;
}
std::vector<std::string> tls(const Context& c) {
    return {"--cert", c.path("cert.pem").string(), "--key", c.path("key.pem").string()};
}
// Socket buffers can absorb megabytes before the server-side send bound sees
// anything. Size the flood past tcp_wmem + tcp_rmem plus the 2 MiB bound.
std::string flood_pushes() {
    const auto max_field = [](const char* path) -> std::size_t {
        std::ifstream in(path);
        if (!in) return 6ull*1024ull*1024ull; // linux default-ish fallback
        std::size_t value = 0, item = 0;
        while (in >> item) value = std::max(value, item);
        return value;
    };
    const auto bytes = max_field("/proc/sys/net/ipv4/tcp_wmem")
        + max_field("/proc/sys/net/ipv4/tcp_rmem") + 3*max_frame;
    const auto pushes = std::min<std::size_t>(64, (bytes + max_frame - 1)/max_frame);
    return "flood " + std::to_string(std::max<std::size_t>(pushes, 8));
}
// Minimal websocket client over a raw socket: enough for lifecycle checks that
// the request/response cli client cannot do (registry state, unread floods).
class WsRaw {
public:
    explicit WsRaw(unsigned port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        require(fd_ >= 0, "websocket socket failed");
        timeval timeout{5, 0};
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        sockaddr_in address{};
        address.sin_family = AF_INET; address.sin_port = htons(static_cast<std::uint16_t>(port));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            ::close(fd_); throw std::runtime_error("websocket connect failed");
        }
        send_all("GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
        std::string head;
        char ch;
        while (!head.ends_with("\r\n\r\n")) {
            const auto n = ::recv(fd_, &ch, 1, 0);
            require(n == 1, "handshake incomplete");
            head.push_back(ch);
            require(head.size() < 16384, "handshake too large");
        }
        require(head.starts_with("HTTP/1.1 101"), "upgrade rejected: "+head.substr(0, head.find('\r')));
    }
    ~WsRaw() { ::close(fd_); }
    WsRaw(const WsRaw&) = delete;
    WsRaw& operator=(const WsRaw&) = delete;
    void text(std::string_view payload) {
        require(payload.size() <= 125, "test frame too large");
        const char mask[4] = {'\x11', '\x22', '\x33', '\x44'};
        std::string frame{'\x81', static_cast<char>(0x80 | payload.size()), mask[0], mask[1], mask[2], mask[3]};
        for (std::size_t i = 0; i < payload.size(); ++i) frame.push_back(payload[i] ^ mask[i % 4]);
        send_all(frame);
    }
    void bin(std::string_view payload) {
        require(payload.size() <= 125, "test frame too large");
        const char mask[4] = {'\x11', '\x22', '\x33', '\x44'};
        std::string frame{'\x82', static_cast<char>(0x80 | payload.size()), mask[0], mask[1], mask[2], mask[3]};
        for (std::size_t i = 0; i < payload.size(); ++i) frame.push_back(payload[i] ^ mask[i % 4]);
        send_all(frame);
    }
    std::string read_text() {
        for (;;) {
            unsigned char head[2];
            recv_all(reinterpret_cast<char*>(head), 2);
            std::size_t length = head[1] & 0x7f;
            if (length == 126) { unsigned char ext[2]; recv_all(reinterpret_cast<char*>(ext), 2); length = (ext[0]<<8)|ext[1]; }
            else if (length == 127) {
                unsigned char ext[8]; recv_all(reinterpret_cast<char*>(ext), 8);
                length = 0;
                for (unsigned char byte : ext) length = (length<<8)|byte;
            }
            char mask[4] = {};
            const bool masked = head[1] & 0x80;
            if (masked) recv_all(mask, 4);
            std::string payload(length, '\0');
            if (length) recv_all(payload.data(), length);
            if (masked) for (std::size_t i = 0; i < length; ++i) payload[i] ^= mask[i % 4];
            const auto opcode = head[0] & 0x0f;
            if (opcode == 8) throw std::runtime_error("websocket closed by server");
            if (opcode == 1 || opcode == 2) return payload; // ping/pong/continuation skipped
        }
    }
    // Reads and discards until the server closes the session; returns the bytes seen.
    std::size_t drain_until_closed(std::chrono::seconds budget) {
        const auto deadline = std::chrono::steady_clock::now()+budget;
        std::size_t drained = 0;
        char buffer[65536];
        for (;;) {
            const auto n = ::recv(fd_, buffer, sizeof buffer, 0);
            if (n == 0 || (n < 0 && errno == ECONNRESET)) return drained;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("flood session was not closed");
                continue;
            }
            require(n > 0, "drain failed");
            drained += static_cast<std::size_t>(n);
        }
    }
private:
    void send_all(std::string_view data) {
        while (!data.empty()) {
            const auto n = ::send(fd_, data.data(), data.size(), MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            require(n > 0, "websocket send failed");
            data.remove_prefix(static_cast<std::size_t>(n));
        }
    }
    void recv_all(char* target, std::size_t size) {
        std::size_t offset = 0;
        while (offset < size) {
            const auto n = ::recv(fd_, target+offset, size-offset, 0);
            if (n < 0 && errno == EINTR) continue;
            require(n > 0, "websocket closed while reading a frame");
            offset += static_cast<std::size_t>(n);
        }
    }
    int fd_;
};
}
// Big-bag single-item modification — the property design's cost question.
// One nested write must journal exactly one delta (never a full-bag
// rewrite), must advance the persistent revision and set dirty without any
// setDirty or top-level reassignment, and its cost must not scale with bag
// size. Byte counts use the wire-shape accounting below (u32 property id,
// u16 field id or u32 index per path step, u8 kind, u32 position, fixed-
// width values); the byte serializer itself arrives with wire transport.
std::size_t wire_bytes(const EntityValue& value) {
    if (auto* array = std::get_if<EntityArray>(&value.data))
        return 4 + std::accumulate(array->begin(), array->end(), std::size_t{0},
            [](std::size_t sum, const EntityValue& item) { return sum + wire_bytes(item); });
    if (auto* object = std::get_if<EntityObject>(&value.data))
        return std::accumulate(object->begin(), object->end(), std::size_t{0},
            [](std::size_t sum, const auto& field) { return sum + 2 + wire_bytes(field.second); });
    if (std::holds_alternative<std::string>(value.data)) return 4 + std::get<std::string>(value.data).size();
    return std::holds_alternative<bool>(value.data) ? 1 : 8;
}
std::size_t delta_bytes(const SyncDelta& delta) {
    std::size_t steps = 0;
    for (const auto& step : delta.path) steps += step.by_index ? 4 : 2;
    return 4 + steps + 1 + 4 + (delta.kind == SyncDelta::Kind::REMOVE ? 0 : wire_bytes(delta.value));
}
void entity_perf() {
    const EntityContract contract(R"(<Contracts><Types>
        <Item><itemId id="0" type="UINT32"/><count id="1" type="UINT32" default="1"/></Item>
        <Inventory><capacity id="0" type="UINT16" default="512"/><items id="1" type="ARRAY" of="Item"/></Inventory>
        </Types>
        <Entity name="Avatar" id="1">
          <Properties>
            <level id="1" type="UINT16" default="1" flags="OWN_CLIENT"/>
            <inventory id="2" type="Inventory" flags="OWN_CLIENT" persistent="true"/>
          </Properties>
        </Entity></Contracts>)");
    PropertyStore store(contract, 1);
    PersistentStore persist(store);
    require(persist.assume(7), "hosting generation was not accepted");
    const auto make_bag = [](std::size_t cells) {
        EntityArray items;
        for (std::size_t i = 0; i < cells; ++i)
            items.push_back(EntityObject{{"itemId", std::int64_t{1000 + static_cast<std::int64_t>(i)}},
                                         {"count", std::int64_t{3}}});
        return EntityObject{{"capacity", std::int64_t{static_cast<std::int64_t>(cells)}},
                            {"items", EntityValue{std::move(items)}}};
    };
    require(persist.set("inventory", EntityValue{make_bag(512)}), "the bag baseline write was refused");
    const std::uint64_t after_baseline = persist.revision();
    require(after_baseline >= 1, "the bag baseline write did not mark persistence");

    // One single-item write: one journal delta, one revision, dirty — no
    // setDirty, no top-level reassignment, no full-bag journal entry.
    PropertyChannel channel(&store);
    require(channel.observe("owner", true), "owner observer entry refused");
    require(channel.deliver().size() == 1, "owner entry did not deliver one baseline");
    require(persist.set_path("inventory",
                             TreePath{TreeStep{false, "items", 0}, TreeStep{true, std::string(), 5},
                                      TreeStep{false, "count", 0}},
                             EntityValue{std::int64_t{4}}),
            "the single-item write was refused");
    require(persist.revision() == after_baseline + 1 && persist.dirty(),
            "the single write did not mark persistence");
    const auto drained = channel.deliver();
    require(drained.size() == 1 && drained[0].kind == SyncMessage::Kind::DELTAS && drained[0].deltas.size() == 1,
            "a single-item write did not journal exactly one delta");
    const SyncDelta& delta = drained[0].deltas[0];
    require(delta.kind == SyncDelta::Kind::SET && delta.property_id == 2 && delta.path.size() == 3 &&
                delta.path[0].field_id == 1 && delta.path[1].by_index && delta.path[1].index == 5 &&
                delta.path[2].field_id == 1,
            "the delta lost its nested wire path");
    require(std::get<std::int64_t>(delta.value.data) == 4, "the delta lost its leaf value");

    // Bytes: the one write costs a fixed header plus the leaf value — orders
    // of magnitude below rebaselining the same bag.
    const std::size_t one = delta_bytes(delta);
    const auto* root = store.get("inventory");
    require(root, "the store lost the bag");
    const std::size_t whole = 4 + wire_bytes(*root);
    require(one * 64 < whole, "the incremental delta is not meaningfully smaller than a rebaseline");
    std::cout << "entity.perf bytes: single-item delta " << one << " B vs full-bag rebaseline " << whole
              << " B (" << (whole / one) << "x smaller) [PASS]\n";

    // CPU: writes into a bigger bag must not cost proportionally more — the
    // walk is O(path), not O(bag). Generous ceilings; this measures.
    using steady = std::chrono::steady_clock;
    const auto write_one = [&](std::size_t i, std::size_t cells) {
        (void)persist.set_path("inventory",
                               TreePath{TreeStep{false, "items", 0}, TreeStep{true, std::string(), i % cells},
                                        TreeStep{false, "count", 0}},
                               EntityValue{std::int64_t{static_cast<std::int64_t>(i % 1000) + 10}});
        if ((i & 15) == 15) (void)channel.deliver();
    };
    double small_ns = 0, big_ns = 0;
    for (int round = 0; round < 2; ++round) {
        const std::size_t cells = round == 0 ? 512 : 4096;
        require(persist.set("inventory", EntityValue{make_bag(cells)}), "the refill was refused");
        (void)channel.deliver();
        for (std::size_t i = 0; i < 200; ++i) write_one(i, cells);
        (void)channel.deliver();
        const std::size_t ops = 20000;
        const auto begin = steady::now();
        for (std::size_t i = 0; i < ops; ++i) write_one(i, cells);
        (void)channel.deliver();
        const auto end = steady::now();
        const double ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count() / double(ops);
        (round == 0 ? small_ns : big_ns) = ns;
        std::cout << "entity.perf write+drain: " << cells << " cells -> " << ns << " ns/op\n";
    }
    require(small_ns < 200000, "a single-item write took unreasonably long");
    require(big_ns < small_ns * 4, "per-write cost scaled with bag size (O(bag) walk?)");

    // Allocation retention: after warmup, steady-state single-item writes
    // must not keep growing the heap (the journal drains, cells are reused).
    {
        for (std::size_t i = 0; i < 500; ++i) write_one(i, 512);
        (void)channel.deliver();
        const auto before = mallinfo2();
        const std::size_t ops = 20000;
        for (std::size_t i = 0; i < ops; ++i) write_one(i, 512);
        (void)channel.deliver();
        const auto after = mallinfo2();
        const double retained = double(after.uordblks) - double(before.uordblks);
        require(retained < double(1 << 20), "steady-state single-item writes kept growing the heap");
        std::cout << "entity.perf allocations: retained " << retained / double(ops)
                  << " B/op over " << ops << " writes [PASS]\n";
    }
    std::cout << "entity.perf big-bag single-item modification [PASS]\n";
}
void core_contracts() {

    TokenBucket bucket(2, 0.000001);
    require(bucket.consume() && bucket.consume() && !bucket.consume(), "token budget is not enforced");
    must_fail([] { TokenBucket invalid(0, 1); });
    auto context = cross_context("MLC1 1 realm group room");
    equal(context.room, "room");
    require(context.character.empty() && !context.generation && context.return_room.empty(),
        "anonymous join gained participant fields");
    must_fail([] { (void)cross_context("MLC1 0 realm group room"); });
    must_fail([] { (void)cross_context("MLC1 1 realm group room extra"); });
    // A participant declares character, hosting generation and return route
    // together; every partial declaration is refused.
    auto participant = cross_context("MLC1 1 realm group room ch hero-7 gen 12 ret home arena r1");
    equal(participant.character, "hero-7");
    require(participant.generation == 12, "hosting generation lost");
    equal(participant.return_realm+"/"+participant.return_group+"/"+participant.return_room, "home/arena/r1");
    require(!participant.ticket, "a join without a ticket gained one");
    // The optional entry ticket rides the last trailing group as hex MLT1.
    ApiSecret ticket_secret{};
    for (std::size_t i = 0; i < ticket_secret.size(); ++i) ticket_secret[i] = static_cast<unsigned char>(i*7+1);
    EntryTicket ticket;
    ticket.character = {"home", "hero-7"};
    ticket.instance = "gs-a";
    ticket.generation = 12;
    ticket.expires = 1'760'000'000;
    ticket.nonce = "0123456789abcdef0123456789abcdef";
    ticket.signature = entry_ticket_sign(ticket, ticket_secret);
    const auto hex = entry_ticket_hex(ticket);
    const auto with_ticket = "MLC1 1 realm group room ch hero-7 gen 12 ret home arena r1 tk " + hex;
    const auto entered = cross_context(with_ticket);
    require(entered.ticket && *entered.ticket == ticket, "the ticket did not round-trip the join");
    // Decoding refuses anything off the wire, so a forged tail is an INVALID
    // join instead of a partly-valid participant.
    for (const auto& forged : std::vector<std::string>{
            hex.substr(0, hex.size()-2),
            hex.substr(1),
            "0"+hex,
            hex.substr(0, hex.size()-4)+"ZZZZ",
            std::string(512, '0'),
            "DEADBEEF"})
        must_fail([&] { (void)cross_context("MLC1 1 realm group room ch hero-7 gen 12 ret home arena r1 tk "+forged); });
    // The ticket is bound to the same identity and generation it names.
    for (const auto& mismatch : std::vector<std::string>{
            "MLC1 1 realm group room ch other gen 12 ret home arena r1",
            "MLC1 1 realm group room ch hero-7 gen 13 ret home arena r1"})
        must_fail([&] { (void)cross_context(mismatch+" tk "+hex); });
    must_fail([&] { (void)cross_context("MLC1 1 realm group room tk "+hex); });
    must_fail([&] { (void)cross_context("MLC1 1 realm group room ch hero-7 gen 12 ret home arena r1 tke "+hex); });
    must_fail([&] { (void)cross_context("MLC1 1 realm group room ch hero-7 gen 12 ret home arena r1 tk "+std::string(420, 'a')); });
    // Oversized tails are refused at the caps, not just by the decoder.
    must_fail([&] { (void)cross_context("MLC1 1 realm group room ch hero-7 gen 12 ret home arena r1 tk "+std::string(1200, 'a')); });
    must_fail([&] { (void)cross_context("MLC1 1 realm group room "+std::string(2000, 'x')); });
    for (const auto& partial : std::vector<std::string>{
        "MLC1 1 realm group room ch hero",
        "MLC1 1 realm group room ch hero gen 12",
        "MLC1 1 realm group room ch hero gen 12 ret home arena",
        "MLC1 1 realm group room gen 12",
        "MLC1 1 realm group room ret home arena r1",
        "MLC1 1 realm group room ch hero gen 0 ret home arena r1",
        "MLC1 1 realm group room ch hero gen 12 ret home arena r1 extra",
        "MLC1 1 realm group room ch her!o gen 12 ret home arena r1"})
        must_fail([&] { (void)cross_context(partial); });
    // MLD1 record grammar: load and submit both parse, and every partial or
    // out-of-shape declaration is refused.
    auto load = parse_record("MLD1 LOAD home hero-7");
    equal(load.realm+"/"+load.character, "home/hero-7");
    require(load.type == RecordRequest::Type::Load, "record load mislabeled");
    auto submit = parse_record("MLD1 SUBMIT home hero-7 gen 12 seq 3 {\"hp\":5}");
    require(submit.type == RecordRequest::Type::Submit && submit.generation == 12
            && submit.sequence == 3, "record submit header lost");
    equal(submit.payload, "{\"hp\":5}");
    equal(parse_record("MLD1 SUBMIT home hero-7 gen 12 seq 3").payload, "");
    for (const auto& broken : std::vector<std::string>{
        "MLD1", "MLD1 LOAD home", "MLD1 LOAD home hero extra",
        "MLD1 SUBMIT home hero gen 12", "MLD1 SUBMIT home hero gen 12 seq",
        "MLD1 SUBMIT home hero gen 0 seq 1", "MLD1 SUBMIT home hero gen 12 seq 0",
        "MLD1 SUBMIT home hero gen x seq 1", "MLD1 SUBMIT home hero gen 12 seq 99999999999",
        "MLD1 SUBMIT ho/me hero gen 12 seq 1", "MLD1 SUBMIT home her!o gen 12 seq 1",
        std::string("MLD1 SUBMIT home hero gen 12 seq 1 ")+std::string(1024*1024+1, 'x')})
        must_fail([&] { (void)parse_record(broken); });
    // The home side owns the fence and the sequence discipline.
    RecordStore records;
    std::string_view result;
    equal(records.handle("MLD1 LOAD home hero", result), "MLD1 EMPTY");
    require(result == "empty", "record load mislabeled as " + std::string(result));
    equal(records.handle("MLD1 SUBMIT home hero gen 5 seq 1 hp=10", result), "MLD1 RECEIVED gen 5 seq 1");
    require(result == "accepted", "first submit was not accepted");
    equal(records.handle("MLD1 LOAD home hero", result), "MLD1 STATE gen 5 seq 1 hp=10");
    require(result == "loaded", "record load after submit mislabeled");
    // A resend dedups to a second receipt and never overwrites the record.
    equal(records.handle("MLD1 SUBMIT home hero gen 5 seq 1 hp=99", result), "MLD1 RECEIVED gen 5 seq 1");
    require(result == "duplicate", "resend was not deduplicated");
    equal(records.handle("MLD1 LOAD home hero", result), "MLD1 STATE gen 5 seq 1 hp=10");
    equal(records.handle("MLD1 SUBMIT home hero gen 5 seq 3 gap", result), "MLD1 ERR SEQ seq 2");
    require(result == "seq", "sequence gap mislabeled");
    equal(records.handle("MLD1 SUBMIT home hero gen 4 seq 2 late", result), "MLD1 ERR STALE gen 5");
    require(result == "stale", "generation fence mislabeled");
    equal(records.handle("MLD1 SUBMIT home hero gen 6 seq 2 taken", result), "MLD1 RECEIVED gen 6 seq 2");
    equal(records.handle("MLD1 SUBMIT home hero gen 7 seq 9 jump", result), "MLD1 ERR SEQ seq 3");
    equal(records.handle("MLD1 SUBMIT home hero gen 7 seq 3", result), "MLD1 RECEIVED gen 7 seq 3");
    equal(records.handle("MLD1 LOAD home hero", result), "MLD1 STATE gen 7 seq 3");
    // Records are independent per character, and a fresh record starts at one.
    equal(records.handle("MLD1 SUBMIT home other gen 1 seq 2", result), "MLD1 ERR SEQ seq 1");
    // Realm is part of both the fence and sequence key: the same character
    // id in another home domain starts independently at sequence one.
    equal(records.handle("MLD1 SUBMIT away hero gen 1 seq 1 hp=3", result), "MLD1 RECEIVED gen 1 seq 1");
    equal(records.handle("MLD1 LOAD home hero", result), "MLD1 STATE gen 7 seq 3");
    equal(records.handle("MLD1 LOAD away hero", result), "MLD1 STATE gen 1 seq 1 hp=3");
    equal(records.handle("MLD1 NOPE", result), "MLD1 ERR INVALID");
    require(result == "invalid", "malformed frame mislabeled");
    // Entity contract prototype: the design page's example parses, resolves
    // and digests; every listed semantic rule refuses its violation.
    const auto document = R"(<Contracts>
  <Types>
    <Item><itemId id="0" type="UINT32"/><count id="1" type="UINT32" default="1"/></Item>
    <Inventory><capacity id="0" type="UINT16" default="20"/><items id="1" type="ARRAY" of="Item"/></Inventory>
  </Types>
  <Entity name="Avatar" id="1">
    <Properties>
      <level id="1" type="UINT16" default="1" flags="OWN_CLIENT" persistent="true"/>
      <hp id="2" type="UINT32" default="100" flags="ALL_CLIENTS"/>
      <gold id="3" type="INT64" default="-5" flags="OWN_CLIENT" persistent="true"/>
      <inventory id="4" type="Inventory" flags="OWN_CLIENT" persistent="true"/>
    </Properties>
    <ServerMethods>
      <Method name="attack" id="1" mode="notify" caller="owner">
        <Arg name="target" id="0" type="EntityRef"/>
      </Method>
      <Method name="getProfile" id="2" mode="request" caller="server">
        <Returns><Field name="level" id="0" type="UINT16"/></Returns>
      </Method>
    </ServerMethods>
    <ClientMethods>
      <Method name="onReward" id="1" mode="notify" audience="owner">
        <Arg name="itemId" id="0" type="UINT32"/><Arg name="count" id="1" type="UINT32"/>
      </Method>
    </ClientMethods>
  </Entity>
</Contracts>)";
    const auto document_with = [&](std::string_view from, std::string_view to) {
        auto text = std::string(document);
        const auto at = text.find(from);
        require(at != std::string::npos, std::string("test edit missed its anchor: ")+std::string(from));
        text.replace(at, from.size(), to);
        return text;
    };
    EntityContract contract(document);
    require(contract.structs().size() == 2 && contract.entities().size() == 1,
            "contract model lost a declaration");
    const auto& avatar = contract.entities().front();
    equal(avatar.properties[3].type, "Inventory");
    require(avatar.properties[3].persistent, "persistent attribute lost");
    require(avatar.properties[1].flags == "ALL_CLIENTS" && !avatar.properties[1].persistent,
            "explicit flags lost");
    require(avatar.properties[2].default_value == "-5", "negative INT64 default lost");
    require(avatar.server_methods[1].returns.size() == 1, "request returns lost");
    require(contract.structs().at("Item").front().id == 0, "struct field id lost");
    // The dump is the digest input and keeps declaration order and explicit ids.
    require(contract.describe().find(" prop {1 level UINT16 default 1} OWN_CLIENT persistent") != std::string::npos &&
            contract.describe().find(" method server 2 getProfile request server") != std::string::npos,
            "canonical dump lost the resolved model: "+contract.describe());
    require(contract.digest() != 0, "digest is computed");
    require(EntityContract(document).digest() == contract.digest(), "digest is not reproducible");
    for (const auto& broken : std::vector<std::string>{
        // ids: explicit, unique, never recomputed
        document_with("<hp id=\"2\" type=\"UINT32\" default=\"100\" flags=\"ALL_CLIENTS\"/>",
                      "<hp id=\"1\" type=\"UINT32\" default=\"100\" flags=\"ALL_CLIENTS\"/>"),
        // closed vocabularies
        document_with("flags=\"ALL_CLIENTS\"", "flags=\"EVERYONE\""),
        document_with("type=\"UINT32\" default=\"100\" flags", "type=\"UINT8\" default=\"100\" flags"),
        document_with("caller=\"server\"", "caller=\"anyone\""),
        document_with("mode=\"request\" caller=\"server\"",
                      "mode=\"fire-and-forget\" caller=\"server\""),
        // typed defaults
        document_with("default=\"100\" flags", "default=\"4294967296\" flags"),
        document_with("default=\"-5\"", "default=\"false\""),
        // arrays and struct references
        document_with("type=\"ARRAY\" of=\"Item\"", "type=\"ARRAY\""),
        document_with("type=\"ARRAY\" of=\"Item\"", "type=\"ARRAY\" of=\"Warehouse\""),
        // methods
        document_with("<Method name=\"getProfile\" id=\"2\" mode=\"request\" caller=\"server\">\n"
                      "        <Returns><Field name=\"level\" id=\"0\" type=\"UINT16\"/></Returns>\n"
                      "      </Method>",
                      "<Method name=\"getProfile\" id=\"2\" mode=\"request\" caller=\"server\"/>"),
        document_with("mode=\"notify\" caller=\"owner\">\n"
                      "        <Arg name=\"target\" id=\"0\" type=\"EntityRef\"/>\n"
                      "      </Method>",
                      "mode=\"notify\" caller=\"owner\">\n"
                      "        <Returns><Field name=\"x\" id=\"0\" type=\"UINT16\"/></Returns>\n"
                      "      </Method>"),
        document_with("audience=\"owner\"", "audience=\"server\""),
        "<Entity name=\"Avatar\" id=\"1\"><ServerMethods>"
        "<Method name=\"attack\" id=\"1\" mode=\"request\" caller=\"owner\"/></ServerMethods></Entity>",
        // xml subset: external entities and CDATA are refused outright
        std::string("<!DOCTYPE Contracts [<!ENTITY xxe \"evil\">]><Contracts></Contracts>"),
        std::string("<Contracts><Entity name=\"A\" id=\"1\"/><![CDATA[raw]]></Contracts>")})
        must_fail([&] { (void)EntityContract(broken); });
    // Duplicate member names across groups are refused, even across method
    // sides; the same method id on both sides is fine.
    EntityContract mirrored(std::string(
        "<Contracts><Entity name=\"A\" id=\"1\">"
        "<Properties><x id=\"1\" type=\"UINT32\"/></Properties>"
        "<ServerMethods><Method name=\"ping\" id=\"1\" mode=\"notify\" caller=\"server\"/></ServerMethods>"
        "<ClientMethods><Method name=\"pong\" id=\"1\" mode=\"notify\" audience=\"owner\"/></ClientMethods>"
        "</Entity></Contracts>"));
    require(mirrored.entities().front().client_methods.front().id == 1, "side-local method id lost");
    must_fail([&] { (void)EntityContract(std::string(
        "<Contracts><Entity name=\"A\" id=\"1\">"
        "<ServerMethods><Method name=\"ping\" id=\"1\" mode=\"notify\" caller=\"server\"/></ServerMethods>"
        "<ClientMethods><Method name=\"ping\" id=\"1\" mode=\"notify\" audience=\"owner\"/></ClientMethods>"
        "</Entity></Contracts>")); });
    must_fail([&] { (void)EntityContract(std::string(
        "<Contracts><Entity name=\"A\" id=\"1\">"
        "<Properties><x id=\"1\" type=\"UINT32\"/></Properties>"
        "<ServerMethods><Method name=\"x\" id=\"1\" mode=\"notify\" caller=\"server\"/></ServerMethods>"
        "</Entity></Contracts>")); });
    // Recursive struct chains are refused; acyclic chains resolve.
    must_fail([&] { (void)EntityContract(std::string(
        "<Contracts><Types>"
        "<A><next id=\"0\" type=\"ARRAY\" of=\"B\"/></A>"
        "<B><prev id=\"0\" type=\"A\"/></B>"
        "</Types></Contracts>")); });
    EntityContract chain(std::string(
        "<Contracts><Types>"
        "<A><b id=\"0\" type=\"B\"/></A>"
        "<B><leaf id=\"0\" type=\"UINT16\"/></B>"
        "</Types><Entity name=\"E\" id=\"1\">"
        "<Properties><root id=\"1\" type=\"A\"/></Properties>"
        "</Entity></Contracts>"));
    require(chain.digest() != 0, "digest is computed");
    require(chain.describe().find("struct A") != std::string::npos &&
            chain.describe().find(" prop {1 root A} SERVER_ONLY") != std::string::npos,
            "canonical dump lost the resolved model: "+chain.describe());
    // Reserved ids keep retired tags forever, and the schema/lock emit them.
    EntityContract locked(document_with("</Entity>", "<Reserved>"
            "<Property id=\"5\"/><ServerMethod id=\"9\"/><ClientMethod id=\"7\"/></Reserved></Entity>"));
    const auto& retained = locked.entities().front();
    require(retained.reserved_properties.size() == 1 && retained.reserved_properties[0] == 5 &&
            retained.reserved_server_methods.size() == 1 && retained.reserved_server_methods[0] == 9 &&
            retained.reserved_client_methods.size() == 1 && retained.reserved_client_methods[0] == 7,
            "reserved ids lost: "+locked.describe());
    require(locked.digest() != contract.digest(), "reserving an id does not change the model");
    for (const auto& broken : std::vector<std::string>{
        document_with("</Entity>", "<Reserved><Property id=\"1\"/></Reserved></Entity>"),
        document_with("</Entity>", "<Reserved><Property id=\"5\"/><Property id=\"5\"/></Reserved></Entity>"),
        document_with("</Entity>", "<Reserved><Property id=\"5\" note=\"retired\"/></Reserved></Entity>"),
        document_with("</Entity>", "<Reserved><Property/></Reserved></Entity>"),
        document_with("</Entity>", "<Reserved><Thing id=\"5\"/></Reserved></Entity>"),
        document_with("</Entity>", "<Reserved><Property id=\"7\"/></Reserved></Entity>"
        "<Reserved><Property id=\"5\"/></Reserved></Entity>")})
        must_fail([&] { (void)EntityContract(broken); });
    // The generated schema is well-formed XML and pins each member's id and type.
    const auto schema = locked.xsd();
    require(EntityContract::well_formed(schema), "generated schema is not well-formed: "+schema);
    require(EntityContract(document_with("</Entity>", "<Reserved><Property id=\"5\"/>"
            "<ServerMethod id=\"9\"/><ClientMethod id=\"7\"/></Reserved></Entity>")).xsd() == schema,
            "schema is not reproducible");
    const auto block = [&](const std::string& needle) {
        const auto at = schema.find(needle);
        require(at != std::string::npos, std::string("schema is missing ")+needle);
        const auto end = schema.find("</xs:element>", at);
        require(end != std::string::npos, "schema block is unterminated");
        return schema.substr(at, end-at);
    };
    const auto hp = block("<xs:element name=\"hp\" ");
    require(hp.find("<xs:enumeration xs:value=\"2\"/>") != std::string::npos &&
            hp.find("<xs:enumeration xs:value=\"UINT32\"/>") != std::string::npos &&
            hp.find("type=\"ReplicationFlags\"") != std::string::npos,
            "the schema does not pin a property's id, type and flags: "+hp);
    require(block("<xs:element name=\"Property\" ").find("<xs:enumeration xs:value=\"5\"/>") !=
            std::string::npos, "the schema does not pin a reserved property id");
    require(block("<xs:element name=\"ClientMethod\" ").find("<xs:enumeration xs:value=\"7\"/>") !=
            std::string::npos, "the schema does not pin a reserved client method id");
    require(block("<xs:element name=\"Field\" ").find("<xs:enumeration xs:value=\"UINT16\"/>") !=
            std::string::npos, "the schema lost the request's typed return");
    const auto attr = [&](const char* key, const std::string& value) {
        return std::string("<xs:attribute name=\"")+key+"\" use=\"required\"><xs:simpleType>"
            "<xs:restriction base=\"xs:string\"><xs:enumeration xs:value=\""+value+
            "\"/></xs:restriction></xs:simpleType></xs:attribute>";
    };
    require(schema.find(attr("mode", "request")) != std::string::npos &&
            schema.find(attr("mode", "notify")) != std::string::npos,
            "the schema does not pin the declared modes");
    require(schema.find(attr("caller", "owner")) != std::string::npos &&
            schema.find(attr("caller", "server")) != std::string::npos &&
            schema.find(attr("audience", "owner")) != std::string::npos,
            "the schema mixed up caller and audience");
    require(schema.find(attr("name", "attack")) != std::string::npos &&
            schema.find(attr("id", "1")) != std::string::npos,
            "the schema does not pin member names and ids");
    require(schema.find("<xs:enumeration xs:value=\"Inventory\"/>") != std::string::npos &&
            schema.find("<xs:element name=\"Inventory\"") != std::string::npos &&
            schema.find("name=\"capacity\"") != std::string::npos,
            "the schema lost the declared structs");
    require(contract.xsd().find("name=\"Reserved\"") == std::string::npos,
            "an empty Reserved group must not appear in the schema");
    require(!EntityContract::well_formed("<Contracts><A><B/></Contracts>"),
            "well-formedness accepted a mismatched close tag");
    require(!EntityContract::well_formed("<Contracts/></Contracts>"),
            "well-formedness accepted trailing content");
    // The release lock keeps every assignment a published version made.
    const auto lock_text = locked.lock();
    require(lock_text.find("# mistlane entity contract lock\n# digest ") != std::string::npos &&
            lock_text.find("struct Item") != std::string::npos &&
            lock_text.find("entity Avatar 1") != std::string::npos &&
            lock_text.find(" prop Avatar 3 gold INT64") != std::string::npos &&
            lock_text.find(" method Avatar server 2 getProfile") != std::string::npos &&
            lock_text.find(" method Avatar client 1 onReward") != std::string::npos &&
            lock_text.find(" reserved Avatar property 5") != std::string::npos &&
            lock_text.find(" reserved Avatar server 9") != std::string::npos &&
            lock_text.find(" reserved Avatar client 7") != std::string::npos,
            "release lock lost assignments: "+lock_text);
    require(EntityContract(document_with("</Entity>", "<Reserved><Property id=\"5\"/>"
            "<ServerMethod id=\"9\"/><ClientMethod id=\"7\"/></Reserved></Entity>")).lock() == lock_text,
            "release lock is not reproducible");
    // Compatibility model: the lock text parses back into exactly the entries
    // the live model flattens to, so the publication gate compares one model
    // shape on both sides instead of a second ad-hoc lock reader.
    const auto baseline = locked.entries();
    require(parse_lock(lock_text) == baseline, "lock text and live model disagree on entries");
    // A lock line the parser cannot place is refused outright, never skipped.
    must_fail([&] { (void)parse_lock(lock_text+" widget Avatar x\n"); });
    must_fail([&] { (void)parse_lock("struct Item {0 itemId UINT32 extra}\n"); });
    must_fail([&] { (void)parse_lock(lock_text+" prop Avatar 2 hp UINT32\n"); });
    require(compat_diff(baseline, baseline).compatible(), "a lock is incompatible with itself");
    const auto compat_source = document_with("</Entity>", "<Reserved><Property id=\"5\"/>"
            "<ServerMethod id=\"9\"/><ClientMethod id=\"7\"/></Reserved></Entity>");
    const auto compat_edit = [&](std::string_view from, std::string_view to) {
        auto text = compat_source;
        const auto at = text.find(from);
        require(at != std::string::npos,
                std::string("compat edit missed its anchor: ")+std::string(from));
        text.replace(at, from.size(), to);
        return EntityContract(text).entries();
    };
    const auto joined_lines = [](const std::vector<std::string>& lines) {
        std::string text;
        for (const auto& line : lines) text += line + "; ";
        return text;
    };
    const auto breaks_with = [&](const std::vector<ContractEntry>& prev, const std::vector<ContractEntry>& curr,
                                 std::string_view needle) {
        const auto diff = compat_diff(prev, curr);
        const auto reports = joined_lines(diff.breaking);
        require(!diff.compatible() && reports.find(needle) != std::string::npos,
                std::string("compat gate missed ")+std::string(needle)+": "+reports);
    };
    breaks_with(baseline, compat_edit("<gold id=\"3\" type=\"INT64\"", "<gold id=\"6\" type=\"INT64\""), "3 -> 6");
    breaks_with(baseline, compat_edit("<gold id=\"3\" type=\"INT64\"", "<gold id=\"3\" type=\"STRING\""), "INT64");
    breaks_with(baseline, compat_edit("caller=\"owner\"", "caller=\"server\""), "notify owner -> notify server");
    breaks_with(baseline, compat_edit("<gold id=\"3\" type=\"INT64\" default=\"-5\" flags=\"OWN_CLIENT\" persistent=\"true\"/>\n      ",
            ""), "gold");
    breaks_with(baseline, EntityContract(document).entries(), "dropped");
    // Retired ids never come back as live meanings on any id space. The XML
    // parser refuses the collision outright, so the attempt lives in lock text
    // exactly where a published lock could be hand-edited into it.
    const auto lock_edit = [&](std::string_view from, std::string_view to) {
        auto text = lock_text;
        const auto at = text.find(from);
        require(at != std::string::npos,
                std::string("lock edit missed its anchor: ")+std::string(from));
        text.replace(at, from.size(), to);
        return parse_lock(text);
    };
    breaks_with(baseline, lock_edit(" reserved Avatar property 5\n",
            " reserved Avatar property 5\n prop Avatar 5 magic UINT32\n"), "reused");
    breaks_with(baseline, lock_edit(" reserved Avatar server 9\n",
            " reserved Avatar server 9\n method Avatar server 9 zap notify server\n"), "reused");
    breaks_with(baseline, lock_edit(" reserved Avatar client 7\n",
            " reserved Avatar client 7\n method Avatar client 7 zap notify owner\n"), "reused");
    breaks_with(baseline, lock_edit(" arg Avatar server 1 0 target EntityRef\n",
            " arg Avatar server 1 0 target ARRAY Item\n"), "EntityRef");
    const auto added = compat_diff(baseline, compat_edit(
            "<gold id=\"3\" type=\"INT64\" default=\"-5\" flags=\"OWN_CLIENT\" persistent=\"true\"/>",
            "<gold id=\"3\" type=\"INT64\" default=\"-5\" flags=\"OWN_CLIENT\" persistent=\"true\"/>"
            "<nickname id=\"6\" type=\"STRING\"/>"));
    require(added.compatible() && !added.additive.empty() &&
            joined_lines(added.additive).find("nickname") != std::string::npos,
            "a new property did not land as an additive change");
    CrossAdmission admission(1, 1, 1, 1);
    int a = 0, b = 0;
    require(admission.acquire(&a, context, 0).empty(), "first admission failed");
    equal(admission.acquire(&b, context, 0), "ROOM_FULL");
    admission.release(&a);
    require(admission.acquire(&b, context, 0).empty(), "admission did not recover on release");
    Context c;
    for (auto args : std::vector<std::vector<std::string>>{
        {"--role", "invalid"}, {"--listen", "127.0.0.1:65536"}, {"--role", "gameServer"},
        {"--role", "gateway"}, {"--max-connections", "0"}, {"--role", "gameServer", "--transport", "wss"}}) {
        args.insert(args.begin(), self().string());
        require(c.command(args) != 0, "invalid CLI options accepted");
    }
}
void role_names() {
    Context c;
    write(c.path("echo.lua"), echo);
    for (const auto& old : {"worker", "router", "cross", "shard"}) {
        auto& child = c.spawn({self().string(), "--role", old, "--script", c.path("echo.lua").string(),
                              "--upstream", endpoint(port())});
        require(child.wait() != 0, "removed role accepted");
        require(child.output().find("unknown role") != std::string::npos, "removed role was not rejected by name");
    }
    // login and data run as script services: a script-less launch is refused,
    // and with a script they serve framed sessions under their own role name.
    for (const auto& role : {"login", "data"}) {
        auto& child = c.spawn({self().string(), "--role", role});
        require(child.wait() != 0 && child.output().find("--script") != std::string::npos,
                std::string(role) + " started without a script");
        write(c.path(std::string(role)+".lua"), "return {message=function(p)return p end}");
        const auto listen = port(), metrics = port();
        c.node(role, listen, {"--script", c.path(std::string(role)+".lua").string(),
                              "--metrics-listen", endpoint(metrics)});
        equal(tcp(listen)->request("ok"), "ok");
        const auto samples = http("http://"+endpoint(metrics)+"/metrics");
        require(samples.status == 200 && samples.body.find(std::string("mistlane_ready{role=\"")+role+"\"} 1") != std::string::npos,
            std::string(role) + " readiness gauge missing");
    }
    write(c.path("api.lua"), http_lua);
    const auto api_port = port();
    auto& api = c.node("apiServer", api_port, {"--script", c.path("api.lua").string()});
    require(api.output().find("ready apiServer ") != std::string::npos, "apiServer role missing");
    // Default apiServer transport is HTTP. Missing HTTP callback has a controlled response.
    require(http("http://"+endpoint(api_port)+"/missing").status != 0, "apiServer HTTP default failed");
}
void transport_tcp() {
    Context c;
    const auto script = c.path("echo.lua");
    write(script, echo);
    const auto gameServer=port(), forwarder=port(), gateway=port(), metrics=port();
    c.node("gameServer", gameServer, {"--script", script.string(), "--metrics-listen", endpoint(metrics)});
    c.node("gateway", forwarder, {"--upstream", endpoint(gameServer)});
    c.node("gateway", gateway, {"--upstream", endpoint(forwarder)});
    auto client = tcp(gateway);
    for (const auto& payload : std::vector<std::string>{{}, "hello", std::string("\0\xff", 2), "中文", std::string(1024*1024, 'x')})
        equal(client->request(payload), payload);
    for (int i=0; i<100; ++i) equal(client->request("stack"), "stack");
    auto samples = http("http://"+endpoint(metrics)+"/metrics");
    require(samples.status == 200 && samples.body.find("mistlane_ready{role=\"gameServer\"} 1") != std::string::npos,
        "Prometheus readiness missing");
    require(samples.body.find("mistlane_lua_callback_seconds_count") != std::string::npos, "callback histogram missing");
    const auto metric_value = [](const std::string& body, const std::string& sample) {
        const auto at = body.find(sample);
        require(at != std::string::npos, "missing metric " + sample);
        return std::strtod(body.c_str() + at + sample.size(), nullptr);
    };
    require(metric_value(samples.body, "mistlane_messages_total{role=\"gameServer\",direction=\"rx\"}") > 0,
        "rx message counter missing");
    require(metric_value(samples.body, "mistlane_messages_total{role=\"gameServer\",direction=\"tx\"}") > 0,
        "tx message counter missing");
    require(metric_value(samples.body, "mistlane_payload_bytes_total{role=\"gameServer\",direction=\"tx\"}") > 1024*1024,
        "payload byte counter missing");
    require(metric_value(samples.body, "mistlane_request_seconds_count{role=\"gameServer\"}") > 0,
        "request latency histogram missing");
    require(metric_value(samples.body, "mistlane_request_seconds_sum{role=\"gameServer\"}") > 0,
        "request latency not observed");
    // The first scheduled tick fires at least 100ms after start; wait for it.
    Http ticks;
    eventually([&] {
        ticks = http("http://"+endpoint(metrics)+"/metrics");
        return metric_value(ticks.body, "mistlane_event_loop_lag_seconds_count{role=\"gameServer\"}") > 0;
    });
    require(metric_value(ticks.body, "mistlane_event_loop_lag_max_seconds{role=\"gameServer\"}") > 0,
        "tick lag maximum missing");
    require(metric_value(samples.body, "mistlane_upstream_errors_total{role=\"gameServer\"}") == 0,
        "unexpected upstream errors");
    require(metric_value(samples.body, "mistlane_upstream_timeouts_total{role=\"gameServer\"}") == 0,
        "unexpected upstream timeouts");
    const std::string frame("\0\0\0\5abcde", 9);
    {
        RawSocket raw(gateway);
        for (char ch : frame) raw.send(std::string_view(&ch, 1));
        equal(raw.receive(frame.size()), frame);
        raw.send(frame+frame);
        equal(raw.receive(2*frame.size()), frame+frame);
        raw.send(std::string("\0\x10\0\1", 4));
        require(raw.closed(), "oversize frame was accepted");
    }
    std::vector<std::future<void>> tasks;
    for (int i=0; i<8; ++i) tasks.push_back(std::async(std::launch::async, [=] {
        auto peer = tcp(gateway); equal(peer->request("parallel"), "parallel");
    }));
    for (auto& task : tasks) task.get();
    const auto limited = port();
    c.node("gameServer", limited, {"--script", script.string(), "--max-connections", "1"});
    auto first = tcp(limited); equal(first->request("one"), "one");
    RawSocket extra(limited);
    require(extra.closed(), "connection admission was not enforced");
    equal(first->request("still-alive"), "still-alive");
    const auto dead = port(), ingress = port(), gateway_metrics = port();
    c.node("gateway", ingress, {"--upstream", endpoint(dead), "--metrics-listen", endpoint(gateway_metrics)});
    must_fail([&] { (void)tcp(ingress)->request("unavailable"); });
    auto failure = http("http://"+endpoint(gateway_metrics)+"/metrics");
    require(metric_value(failure.body, "mistlane_upstream_errors_total{role=\"gateway\"}") >= 1,
        "upstream connect failure not counted");
}
void transport_websocket() {
    Context c; c.cert(); write(c.path("echo.lua"), echo);
    const auto gameServer = port(), forwarder = port();
    c.node("gameServer", gameServer, {"--script", c.path("echo.lua").string()});
    c.node("gateway", forwarder, {"--upstream", endpoint(gameServer)});
    for (const auto& transport : {"ws", "wss"}) {
        const bool secure = std::string_view(transport) == "wss";
        for (const auto& role : {"gateway", "gameServer"}) {
            const auto listen = port();
            std::vector<std::string> args{"--transport", transport};
            if (secure) { auto cert = tls(c); args.insert(args.end(), cert.begin(), cert.end()); }
            if (std::string_view(role) == "gateway") args.insert(args.end(), {"--upstream", endpoint(forwarder)});
            else args.insert(args.end(), {"--script", c.path("echo.lua").string()});
            c.node(role, listen, args);
            for (const auto& path : {"/", "/ws"}) {
                cli::ClientOptions options{std::string(transport)+"://localhost:"+std::to_string(listen)+path,
                    secure ? c.path("cert.pem").string() : ""};
                auto client = cli::make_client(options);
                for (const auto& data : std::vector<std::string>{{}, "中文", std::string("\0\xff", 2), std::string(65536, 'w')})
                    equal(client->request(data), data);
            }
            if (secure) {
                // Certificate trust and hostname verification must stay enabled.
                must_fail([&] { auto peer = cli::make_client({"wss://localhost:"+std::to_string(listen)+"/ws", {}, 500}); });
                must_fail([&] { auto peer = cli::make_client({"wss://"+endpoint(listen)+"/ws", c.path("cert.pem").string(), 500}); });
            }
        }
    }
}
void transport_http() {
    Context c; c.cert(); write(c.path("echo.lua"), echo);
    const auto gameServer = port();
    c.node("gameServer", gameServer, {"--script", c.path("echo.lua").string()});
    for (const auto& transport : {"http", "https"}) {
        const bool secure = std::string_view(transport) == "https";
        const auto listen = port();
        std::vector<std::string> args{"--transport", transport, "--upstream", endpoint(gameServer)};
        if (secure) { auto cert = tls(c); args.insert(args.end(), cert.begin(), cert.end()); }
        c.node("gateway", listen, args);
        const auto origin = std::string(transport)+"://localhost:"+std::to_string(listen);
        HttpRequest request{origin+"/message", "POST", std::string("\0\xffhello", 7),
            secure ? c.path("cert.pem").string() : ""};
        const auto result = http_request(request);
        require(result.status == 200, "HTTP adapter status"); equal(result.body, request.body);
        require(http(origin+"/missing", "GET", {}, request.ca_file).status == 404, "unknown HTTP route");
        request.max_response_bytes = 2;
        must_fail([&] { (void)http_request(request); });
        if (secure) {
            request.max_response_bytes = 1024; request.ca_file.clear();
            must_fail([&] { (void)http_request(request); });
        }
    }
    must_fail([] { (void)http_request({"file:///etc/hosts"}); });
}

// MLA1 rides the node's HTTP listener: a native /mla1 intercept beside the Lua
// routes, enabled by credential environment variables only.
void api_http() {
    Context c;
    write(c.path("api.lua"), http_lua);
    ApiSecret secret{};
    for (auto& byte : secret) byte = 0x37;
    std::string hex;
    for (std::size_t i = 0; i < secret.size(); ++i) hex += "37";
    const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    unsigned counter = 0;
    const auto nonce = [&counter] {
        char digits[33];
        std::snprintf(digits, sizeof digits, "%032x", ++counter);
        return std::string(digits);
    };
    const auto signed_body = [&](ApiRequest& request) {
        request.signature = api_sign(request, secret);
        return api_canonical(request) + std::string(request.signature.begin(), request.signature.end());
    };
    const auto accept_body = [&](const std::string& idempotency, const std::string& payload = "grant") {
        ApiRequest request;
        request.operation = ApiOperation::Accept;
        request.audience = "platform";
        request.principal = "ops-callback";
        request.key_id = "key-2026";
        request.timestamp = now;
        request.nonce = nonce();
        request.character = {"realm1", "char42"};
        request.idempotency_key = idempotency;
        request.payload = payload;
        return signed_body(request);
    };
    const auto owner_body = [&] {
        ApiRequest request;
        request.operation = ApiOperation::Owner;
        request.audience = "platform";
        request.principal = "ops-callback";
        request.key_id = "key-2026";
        request.timestamp = now;
        request.nonce = nonce();
        request.character = {"realm1", "char42"};
        return signed_body(request);
    };
    const auto reply_body = [](std::initializer_list<std::string_view> values) {
        std::string bytes = "MLA1";
        for (const auto& value : values) {
            const auto size = static_cast<std::uint32_t>(value.size());
            for (int shift : {24, 16, 8, 0}) bytes.push_back(static_cast<char>((size >> shift) & 255));
            bytes.append(value);
        }
        return bytes;
    };
    {
        ScopedEnv audience("MISTLANE_MLA1_AUDIENCE", "platform");
        ScopedEnv credentials("MISTLANE_MLA1_CREDENTIAL", "ops-callback:key-2026:"+hex+":realm1");
        const auto listen = port();
        c.node("apiServer", listen, {"--script", c.path("api.lua").string()});
        const auto base = "http://"+endpoint(listen);
        // A signed acceptance inserts once, then deduplicates on retry.
        auto sent = http(base+"/mla1", "POST", accept_body("grant-7"));
        require(sent.status == 200, "a signed acceptance was not 200");
        equal(sent.body, reply_body({"OK", "INSERTED", "VOLATILE", "grant", ""}));
        sent = http(base+"/mla1", "POST", accept_body("grant-7"));
        require(sent.status == 200, "a retried acceptance was not 200");
        equal(sent.body, reply_body({"OK", "EXISTING", "VOLATILE", "grant", ""}));
        // One idempotency key with a changed body conflicts, nothing stored.
        sent = http(base+"/mla1", "POST", accept_body("grant-7", "grant-v2"));
        require(sent.status == 409, "a changed body under one key was not 409");
        equal(sent.body, reply_body({"OK", "CONFLICT", "VOLATILE", "", ""}));
        // One flipped signature bit: 401 verdict word, never stored.
        auto tampered = accept_body("grant-8");
        tampered.back() = static_cast<char>(tampered.back() ^ 0x80);
        sent = http(base+"/mla1", "POST", tampered);
        require(sent.status == 401, "a tampered signature was not 401");
        equal(sent.body, reply_body({"BAD_SIGNATURE", "", "", "", ""}));
        // OWNER asks home facts; with no facts wired the answer is NOT_FOUND.
        sent = http(base+"/mla1", "POST", owner_body());
        require(sent.status == 404, "an unknown character was not 404");
        equal(sent.body, reply_body({"OK", "", "", "", "NOT_FOUND"}));
        // Structural garbage is a 400 verdict body, not a Lua trip.
        sent = http(base+"/mla1", "POST", "garbage");
        require(sent.status == 400, "a malformed body was not 400");
        equal(sent.body, reply_body({"INVALID", "", "", "", ""}));
        // Every other route (and GET /mla1) stays plain Lua routing.
        equal(http(base+"/nowhere", "POST", "ping").body, "missing");
        equal(http(base+"/mla1", "GET").body, "missing");
    }
    // Without credential variables /mla1 is ordinary Lua routing.
    {
        const auto listen = port();
        c.node("apiServer", listen, {"--script", c.path("api.lua").string()});
        equal(http("http://"+endpoint(listen)+"/mla1", "POST", accept_body("grant-9")).body, "missing");
    }
    // Half-configured or malformed credentials refuse startup.
    {
        ScopedEnv audience("MISTLANE_MLA1_AUDIENCE", "platform");
        auto& lonely = c.spawn({self().string(), "--role", "apiServer", "--listen", endpoint(port()),
            "--script", c.path("api.lua").string()});
        require(lonely.wait() != 0 && lonely.output().find("MISTLANE_MLA1") != std::string::npos,
                "an audience without credentials booted");
        ScopedEnv bad("MISTLANE_MLA1_CREDENTIAL", "ops-callback:key-2026:not-hex:realm1");
        auto& corrupt = c.spawn({self().string(), "--role", "apiServer", "--listen", endpoint(port()),
            "--script", c.path("api.lua").string()});
        require(corrupt.wait() != 0 && corrupt.output().find("MLA1 credential") != std::string::npos,
                "a malformed credential booted");
    }
    // A rotation bound on the row: expired retires the key, live verifies.
    {
        ScopedEnv audience("MISTLANE_MLA1_AUDIENCE", "platform");
        {
            ScopedEnv expired("MISTLANE_MLA1_CREDENTIAL",
                "ops-callback:key-2026:"+hex+":realm1:"+std::to_string(now-10));
            const auto listen = port();
            c.node("apiServer", listen, {"--script", c.path("api.lua").string()});
            const auto sent = http("http://"+endpoint(listen)+"/mla1", "POST", accept_body("grant-old"));
            require(sent.status == 401, "an expired rotation bound was not 401");
            equal(sent.body, reply_body({"UNKNOWN_CREDENTIAL", "", "", "", ""}));
        }
        {
            ScopedEnv bounded("MISTLANE_MLA1_CREDENTIAL",
                "ops-callback:key-2026:"+hex+":realm1:"+std::to_string(now+3600));
            const auto listen = port();
            c.node("apiServer", listen, {"--script", c.path("api.lua").string()});
            const auto sent = http("http://"+endpoint(listen)+"/mla1", "POST", accept_body("grant-live"));
            require(sent.status == 200, "a live rotation bound refused a signed request");
            equal(sent.body, reply_body({"OK", "INSERTED", "VOLATILE", "grant", ""}));
        }
        ScopedEnv bad("MISTLANE_MLA1_CREDENTIAL", "ops-callback:key-2026:"+hex+":realm1:soon");
        auto& refused = c.spawn({self().string(), "--role", "apiServer", "--listen", endpoint(port()),
            "--script", c.path("api.lua").string()});
        require(refused.wait() != 0 && refused.output().find("rotation bound") != std::string::npos,
                "a malformed rotation bound booted");
    }
    std::cout << "api.http surface: signed POST /mla1 inserts, dedupes, refuses [PASS]\n";
    std::cout << "api.http config: env-only enablement, malformed credentials refuse startup [PASS]\n";
}
void transport_listeners() {
    Context c; c.cert();
    write(c.path("echo.lua"), echo);
    write(c.path("ws.lua"), websocket_lua);
    // One process, three listeners: framed tcp plus ws and wss with a
    // per-listener certificate override. The ws listeners stay framed game
    // sessions (Lua message), exactly like single-transport runs.
    const auto plain = port(), ws_port = port(), wss_port = port();
    auto& mixed = c.spawn({self().string(), "--role", "gameServer", "--listen", endpoint(plain),
        "--script", c.path("echo.lua").string(),
        "--listener", "ws://127.0.0.1:"+std::to_string(ws_port),
        "--listener", "wss://127.0.0.1:"+std::to_string(wss_port)+"?cert="+c.path("cert.pem").string()+"&key="+c.path("key.pem").string()});
    mixed.ready("ready gameServer 127.0.0.1:"+std::to_string(wss_port));
    equal(tcp(plain)->request("plain"), "plain");
    equal(cli::make_client({"ws://localhost:"+std::to_string(ws_port)+"/ws"})->request("session"), "session");
    equal(cli::make_client({"wss://localhost:"+std::to_string(wss_port)+"/ws", c.path("cert.pem").string()})->request("secure"), "secure");

    // apiServer: one http listener serving the Lua HTTP service and websocket
    // lifecycle side by side. Plain requests stay with Lua; upgrades upgrade.
    const auto api = port();
    auto& apiServer = c.node("apiServer", api, {"--transport", "http", "--script", c.path("ws.lua").string()});
    const auto origin = "http://"+endpoint(api);
    require(http(origin+"/ws").status == 404, "plain HTTP on the ws path must reach the Lua service");
    require(http(origin+"/missing").status == 404, "Lua HTTP routing broken");
    std::string lifecycle_id;
    {
        WsRaw ws(api);
        ws.text("hello");
        equal(ws.read_text(), "echo:hello");
        ws.text("state");
        const auto state = ws.read_text();
        require(state.starts_with("open=1 closed=0 "), "websocket registry state wrong: "+state);
        lifecycle_id = state.substr(state.rfind('=')+1);
        // A lifecycle message that sends nothing must not wedge the bridge.
        ws.text("noreply");
        ws.text("state");
        require(ws.read_text().starts_with("open=1 closed=0 "), "reply-less message broke the bridge");
        ws.text("oversize");
        equal(ws.read_text(), "guarded");
        ws.text("unknown");
        equal(ws.read_text(), "guarded");
        const auto response = http(origin+"/echo", "POST", "via-lua");
        require(response.status == 201, "HTTP service must share the port with websocket upgrades");
        equal(response.body, "via-lua");
        equal(http(origin+"/state").body, "open=1 closed=0");
    }
    // Clean disconnect: close hook runs, registry bookkeeping updates, same id.
    eventually([&] { return apiServer.output().find("WSHOOK close "+lifecycle_id) != std::string::npos; },
        5s, "websocket close hook did not run");
    eventually([&] { return http(origin+"/state").body == "open=1 closed=1"; }, 5s, "websocket session leak");
    // A Lua failure inside the message callback closes that session only.
    {
        WsRaw ws(api);
        ws.text("boom");
        eventually([&] { return apiServer.output().find("websocket message failed") != std::string::npos; },
            5s, "lua failure in websocket message not logged");
    }
    require(http(origin+"/state").status == 200, "HTTP service must survive a failed websocket session");
    equal(cli::make_client({"ws://localhost:"+std::to_string(api)+"/ws"})->request("still"), "echo:still");
    // Exit strategy: SIGTERM with a live websocket session still exits cleanly.
    {
        WsRaw ws(api);
        ws.text("hello");
        equal(ws.read_text(), "echo:hello");
        apiServer.signal(SIGTERM);
        require(apiServer.wait() == 0, "shutdown with live websocket sessions failed");
    }
    // Slow consumer: pushes past the send bound must close the runaway session
    // while the same process keeps serving fresh ones.
    const auto slow = port();
    auto& lagging = c.node("apiServer", slow, {"--transport", "http", "--script", c.path("ws.lua").string()});
    {
        WsRaw ws(slow);
        ws.text(flood_pushes());
        std::this_thread::sleep_for(500ms); // pushes queue while nobody reads
        const auto drained = ws.drain_until_closed(20s);
        require(drained > 0, "server closed before any flood data reached the client");
        equal(cli::make_client({"ws://localhost:"+std::to_string(slow)+"/ws"})->request("alive"), "echo:alive");
    }
    // HTTPS and WSS on one process: the https primary uses the global cert,
    // the wss listener carries its own via the query override.
    const auto tls_port = port(), wss_extra = port();
    {
        auto cert = tls(c);
        std::vector<std::string> args{"--transport", "https", "--script", c.path("ws.lua").string(),
            "--listener", "wss://127.0.0.1:"+std::to_string(wss_extra)+"?cert="+c.path("cert.pem").string()+"&key="+c.path("key.pem").string()};
        args.insert(args.end(), cert.begin(), cert.end());
        c.node("apiServer", tls_port, args);
    }
    require(http("https://localhost:"+std::to_string(tls_port)+"/state", "GET", {}, c.path("cert.pem").string()).status == 200,
        "https listener not serving the Lua service");
    // The wss listener stays a framed game session: message echo, not lifecycle.
    equal(cli::make_client({"wss://localhost:"+std::to_string(wss_extra)+"/ws", c.path("cert.pem").string()})->request("tls"), "tls");
    // Gateway: framed tcp listener beside an http listener, both forwarding.
    const auto gate = port(), gate_http = port();
    c.node("gateway", gate, {"--upstream", endpoint(plain), "--listener", "http://127.0.0.1:"+std::to_string(gate_http)});
    equal(tcp(gate)->request("tcp-via-gateway"), "tcp-via-gateway");
    const auto forwarded = http("http://"+endpoint(gate_http)+"/message", "POST", std::string("\0\xff", 2));
    require(forwarded.status == 200, "gateway /message forwarding");
    equal(forwarded.body, std::string("\0\xff", 2));
    equal(cli::make_client({"ws://localhost:"+std::to_string(gate_http)+"/ws"})->request("ws-via-gateway"), "ws-via-gateway");
    // Config files carry LISTENERS like any other key.
    const auto cfg_plain = port(), cfg_ws = port();
    write(c.path("node.conf"), "# mistlane-config 1\nROLE=gameServer\nLISTEN=127.0.0.1:"+std::to_string(cfg_plain)+
        "\nSCRIPT="+c.path("echo.lua").string()+"\nLISTENERS=ws://127.0.0.1:"+std::to_string(cfg_ws)+"\n");
    auto& configured = c.spawn({self().string(), "--config", c.path("node.conf").string()});
    configured.ready("ready gameServer 127.0.0.1:"+std::to_string(cfg_ws));
    equal(tcp(cfg_plain)->request("config"), "config");
    equal(cli::make_client({"ws://localhost:"+std::to_string(cfg_ws)+"/ws"})->request("config-ws"), "config-ws");
    // Conflicting listener sets must fail at startup, never half-serve.
    must_fail([&] { c.node("apiServer", port(), {"--transport", "http", "--script", c.path("ws.lua").string(),
        "--listener", "tcp://127.0.0.1:"+std::to_string(port())}); });
    must_fail([&] { const auto reused = port();
        c.node("gameServer", port(), {"--script", c.path("echo.lua").string(),
            "--listener", "tcp://127.0.0.1:"+std::to_string(reused),
            "--listener", "ws://127.0.0.1:"+std::to_string(reused)}); });
}
// Framed game sessions carry a session identity and accept Lua pushes:
// mistlane.session.send/broadcast deliver frames without a matching request,
// a handler may answer nothing, and outbound pushes stay bounded per session.
// Optional correlation mode: the first frame "MLC2" switches the session to
// u32 BE request ids carried inside the frame payload; pushes carry id 0.
void transport_correlate() {
    Context c;
    write(c.path("correlate.lua"), correlate_lua);
    const auto gameServer = port(), gateway = port(), ws_port = port();
    auto& node = c.spawn({self().string(), "--role", "gameServer", "--listen", endpoint(gameServer),
        "--script", c.path("correlate.lua").string(),
        "--listener", "ws://127.0.0.1:"+std::to_string(ws_port)});
    node.ready("ready gameServer "+endpoint(gameServer));
    c.node("gateway", gateway, {"--upstream", endpoint(gameServer)});
    const auto send_frame = [](RawSocket& raw, std::string_view payload) {
        std::string frame(4, '\0');
        auto length = static_cast<std::uint32_t>(payload.size());
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(length & 255); length >>= 8; }
        raw.send(frame+std::string(payload));
    };
    const auto read_frame = [](RawSocket& raw) {
        const auto header = raw.receive(4);
        std::size_t length = 0;
        for (unsigned char ch : header) length = (length << 8) | ch;
        return raw.receive(length);
    };
    const auto tagged = [](std::uint32_t id, std::string_view payload) {
        std::string frame(4, '\0');
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(id & 255); id >>= 8; }
        return frame+std::string(payload);
    };
    std::uint32_t id = 0;
    std::string body;
    const auto split = [&id, &body](const std::string& frame) {
        require(frame.size() >= 4, "correlated frame too short");
        id = 0;
        for (int i = 0; i < 4; ++i) id = (id<<8) | static_cast<unsigned char>(frame[i]);
        body = frame.substr(4);
    };
    // Handshake, then replies echo the request id byte for byte.
    RawSocket first(gameServer);
    send_frame(first, "MLC2");
    equal(read_frame(first), "MLC2 OK");
    send_frame(first, tagged(0x01020304, "hello"));
    split(read_frame(first));
    require(id == 0x01020304 && body == "hello", "correlated echo lost id or payload");
    send_frame(first, tagged(9, "self"));
    split(read_frame(first));
    require(id == 9 && body.starts_with("session=") && body.ends_with(":req=9"),
        "lua request id missing: "+body);
    // A nil reply still means no frame; pushes carry id 0 and precede the reply.
    send_frame(first, tagged(5, "silent"));
    std::this_thread::sleep_for(300ms);
    require(!first.pending(), "nil reply produced a frame");
    send_frame(first, tagged(6, "push"));
    split(read_frame(first));
    require(id == 0 && body == "pushed", "push must stay uncorrelated");
    split(read_frame(first));
    require(id == 6 && body == "done", "reply after push lost its id");
    // An oversized correlated push reports instead of failing the handler.
    send_frame(first, tagged(7, "big"));
    split(read_frame(first));
    require(id == 7 && body == "big=false:payload exceeds 1 MiB", "oversize push not reported: "+body);
    // A plain client on the same server stays uncorrelated; broadcast shapes differ.
    RawSocket second(gameServer);
    send_frame(second, "hello");
    equal(read_frame(second), "hello");
    send_frame(second, "bcast");
    split(read_frame(first));
    require(id == 0 && body == "bc", "broadcast to a correlated session must carry id 0");
    equal(read_frame(second), "bc");
    equal(read_frame(second), "bcast=2");
    // The handshake is honored as the first frame only.
    send_frame(second, "MLC2");
    equal(read_frame(second), "MLC2");
    // The same contract through a gateway: ids survive the relay.
    RawSocket via(gateway);
    send_frame(via, "MLC2");
    equal(read_frame(via), "MLC2 OK");
    send_frame(via, tagged(0x0A0B0C0D, "ping"));
    split(read_frame(via));
    require(id == 0x0A0B0C0D && body == "ping", "gateway lost correlation");
    send_frame(via, tagged(3, "self"));
    split(read_frame(via));
    require(id == 3 && body.ends_with(":req=3"), "gateway lua request id missing: "+body);
    // The ws game-session leg speaks the same mode in binary frames.
    {
        WsRaw over(ws_port);
        over.text("MLC2");
        equal(over.read_text(), "MLC2 OK");
        over.bin(tagged(11, "w"));
        split(over.read_text());
        require(id == 11 && body == "w", "ws correlated echo failed");
    }
    // Cross routing keeps its own handshake and rejects correlation.
    const auto shard = port(), cross_gate = port();
    c.node("crossServer", shard, {"--script", c.path("correlate.lua").string()});
    c.cross_gateway(cross_gate, {"--upstream", endpoint(shard)});
    {
        RawSocket crossed(cross_gate);
        send_frame(crossed, "MLC2");
        equal(read_frame(crossed), "MLC1 ERR INVALID");
    }
    require(node.output().find("callback failed") == std::string::npos, "correlation path logged Lua failures");
}
// MLS1 identifies internal legs: a node with --service-token only serves
// peers that present "MLS1 <role> <instance> <token>", a gateway with
// --upstream-token presents itself before any business frame, and every
// refusal closes the connection with a reason instead of leaking frames into
// the script.
void transport_identity() {
    Context c;
    write(c.path("correlate.lua"), correlate_lua);
    const auto gameServer = port(), open_server = port(), gateway = port(),
              no_token = port(), wrong_token = port(), mismatched = port();
    auto& node = c.spawn({self().string(), "--role", "gameServer", "--listen", endpoint(gameServer),
        "--script", c.path("correlate.lua").string(), "--service-token", "s3cret",
        "--service-name", "game-1"});
    auto& open_node = c.spawn({self().string(), "--role", "gameServer", "--listen", endpoint(open_server),
        "--script", c.path("correlate.lua").string()});
    node.ready("ready gameServer " + endpoint(gameServer));
    open_node.ready("ready gameServer " + endpoint(open_server));
    c.node("gateway", gateway, {"--upstream", endpoint(gameServer),
        "--upstream-token", "s3cret", "--service-name", "gwy-1"});
    c.node("gateway", no_token, {"--upstream", endpoint(gameServer)});
    c.node("gateway", wrong_token, {"--upstream", endpoint(gameServer), "--upstream-token", "nope"});
    c.node("gateway", mismatched, {"--upstream", endpoint(open_server), "--upstream-token", "s3cret"});
    const auto send_frame = [](RawSocket& raw, std::string_view payload) {
        std::string frame(4, '\0');
        auto length = static_cast<std::uint32_t>(payload.size());
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(length & 255); length >>= 8; }
        raw.send(frame+std::string(payload));
    };
    const auto read_frame = [](RawSocket& raw) {
        const auto header = raw.receive(4);
        std::size_t length = 0;
        for (unsigned char ch : header) length = (length << 8) | ch;
        return raw.receive(length);
    };
    const auto tagged = [](std::uint32_t id, std::string_view payload) {
        std::string frame(4, '\0');
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(id & 255); id >>= 8; }
        return frame+std::string(payload);
    };
    std::uint32_t id = 0;
    std::string body;
    const auto split = [&id, &body](const std::string& frame) {
        require(frame.size() >= 4, "correlated frame too short");
        id = 0;
        for (int i = 0; i < 4; ++i) id = (id<<8) | static_cast<unsigned char>(frame[i]);
        body = frame.substr(4);
    };
    const auto expect_close = [](RawSocket& raw) {
        for (int i = 0; i < 40 && !raw.closed(); ++i) std::this_thread::sleep_for(50ms);
        require(raw.closed(), "connection stayed open after a refusal");
    };
    // An unauthenticated first frame never reaches the script.
    RawSocket anonymous(gameServer);
    send_frame(anonymous, "self");
    equal(read_frame(anonymous), "MLS1 ERR TOKEN");
    expect_close(anonymous);
    // Malformed handshakes report the shape, not the secret.
    for (const auto& hello : {"MLS1 onlyrole", "MLS1 a b c d"}) {
        RawSocket malformed(gameServer);
        send_frame(malformed, hello);
        equal(read_frame(malformed), "MLS1 ERR INVALID");
        expect_close(malformed);
    }
    RawSocket refused(gameServer);
    send_frame(refused, "MLS1 gateway inst-9 nope");
    equal(read_frame(refused), "MLS1 ERR TOKEN");
    expect_close(refused);
    // Correct credentials admit the session; the handshake stays invisible after.
    RawSocket joined(gameServer);
    send_frame(joined, "MLS1 gateway inst-9 s3cret");
    equal(read_frame(joined), "MLS1 OK");
    send_frame(joined, "self");
    body = read_frame(joined);
    require(body.starts_with("session=") && body.ends_with(":req=nil"), "handshaken session broken: "+body);
    // MLC2 stacks on the accepted handshake: both close at the first payload.
    RawSocket correlated(gameServer);
    send_frame(correlated, "MLS1 gateway inst-9 s3cret");
    equal(read_frame(correlated), "MLS1 OK");
    send_frame(correlated, "MLC2");
    equal(read_frame(correlated), "MLC2 OK");
    send_frame(correlated, tagged(9, "self"));
    split(read_frame(correlated));
    require(id == 9 && body.ends_with(":req=9"), "correlation lost after the handshake: "+body);
    // A token-bearing gateway shows no handshake at either end.
    RawSocket via(gateway);
    send_frame(via, "MLC2");
    equal(read_frame(via), "MLC2 OK");
    send_frame(via, tagged(3, "ping"));
    split(read_frame(via));
    require(id == 3 && body == "ping", "gateway handshake leaked into the relay: "+body);
    RawSocket plain_via(gateway);
    send_frame(plain_via, "self");
    body = read_frame(plain_via);
    require(body.starts_with("session=") && body.ends_with(":req=nil"), "gateway session broke: "+body);
    // An upstream refusal ends the session without relaying the verdict.
    RawSocket silent(wrong_token);
    send_frame(silent, "ping");
    std::this_thread::sleep_for(300ms);
    require(!silent.pending(), "upstream verdict leaked to the client");
    expect_close(silent);
    // A gateway without a token cannot hide the mismatch, so it relays the reason.
    RawSocket relayed(no_token);
    send_frame(relayed, "ping");
    equal(read_frame(relayed), "MLS1 ERR TOKEN");
    expect_close(relayed);
    // A server that needs no handshake refuses the stray one.
    RawSocket to_plain(open_server);
    send_frame(to_plain, "MLS1 gateway inst-9 s3cret");
    equal(read_frame(to_plain), "MLS1 ERR UNEXPECTED");
    expect_close(to_plain);
    RawSocket via_open(mismatched);
    send_frame(via_open, "ping");
    std::this_thread::sleep_for(300ms);
    require(!via_open.pending(), "server refusal leaked to the client");
    expect_close(via_open);
    // ...and keeps serving anonymous clients.
    RawSocket open_client(open_server);
    send_frame(open_client, "hello");
    equal(read_frame(open_client), "hello");
    // Cross routing keeps its own identity, so the token flags refuse to start.
    const auto cross_arg = std::vector<std::string>{"--routing", "cross"};
    for (const auto& extra : {std::vector<std::string>{"--service-token", "s3cret"},
                              std::vector<std::string>{"--upstream-token", "s3cret"}}) {
        auto args = std::vector<std::string>{self().string(), "--role", "gateway",
            "--listen", endpoint(port()), "--upstream", endpoint(gameServer)};
        args.insert(args.end(), cross_arg.begin(), cross_arg.end());
        args.insert(args.end(), extra.begin(), extra.end());
        auto& child = c.spawn(args);
        require(child.wait() != 0 && child.output().find("does not apply to cross routing") != std::string::npos,
                "token flags accepted on cross routing");
    }
    for (const auto& node_output : {node.output(), open_node.output()})
        require(node_output.find("callback failed") == std::string::npos, "handshake path logged Lua failures");
}
// The gateway --upstream-multiplex mode (MLM1) shares one upstream connection
// across client sessions: logical sessions stay isolated, pushes and broadcast
// address a single session, a Lua error or disconnect closes only its own
// logical session, and plain clients are told off instead of upgrading.
void transport_multiplex() {
    Context c;
    write(c.path("correlate.lua"), correlate_lua);
    const auto gameServer = port(), metrics = port(), gateway = port();
    auto& node = c.spawn({self().string(), "--role", "gameServer", "--listen", endpoint(gameServer),
        "--script", c.path("correlate.lua").string(), "--service-token", "s3cret",
        "--metrics-listen", endpoint(metrics)});
    // Gateway starts its shared MuxLink on the first client. Wait until the
    // upstream listener has announced readiness before allowing that connect.
    node.ready("ready gameServer");
    c.node("gateway", gateway, {"--upstream", endpoint(gameServer),
        "--upstream-token", "s3cret", "--upstream-multiplex", "1"});
    std::string phase = "initial client echo";
    const auto send_frame = [](RawSocket& raw, std::string_view payload) {
        std::string frame(4, '\0');
        auto length = static_cast<std::uint32_t>(payload.size());
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(length & 255); length >>= 8; }
        raw.send(frame+std::string(payload));
    };
    const auto read_frame = [&phase](RawSocket& raw) {
        try {
            const auto header = raw.receive(4);
            std::size_t length = 0;
            for (unsigned char ch : header) length = (length << 8) | ch;
            return raw.receive(length);
        } catch (const std::exception& error) {
            throw std::runtime_error("multiplex phase '"+phase+"': "+error.what());
        }
    };
    const auto tagged = [](std::uint32_t id, std::string_view payload) {
        std::string frame(4, '\0');
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(id & 255); id >>= 8; }
        return frame+std::string(payload);
    };
    std::uint32_t id = 0;
    std::string body;
    const auto split = [&id, &body](const std::string& frame) {
        require(frame.size() >= 4, "correlated frame too short");
        id = 0;
        for (int i = 0; i < 4; ++i) id = (id<<8) | static_cast<unsigned char>(frame[i]);
        body = frame.substr(4);
    };
    const auto expect_close = [](RawSocket& raw) {
        for (int i = 0; i < 40 && !raw.closed(); ++i) std::this_thread::sleep_for(50ms);
        require(raw.closed(), "connection stayed open after a refusal");
    };
    const auto established = [](unsigned port) {
        char hex[8];
        std::snprintf(hex, sizeof hex, "%04X", port);
        std::ifstream net("/proc/net/tcp");
        std::string sl, local, remote, state, rest;
        int count = 0;
        while (net >> sl >> local >> remote >> state) {
            if (state == "01" && local.ends_with(":"+std::string(hex))) ++count;
            std::getline(net, rest);
        }
        return count;
    };
    // Two clients ride one upstream connection; the server still counts both.
    RawSocket a(gateway), b(gateway);
    send_frame(a, "hello"); equal(read_frame(a), "hello");
    send_frame(b, "world"); equal(read_frame(b), "world");
    // Synchronize on server admission and the kernel socket state. The fixed
    // sleep used to sample /proc/net/tcp before the shared connection was
    // visible under load.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    int upstream_connections = 0;
    std::string live;
    do {
        live = http("http://"+endpoint(metrics)+"/metrics").body;
        upstream_connections = established(gameServer);
        if (upstream_connections == 1 &&
            live.find("mistlane_active_sessions{role=\"gameServer\"} 2") != std::string::npos) break;
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    require(upstream_connections == 1, "expected one shared upstream TCP connection; observed " + std::to_string(upstream_connections));
    require(live.find("mistlane_active_sessions{role=\"gameServer\"} 2") != std::string::npos,
            "logical sessions missing from server admission");
    // Correlation is a first-frame handshake, so it lives on a third client and
    // must survive the envelope of a session it never shared.
    RawSocket corr(gateway);
    phase = "correlation handshake";
    send_frame(corr, "MLC2"); equal(read_frame(corr), "MLC2 OK");
    send_frame(corr, tagged(5, "self"));
    split(read_frame(corr));
    require(id == 5 && body.ends_with(":req=5"), "correlation lost through the shared leg: "+body);
    send_frame(b, "self");
    body = read_frame(b);
    require(body.starts_with("session=") && body.ends_with(":req=nil"), "plain session broken: "+body);
    // A push and a broadcast stay inside their logical session.
    phase = "push and correlated reply";
    send_frame(corr, tagged(6, "push"));
    split(read_frame(corr)); require(id == 0 && body == "pushed", "push lost on the shared leg");
    split(read_frame(corr)); require(id == 6 && body == "done", "reply lost after a push");
    require(!b.pending(), "push leaked into another logical session");
    // A directly authenticated session shares the registry too, so the count
    // reaches four: a, b, corr and the direct peer.
    phase = "direct peer and broadcast";
    RawSocket direct(gameServer);
    send_frame(direct, "MLS1 gateway inst-9 s3cret");
    equal(read_frame(direct), "MLS1 OK");
    send_frame(direct, "hi"); equal(read_frame(direct), "hi");
    send_frame(b, "bcast");
    split(read_frame(corr)); require(id == 0 && body == "bc", "broadcast shape lost on the correlated session");
    equal(read_frame(a), "bc");
    equal(read_frame(b), "bc"); equal(read_frame(b), "bcast=4");
    equal(read_frame(direct), "bc");
    // A Lua error closes only that logical session.
    {
        phase = "isolated Lua error";
        RawSocket err(gateway);
        send_frame(err, "oops");
        expect_close(err);
    }
    require(node.output().find("mux oops") != std::string::npos,
            "logical session error not reported");
    phase = "survivor after Lua error";
    send_frame(b, "again"); equal(read_frame(b), "again");
    // A disconnect retires its logical session; the rest of the hub survives.
    phase = "disconnect retirement";
    {
        RawSocket gone(gateway);
        send_frame(gone, "bye"); equal(read_frame(gone), "bye");
        send_frame(b, "bcast");
        equal(read_frame(b), "bc"); equal(read_frame(b), "bcast=5");
        equal(read_frame(a), "bc"); // the survivors read the broadcast twice
    }
    std::this_thread::sleep_for(300ms);
    send_frame(b, "bcast");
    equal(read_frame(b), "bc"); equal(read_frame(b), "bcast=4");
    equal(read_frame(a), "bc");
    // Nobody upgrades mid-session, whether through the gateway or directly.
    // Check for malformed hub traffic before the deliberate MLM1 refusals
    // below: those expected refusals are logged as "unexpected frame" too.
    require(node.output().find("unexpected frame") == std::string::npos,
            "hub parsed a malformed frame before the refusal cases");
    phase = "mid-session refusal probes";
    send_frame(a, "MLM1"); equal(read_frame(a), "MLM1");
    {
        RawSocket mid(gateway);
        send_frame(mid, "hi"); equal(read_frame(mid), "hi");
        send_frame(mid, "MLM1"); equal(read_frame(mid), "MLM1");
    }
    RawSocket plain(gameServer);
    send_frame(plain, "MLM1");
    equal(read_frame(plain), "MLM1 ERR UNEXPECTED");
    expect_close(plain);
    {
        RawSocket through(gateway);
        send_frame(through, "MLM1");
        equal(read_frame(through), "MLM1 ERR UNEXPECTED");
        expect_close(through);
    }
    // Multiplexing is an internal mode, so it needs the identity handshake.
    {
        auto& bad = c.spawn({self().string(), "--role", "gateway", "--listen", endpoint(port()),
                             "--upstream", endpoint(gameServer), "--upstream-multiplex", "1"});
        require(bad.wait() != 0 && bad.output().find("requires --upstream-token") != std::string::npos,
                "multiplex accepted without a service identity");
    }
}
// The coordinator role (MLR1) keeps a live registry instead of forwarding
// gameplay: members register with a scope and heartbeat readiness/load, a
// registry client lists or places against it, disconnects drop entries, and a
// token-requiring coordinator admits only identified members.
void coordinator_registry() {
    Context c;
    write(c.path("echo.lua"), correlate_lua);
    const auto registry = port(), metrics = port(), tokened = port(),
              first = port(), second = port(), third = port();
    auto& coord = c.node("coordinator", registry, {"--metrics-listen", endpoint(metrics)});
    c.node("coordinator", tokened, {"--service-token", "s3cret"});
    const auto send_frame = [](RawSocket& raw, std::string_view payload) {
        std::string frame(4, '\0');
        auto length = static_cast<std::uint32_t>(payload.size());
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(length & 255); length >>= 8; }
        raw.send(frame+std::string(payload));
    };
    const auto read_frame = [](RawSocket& raw) {
        const auto header = raw.receive(4);
        std::size_t length = 0;
        for (unsigned char ch : header) length = (length << 8) | ch;
        return raw.receive(length);
    };
    const auto expect_close = [](RawSocket& raw) {
        for (int i = 0; i < 40 && !raw.closed(); ++i) std::this_thread::sleep_for(50ms);
        require(raw.closed(), "connection stayed open after a refusal");
    };
    // The registry is not a forwarding role: unknown traffic is refused.
    RawSocket stranger(registry);
    send_frame(stranger, "hello");
    equal(read_frame(stranger), "MLR1 ERR UNEXPECTED");
    expect_close(stranger);
    // Malformed registrations report the shape.
    for (const auto& bad : {"MLR1 REGISTER", "MLR1 REGISTER data gs-x alpha 127.0.0.1:1 0",
                            "MLR1 REGISTER gameServer gs-x alpha not-an-endpoint 0"}) {
        RawSocket malformed(registry);
        send_frame(malformed, bad);
        equal(read_frame(malformed), "MLR1 ERR INVALID");
        expect_close(malformed);
    }
    // Members join two scopes; the first saturates under real load.
    auto& gs_a = c.node("gameServer", first, {"--script", c.path("echo.lua").string(),
        "--coordinator", endpoint(registry), "--scope", "alpha", "--service-name", "gs-a",
        "--max-connections", "4", "--overload-ratio", "0.5", "--ready-hysteresis", "1"});
    auto& gs_b = c.node("gameServer", second, {"--script", c.path("echo.lua").string(),
        "--coordinator", endpoint(registry), "--scope", "alpha", "--service-name", "gs-b"});
    auto& gs_c = c.node("gameServer", third, {"--script", c.path("echo.lua").string(),
        "--coordinator", endpoint(registry), "--scope", "beta", "--service-name", "gs-c"});
    std::string body;
    const auto query = [&](const std::string& line) {
        RawSocket raw(registry);
        send_frame(raw, line);
        body = read_frame(raw);
        expect_close(raw);
        return body;
    };
    const auto place = [&](const std::string& line) {
        return query(line);
    };
    eventually([&] { return query("MLR1 QUERY").find("MLR1 MEMBERS 3") != std::string::npos; },
        5s, "members never reached the registry");
    require(body.find("member gameServer gs-a alpha") != std::string::npos &&
            body.find("member gameServer gs-b alpha") != std::string::npos &&
            body.find("member gameServer gs-c beta") != std::string::npos,
            "registry listing lost a member: "+body);
    // Scope filters narrow the listing and placement.
    query("MLR1 QUERY gameServer beta");
    require(body.find("gs-c") != std::string::npos && body.find("gs-a") == std::string::npos,
            "scope filter leaked another scope: "+body);
    const auto beta_place = "MLR1 PLACE gs-c 127.0.0.1:"+std::to_string(third)+" 0 ready 0/42";
    eventually([&] { return place("MLR1 PLACE gameServer beta") == beta_place; },
        5s, "gs-c never became placeable");
    equal(place("MLR1 PLACE gameServer beta"), beta_place);
    equal(place("MLR1 PLACE gameServer gamma"), "MLR1 EMPTY");
    // Placement prefers the less loaded member once real load differs.
    {
        RawSocket load(first);
        send_frame(load, "ping"); equal(read_frame(load), "ping");
        eventually([&] { return place("MLR1 PLACE gameServer alpha") ==
            "MLR1 PLACE gs-b 127.0.0.1:"+std::to_string(second)+" 0 ready 0/42"; },
            5s, "placement ignored member load");
        // Saturation flips readiness, and the registry follows the heartbeats.
        RawSocket full(first);
        send_frame(full, "ping"); equal(read_frame(full), "ping");
        eventually([&] { return query("MLR1 QUERY gameServer alpha").find(
            "member gameServer gs-a alpha 127.0.0.1:"+std::to_string(first)+" 0 notready 2/4")
            != std::string::npos; }, 5s, "readiness did not follow the heartbeat");
    }
    std::this_thread::sleep_for(600ms);
    eventually([&] { return query("MLR1 QUERY gameServer alpha").find(
        "member gameServer gs-a alpha 127.0.0.1:"+std::to_string(first)+" 0 ready 0/4")
        != std::string::npos; }, 5s, "readiness did not recover");
    // A disconnect drops the member from the registry.
    gs_c.signal(SIGTERM);
    eventually([&] { return query("MLR1 QUERY").find("MLR1 MEMBERS 2") != std::string::npos; },
        5s, "a dead member stayed registered");
    // The registry gauge follows the live membership.
    const auto live = http("http://"+endpoint(metrics)+"/metrics").body;
    require(live.find("mistlane_registry_members{role=\"coordinator\"} 2") != std::string::npos,
            "registry gauge missing from the exposition");
    // Identity stacks on the registry: an unnamed member is refused, an
    // identified one registers normally.
    RawSocket anonymous(tokened);
    send_frame(anonymous, "MLR1 REGISTER gameServer gs-x alpha 127.0.0.1:1 0");
    equal(read_frame(anonymous), "MLS1 ERR TOKEN");
    expect_close(anonymous);
    auto& gs_d = c.node("gameServer", port(), {"--script", c.path("echo.lua").string(),
        "--coordinator", endpoint(tokened), "--upstream-token", "s3cret", "--service-name", "gs-d"});
    const auto identified = [&]() {
        RawSocket raw(tokened);
        send_frame(raw, "MLS1 registryClient query-1 s3cret");
        equal(read_frame(raw), "MLS1 OK");
        send_frame(raw, "MLR1 QUERY");
        body = read_frame(raw);
        expect_close(raw);
        return body;
    };
    eventually([&] { return identified().find("member gameServer gs-d main") != std::string::npos; },
        5s, "an identified member never registered");
    require(gs_a.output().find("registered") != std::string::npos &&
            gs_d.output().find("registered") != std::string::npos,
            "member registration not logged");
    // The coordinator is not a renamed router: it takes neither script nor
    // upstreams, and the member flags do not leak into other roles.
    {
        auto& scripted = c.spawn({self().string(), "--role", "coordinator", "--listen", endpoint(port()),
            "--script", c.path("echo.lua").string()});
        require(scripted.wait() != 0 && scripted.output().find("requires no script and no upstream") != std::string::npos,
                "coordinator accepted a script");
        auto& routed = c.spawn({self().string(), "--role", "gateway", "--listen", endpoint(port()),
            "--upstream", endpoint(registry), "--coordinator", endpoint(registry)});
        require(routed.wait() != 0 && routed.output().find("--coordinator applies to gameServer, crossServer and login") != std::string::npos,
                "gateway accepted a coordinator");
        auto& scoped = c.spawn({self().string(), "--role", "gameServer", "--listen", endpoint(port()),
            "--script", c.path("echo.lua").string(), "--scope", "alpha"});
        require(scoped.wait() != 0 && scoped.output().find("--scope requires --coordinator") != std::string::npos,
                "scope accepted without a coordinator");
    }
    for (const auto* member : {&gs_a, &gs_b})
        require(member->output().find("callback failed") == std::string::npos, "registry member logged Lua failures");
    require(coord.output().find("unexpected frame") == std::string::npos, "coordinator log noise");
}
// login is a registry client, not a member: each placement ask owns one
// connection, and the callback answers through mistlane.session.send because
// the answer arrives after the handler returned.
void coordinator_placement() {
    Context c;
    write(c.path("echo.lua"), echo);
    write(c.path("place.lua"),
        "return {message=function(p,c)"
        " local role, scope = p:match('place (%S+) ?(%S*)')\n"
        " if not role then return 'usage' end\n"
        " mistlane.place(role, scope ~= '' and scope or nil, function(err, address)\n"
        "   mistlane.session.send(c.session, err and ('ERR '..err) or ('OK '..address))\n"
        " end)\n"
        " return nil end}");
    const auto send_frame = [](RawSocket& raw, std::string_view payload) {
        std::string frame(4, '\0');
        auto length = static_cast<std::uint32_t>(payload.size());
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(length & 255); length >>= 8; }
        raw.send(frame + std::string(payload));
    };
    const auto read_frame = [](RawSocket& raw) {
        std::size_t length = 0;
        for (unsigned char ch : raw.receive(4)) length = (length << 8) | ch;
        return raw.receive(length);
    };
    const auto place = [&](unsigned listen, std::string_view request) {
        RawSocket raw(listen);
        send_frame(raw, request);
        return read_frame(raw);
    };
    const auto members = [&](unsigned metrics) {
        const auto body = http("http://"+endpoint(metrics)+"/metrics").body;
        const auto needle = std::string("mistlane_registry_members{role=\"coordinator\"} ");
        const auto at = body.find(needle);
        return at == std::string::npos ? -1 : static_cast<int>(std::strtod(body.c_str()+at+needle.size(), nullptr));
    };
    const auto registry = port(), member_listen = port(), login = port(), metrics = port(),
             token_registry = port(), token_member = port(), token_login = port(),
             wrong_login = port(), dead_login = port();
    auto& coord = c.node("coordinator", registry, {"--metrics-listen", endpoint(metrics)});
    auto& member = c.spawn({self().string(), "--role", "gameServer", "--listen", endpoint(member_listen),
             "--script", c.path("echo.lua").string(), "--coordinator", endpoint(registry)});
    member.ready("ready gameServer " + endpoint(member_listen));
    eventually([&] { return members(metrics) == 1; }, 5s, "the member never registered");
    c.node("login", login, {"--script", c.path("place.lua").string(), "--coordinator", endpoint(registry)});
    // Registration precedes the first readiness heartbeat, so the ask waits
    // for the ready verdict instead of racing it.
    eventually([&] { return place(login, "place gameServer")
        == "OK 127.0.0.1:"+std::to_string(member_listen); }, 5s, "placement never saw the ready member");
    equal(place(login, "place gameServer nobody"), "ERR EMPTY");
    equal(place(login, "place crossServer"), "ERR EMPTY");
    equal(place(login, "where"), "usage");
    require(members(metrics) == 1, "login registered itself as a member");
    // A tokened coordinator gates the lookup behind MLS1.
    auto& token_coord = c.node("coordinator", token_registry, {"--service-token", "s3cret"});
    auto& token_member_node = c.spawn({self().string(), "--role", "gameServer", "--listen", endpoint(token_member),
             "--script", c.path("echo.lua").string(), "--coordinator", endpoint(token_registry),
             "--upstream-token", "s3cret", "--service-name", "gs-token"});
    token_member_node.ready("ready gameServer " + endpoint(token_member));
    c.node("login", token_login, {"--script", c.path("place.lua").string(),
        "--coordinator", endpoint(token_registry), "--upstream-token", "s3cret", "--service-name", "login-1"});
    c.node("login", wrong_login, {"--script", c.path("place.lua").string(),
        "--coordinator", endpoint(token_registry), "--upstream-token", "nope"});
    eventually([&] { return place(token_login, "place gameServer")
        == "OK 127.0.0.1:"+std::to_string(token_member); }, 5s, "identity did not reach the tokened registry");
    equal(place(wrong_login, "place gameServer"), "ERR TOKEN");
    // A dead coordinator reports a reason instead of holding the session.
    c.node("login", dead_login, {"--script", c.path("place.lua").string(), "--coordinator", endpoint(port())});
    equal(place(dead_login, "place gameServer"), "ERR unreachable");
    // The registry stays the coordinator's job: data never presents itself.
    auto& data = c.spawn({self().string(), "--role", "data", "--listen", endpoint(port()),
        "--script", c.path("place.lua").string(), "--coordinator", endpoint(registry)});
    require(data.wait() != 0 && data.output().find("--coordinator applies to gameServer, crossServer and login")
        != std::string::npos, "data accepted registry placement");
    require(coord.output().find("member registered login") == std::string::npos, "login registered as a member");
    require(coord.output().find("unexpected frame") == std::string::npos, "coordinator log noise");
    require(token_coord.output().find("unexpected frame") == std::string::npos, "coordinator log noise");
}
void transport_push() {
    Context c;
    write(c.path("push.lua"), push_lua);
    const auto gameServer = port(), gateway = port(), ws_port = port();
    auto& node = c.spawn({self().string(), "--role", "gameServer", "--listen", endpoint(gameServer),
        "--script", c.path("push.lua").string(),
        "--listener", "ws://127.0.0.1:"+std::to_string(ws_port)});
    node.ready("ready gameServer " + endpoint(gameServer));
    c.node("gateway", gateway, {"--upstream", endpoint(gameServer)});
    const auto send_frame = [](RawSocket& raw, std::string_view payload) {
        std::string frame(4, '\0');
        auto length = static_cast<std::uint32_t>(payload.size());
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(length & 255); length >>= 8; }
        raw.send(frame+std::string(payload));
    };
    const auto read_frame = [](RawSocket& raw) {
        const auto header = raw.receive(4);
        std::size_t length = 0;
        for (unsigned char ch : header) length = (length << 8) | ch;
        return raw.receive(length);
    };
    RawSocket first(gameServer);
    send_frame(first, "self");
    equal(read_frame(first), "session=1");
    // A handler may answer nothing; the session must stay usable afterwards.
    send_frame(first, "silent");
    std::this_thread::sleep_for(300ms);
    require(!first.pending(), "nil handler reply produced a frame");
    send_frame(first, "self");
    equal(read_frame(first), "session=1");
    // Lua push reaches the client without a request; the reply follows the pushes.
    send_frame(first, "push");
    equal(read_frame(first), "pushed");
    equal(read_frame(first), "done");
    // Broadcast reaches every session; each client sees the initiator's id.
    RawSocket second(gameServer);
    send_frame(second, "self");
    equal(read_frame(second), "session=2");
    send_frame(first, "bcast");
    equal(read_frame(second), "bc:1");
    equal(read_frame(first), "bc:1");
    equal(read_frame(first), "bcast=2");
    // Push bounds: per-frame cap, unknown ids and the backlog report, not close.
    send_frame(first, "burst 64");
    std::string burst;
    std::size_t pushed = 0;
    for (;;) {
        burst = read_frame(first);
        if (burst.starts_with("burst=")) break;
        require(burst.size() == 64*1024 && burst.find_first_not_of('B') == std::string::npos,
            "pushed frame corrupted");
        ++pushed;
        require(pushed <= 64, "more push frames queued than attempts");
    }
    const auto colon = burst.find(':');
    const auto queued = std::stoul(burst.substr(6, colon-6));
    const auto refused = std::stoul(burst.substr(colon+1));
    require(queued == pushed && refused > 0 && queued+refused == 64, "push bounds not enforced: "+burst);
    // The same contract through a gateway: pushes relay unsolicited.
    RawSocket via(gateway);
    send_frame(via, "self");
    equal(read_frame(via), "session=3");
    send_frame(via, "push");
    equal(read_frame(via), "pushed");
    equal(read_frame(via), "done");
    // Pushes and broadcast also work on the ws game-session leg.
    {
        WsRaw over(ws_port);
        over.text("self");
        equal(over.read_text(), "session=4");
        over.text("push");
        equal(over.read_text(), "pushed");
        equal(over.read_text(), "done");
        over.text("bcast");
        equal(over.read_text(), "bc:4");
        equal(over.read_text(), "bcast=4");
    }
    require(node.output().find("callback failed") == std::string::npos, "push path logged Lua failures");
}
void lua_lifecycle() {
    Context c; const auto script = c.path("service.lua"); write(script, echo);
    const auto listen = port();
    auto& gameServer = c.node("gameServer", listen, {"--script", script.string()});
    auto client = tcp(listen); equal(client->request("v1"), "v1");
    write(script, "return {message=function(p) return 'v2:'..p end}");
    gameServer.signal(SIGHUP); gameServer.ready("script reloaded");
    equal(client->request("x"), "v2:x");
    write(script, "invalid lua !!!");
    gameServer.signal(SIGHUP); gameServer.ready("reload rejected");
    equal(client->request("x"), "v2:x");
    write(script, "return {}");
    gameServer.signal(SIGHUP); gameServer.ready("service.message is required");
    equal(client->request("x"), "v2:x");
    write(c.path("bad.lua"), "return {message=function(p) if p=='error' then error('bad') end "
        "if p=='loop' then while true do end end return p end}");
    const auto bad = port();
    c.node("gameServer", bad, {"--script", c.path("bad.lua").string()});
    for (const auto& payload : {"error", "loop"}) must_fail([&] { (void)tcp(bad)->request(payload); });
    equal(tcp(bad)->request("healthy"), "healthy");
    // The actual exported Lua source is embedded at configure time, so installed
    // binaries can run this regression without depending on a source checkout.
    write(c.path("heap.lua"), "local H=assert(load([==["+std::string(heap_lua)+"]==]))()\n"
        "assert(MinHeap==nil); local h=H:new(3,function(a,b) return a<b end)\n"
        "h:push(3);h:push(1);h:push(2);assert(h:pop()==1 and h:pop()==2 and h:pop()==3)\n"
        "return {message=function(p)return p end}");
    const auto heap = port(); c.node("gameServer", heap, {"--script", c.path("heap.lua").string()});
    equal(tcp(heap)->request("heap"), "heap");
    gameServer.signal(SIGTERM); require(gameServer.wait() == 0, "gameServer graceful shutdown failed");
}
void lua_http() {
    Context c; const auto script = c.path("http.lua"); write(script, http_lua);
    const auto listen = port();
    auto& server = c.node("apiServer", listen, {"--transport", "http", "--script", script.string()});
    const auto origin = "http://"+endpoint(listen);
    auto response = http(origin+"/echo?x=1", "POST", std::string("\0\xff", 2), {}, true);
    require(response.status == 201 && response.headers.find("x-hook: after") != std::string::npos, "HTTP metadata/after hook");
    equal(response.body, std::string("\0\xff", 2));
    for (const auto& [path, status, body] : std::vector<std::tuple<std::string, long, std::string>>{
        {"/private",401,"denied"}, {"/replace",202,"replaced"}, {"/error",502,"handled"},
        {"/badheader",502,"handled"}, {"/fatal",500,"Internal Server Error"}, {"/missing",404,"missing"}}) {
        response = http(origin+path); require(response.status == status, "HTTP hook status"); equal(response.body, body);
    }
    write(script, "return {http=function(r)return{body='bad'}end,start=function()error('start rejected')end}");
    server.signal(SIGHUP); server.ready("reload rejected"); equal(http(origin+"/missing").body, "missing");
    write(script, "return {http=function(r)return{body='v2'}end,"
        "init=function()print('HOOK init v2')end,start=function()print('HOOK start v2')end,"
        "stop=function()print('HOOK stop v2')end,shutdown=function()print('HOOK shutdown v2')end}");
    server.signal(SIGHUP); server.ready("script reloaded"); equal(http(origin+"/").body, "v2");
    server.signal(SIGTERM); require(server.wait() == 0, "HTTP shutdown failed");
    std::istringstream log(server.output()); std::string line; std::vector<std::string> hooks;
    while (std::getline(log, line)) if (line.starts_with("HOOK ")) hooks.push_back(line);
    require(hooks == std::vector<std::string>{"HOOK init v1","HOOK start v1","HOOK init v2","HOOK start v2",
        "HOOK stop v1","HOOK shutdown v1","HOOK stop v2","HOOK shutdown v2"}, "Lua lifecycle order changed");
}
void cross_admission() {
    Context c; write(c.path("cross.lua"),
        "return {message=function(p,c)"
        " local who = c.character and c.character..':'..c.generation..':'"
        " ..c.route.realm..'/'..c.route.group..'/'..c.route.room or 'anon'"
        " return c.realm..'('..who..'):'..c.group..'/'..c.room..':'..p end}");
    const auto s0=port(), s1=port(), ingress=port(), ingress2=port();
    auto& shard = c.node("crossServer", s0, {"--script",c.path("cross.lua").string(),"--max-room-sessions","2"});
    c.node("crossServer", s1, {"--script",c.path("cross.lua").string(),"--max-room-sessions","2"});
    std::vector<std::string> args{"--upstream",endpoint(s0),"--upstream",endpoint(s1),"--max-room-sessions","2"};
    c.cross_gateway( ingress, args); c.cross_gateway( ingress2, args);
    std::array<std::string, 2> rooms;
    for (unsigned i=0; rooms[0].empty() || rooms[1].empty(); ++i) {
        CrossContext route{"a","g","r"+std::to_string(i),1};
        rooms[cross_shard(route,2)] = route.room;
    }
    auto join = [](cli::Client& peer, const std::string& realm, const std::string& room) {
        return peer.request("MLC1 1 "+realm+" g "+room);
    };
    {
        auto a=tcp(ingress), b=tcp(ingress);
        equal(join(*a,"a",rooms[0]),"MLC1 OK 1"); equal(join(*b,"b",rooms[0]),"MLC1 OK 1");
        equal(a->request("x"),"a(anon):g/"+rooms[0]+":x"); equal(b->request("x"),"b(anon):g/"+rooms[0]+":x");
        for (auto entry : {ingress,ingress2}) {
            auto third=tcp(entry); equal(join(*third,"c",rooms[0]),"MLC1 ERR ROOM_FULL");
        }
        auto other=tcp(ingress); equal(join(*other,"c",rooms[1]),"MLC1 OK 1");
    }
    eventually([&] { auto released=tcp(ingress); return join(*released,"d",rooms[0])=="MLC1 OK 1"; });
    {
        // A full participant declaration reaches Lua; every partial one is refused.
        auto hero=tcp(ingress);
        equal(hero->request("MLC1 1 a g "+rooms[0]+" ch hero-3 gen 7 ret home arena r1"), "MLC1 OK 1");
        equal(hero->request("x"), "a(hero-3:7:home/arena/r1):g/"+rooms[0]+":x");
        auto partial=tcp(ingress);
        equal(partial->request("MLC1 1 a g "+rooms[0]+" ch hero-3"), "MLC1 ERR INVALID");
    }
    equal(tcp(ingress)->request("MLC1 2 a g r"),"MLC1 ERR EPOCH");
    equal(tcp(ingress)->request("invalid"),"MLC1 ERR INVALID");
    shard.signal(SIGTERM); require(shard.wait()==0,"shard shutdown failed");
    must_fail([&] { auto failed=tcp(ingress); (void)join(*failed,"a",rooms[0]); });
    auto healthy=tcp(ingress); equal(join(*healthy,"a",rooms[1]),"MLC1 OK 1");
    equal(healthy->request("ok"),"a(anon):g/"+rooms[1]+":ok");
}
// Character route records: a route is only written against a registered
// member, GET answers from the live member state, and a retiring member drops
// every route that pointed at it.
void coordinator_routes() {
    Context c;
    write(c.path("echo.lua"), correlate_lua);
    const auto registry = port(), metrics = port(), tokened = port(),
              first = port(), second = port();
    auto& coord = c.node("coordinator", registry, {"--metrics-listen", endpoint(metrics)});
    c.node("coordinator", tokened, {"--service-token", "s3cret"});
    auto& gs_a = c.node("gameServer", first, {"--script", c.path("echo.lua").string(),
        "--coordinator", endpoint(registry), "--scope", "alpha", "--service-name", "gs-a"});
    auto& gs_b = c.node("gameServer", second, {"--script", c.path("echo.lua").string(),
        "--coordinator", endpoint(registry), "--scope", "alpha", "--service-name", "gs-b"});
    const auto send_frame = [](RawSocket& raw, std::string_view payload) {
        std::string frame(4, '\0');
        auto length = static_cast<std::uint32_t>(payload.size());
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(length & 255); length >>= 8; }
        raw.send(frame+std::string(payload));
    };
    const auto read_frame = [](RawSocket& raw) {
        const auto header = raw.receive(4);
        std::size_t length = 0;
        for (unsigned char ch : header) length = (length << 8) | ch;
        return raw.receive(length);
    };
    const auto expect_close = [](RawSocket& raw) {
        for (int i = 0; i < 40 && !raw.closed(); ++i) std::this_thread::sleep_for(50ms);
        require(raw.closed(), "connection stayed open after a refusal");
    };
    std::string body;
    // One-shot legs: one frame in, one verdict out, then the coordinator closes.
    const auto ask = [&](const std::string& line) {
        RawSocket raw(registry);
        send_frame(raw, line);
        body = read_frame(raw);
        expect_close(raw);
        return body;
    };
    eventually([&] { return ask("MLR1 QUERY").find("MLR1 MEMBERS 2") != std::string::npos; },
        5s, "members never reached the registry");
    // GET before any SET is an honest empty, not an error.
    equal(ask("MLR1 ROUTE GET mist eve"), "MLR1 ROUTE EMPTY");
    // Malformed shapes and unknown verbs report the right word.
    for (const auto& [line, word] : std::vector<std::pair<std::string, std::string>>{
            {"MLR1 ROUTE SET alpha mist eve gameServer", "MLR1 ERR INVALID"},
            {"MLR1 ROUTE SET alpha mist eve gameServer gs-a extra", "MLR1 ERR INVALID"},
            {"MLR1 ROUTE SET alpha mi!t eve gameServer gs-a", "MLR1 ERR INVALID"},
            {"MLR1 ROUTE SET alpha mist eve data gs-a", "MLR1 ERR INVALID"},
            {"MLR1 ROUTE GET mist", "MLR1 ERR INVALID"},
            {"MLR1 ROUTE GET mist ev!", "MLR1 ERR INVALID"},
            {"MLR1 ROUTE LIST", "MLR1 ERR UNEXPECTED"},
            {"MLR1 ROUTE", "MLR1 ERR UNEXPECTED"}}) {
        RawSocket raw(registry);
        send_frame(raw, line);
        equal(read_frame(raw), word);
        expect_close(raw);
    }
    // A route needs a live host: an unregistered instance is refused by name.
    equal(ask("MLR1 ROUTE SET alpha mist eve gameServer gs-z"), "MLR1 ERR UNKNOWN");
    equal(ask("MLR1 ROUTE SET alpha mist eve gameServer gs-a"), "MLR1 ROUTE OK");
    auto live = http("http://"+endpoint(metrics)+"/metrics").body;
    require(live.find("mistlane_registry_routes{role=\"coordinator\"} 1") != std::string::npos,
            "route gauge did not follow the record count");
    const auto eve_at = "MLR1 ROUTE AT alpha gameServer gs-a 127.0.0.1:"+
        std::to_string(first)+" 0 ready 0/42";
    eventually([&] { return ask("MLR1 ROUTE GET mist eve") == eve_at; },
        5s, "route did not follow the live member state");
    // A second SET for the same character overwrites; the count stays at one.
    equal(ask("MLR1 ROUTE SET alpha mist eve gameServer gs-b"), "MLR1 ROUTE OK");
    const auto eve_moved = "MLR1 ROUTE AT alpha gameServer gs-b 127.0.0.1:"+
        std::to_string(second)+" 0 ready 0/42";
    eventually([&] { return ask("MLR1 ROUTE GET mist eve") == eve_moved; },
        5s, "route overwrite did not stick");
    // A registered member publishes routes on its own control leg and the leg
    // keeps heartbeating; GET mirrors the heartbeat state it announces.
    {
        RawSocket host(registry);
        send_frame(host, "MLR1 REGISTER gameServer gs-r alpha 127.0.0.1:1 0");
        equal(read_frame(host), "MLR1 OK");
        send_frame(host, "MLR1 ROUTE SET alpha mist rogue gameServer gs-r");
        equal(read_frame(host), "MLR1 ROUTE OK");
        send_frame(host, "MLR1 notready 0 42");
        equal(read_frame(host), "MLR1 OK");
        equal(ask("MLR1 ROUTE GET mist rogue"),
              "MLR1 ROUTE AT alpha gameServer gs-r 127.0.0.1:1 0 notready 0/42");
        send_frame(host, "MLR1 ready 1 42");
        equal(read_frame(host), "MLR1 OK");
        equal(ask("MLR1 ROUTE GET mist rogue"),
              "MLR1 ROUTE AT alpha gameServer gs-r 127.0.0.1:1 0 ready 1/42");
        live = http("http://"+endpoint(metrics)+"/metrics").body;
        require(live.find("mistlane_registry_routes{role=\"coordinator\"} 2") != std::string::npos,
                "route gauge missed the second record");
    } // the host retires here
    eventually([&] { return ask("MLR1 ROUTE GET mist rogue") == "MLR1 ROUTE EMPTY"; },
        5s, "a retired host kept its route");
    live = http("http://"+endpoint(metrics)+"/metrics").body;
    require(live.find("mistlane_registry_routes{role=\"coordinator\"} 1") != std::string::npos,
            "route gauge did not follow the retirement");
    // Placement beside the route verbs stays member placement.
    eventually([&] { return ask("MLR1 PLACE gameServer alpha") ==
        "MLR1 PLACE gs-a 127.0.0.1:"+std::to_string(first)+" 0 ready 0/42"; },
        5s, "placement broke beside the route verbs");
    // Identity gates routes like everything else on a tokened coordinator.
    RawSocket anonymous(tokened);
    send_frame(anonymous, "MLR1 ROUTE GET mist eve");
    equal(read_frame(anonymous), "MLS1 ERR TOKEN");
    expect_close(anonymous);
    c.node("gameServer", port(), {"--script", c.path("echo.lua").string(),
        "--coordinator", endpoint(tokened), "--upstream-token", "s3cret", "--service-name", "gs-d"});
    const auto identified = [&](const std::string& line) {
        RawSocket raw(tokened);
        send_frame(raw, "MLS1 registryClient routes-1 s3cret");
        equal(read_frame(raw), "MLS1 OK");
        send_frame(raw, line);
        body = read_frame(raw);
        expect_close(raw);
        return body;
    };
    eventually([&] { return identified("MLR1 ROUTE SET main mist kate gameServer gs-d") ==
        "MLR1 ROUTE OK"; }, 5s, "an identified route publish never landed");
    body = identified("MLR1 ROUTE GET mist kate");
    require(body.starts_with("MLR1 ROUTE AT main gameServer gs-d "), "route answer wrong shape: "+body);
    require(gs_a.output().find("callback failed") == std::string::npos &&
            gs_b.output().find("callback failed") == std::string::npos,
            "registry member logged Lua failures");
    require(coord.output().find("unexpected frame") == std::string::npos, "coordinator log noise");
}
void cross_record() {
    Context c;
    write(c.path("echo.lua"), echo);
    const auto data = port(), metrics = port(), second = port(), tokened = port(), plain = port();
    c.node("data", data, {"--script", c.path("echo.lua").string(), "--metrics-listen", endpoint(metrics)});
    const auto send_frame = [](RawSocket& raw, std::string_view payload) {
        std::string frame(4, '\0');
        auto length = static_cast<std::uint32_t>(payload.size());
        for (int i = 3; i >= 0; --i) { frame[i] = static_cast<char>(length & 255); length >>= 8; }
        raw.send(frame + std::string(payload));
    };
    const auto read_frame = [](RawSocket& raw) {
        std::size_t length = 0;
        for (unsigned char ch : raw.receive(4)) length = (length << 8) | ch;
        return raw.receive(length);
    };
    const auto ask = [&send_frame, &read_frame](RawSocket& raw, std::string_view payload) {
        send_frame(raw, payload);
        return read_frame(raw);
    };
    // One leg loads and submits many times over; the verdicts re-arm it.
    RawSocket leg(data);
    equal(ask(leg, "MLD1 LOAD home hero"), "MLD1 EMPTY");
    equal(ask(leg, "MLD1 SUBMIT home hero gen 5 seq 1 hp=10 mp=5"), "MLD1 RECEIVED gen 5 seq 1");
    equal(ask(leg, "MLD1 LOAD home hero"), "MLD1 STATE gen 5 seq 1 hp=10 mp=5");
    equal(ask(leg, "MLD1 SUBMIT home hero gen 5 seq 1 hp=99"), "MLD1 RECEIVED gen 5 seq 1");
    equal(ask(leg, "MLD1 LOAD home hero"), "MLD1 STATE gen 5 seq 1 hp=10 mp=5");
    equal(ask(leg, "MLD1 SUBMIT home hero gen 5 seq 3 gap"), "MLD1 ERR SEQ seq 2");
    equal(ask(leg, "MLD1 SUBMIT home hero gen 4 seq 2 late"), "MLD1 ERR STALE gen 5");
    equal(ask(leg, "MLD1 SUBMIT home hero gen 6 seq 2 taken"), "MLD1 RECEIVED gen 6 seq 2");
    equal(ask(leg, "MLD1 SUBMIT home hero gen 7 seq 9 jump"), "MLD1 ERR SEQ seq 3");
    equal(ask(leg, "MLD1 SUBMIT away hero gen 1 seq 1 hp=3"), "MLD1 RECEIVED gen 1 seq 1");
    equal(ask(leg, "MLD1 LOAD home hero"), "MLD1 STATE gen 6 seq 2 taken");
    equal(ask(leg, "MLD1 LOAD away hero"), "MLD1 STATE gen 1 seq 1 hp=3");
    // Inject sequence 2 before sequence 1. The sender queue holds the gap,
    // then emits both frames in order to the real framed-TCP endpoint.
    ReturnSendQueue ordered(3);
    ordered.seed("order", "hero", 0);
    ordered.enqueue({"order", "hero", 2, 2, "second"});
    require(!ordered.next("order", "hero"), "sender emitted across a sequence gap");
    ordered.enqueue({"order", "hero", 2, 1, "first"});
    auto first = ordered.next("order", "hero");
    require(first && first->sequence == 1, "sender did not select missing sequence first");
    const auto submit = [](const ReturnSendQueue::Change& change) {
        return "MLD1 SUBMIT " + change.realm + " " + change.character + " gen "
            + std::to_string(change.generation) + " seq " + std::to_string(change.sequence)
            + " " + change.payload;
    };
    equal(ask(leg, submit(*first)), "MLD1 RECEIVED gen 2 seq 1");
    require(ordered.received("order", "hero", 1), "sender did not release first receipt");
    auto second_change = ordered.next("order", "hero");
    require(second_change && second_change->sequence == 2, "sender did not continue in order");
    equal(ask(leg, submit(*second_change)), "MLD1 RECEIVED gen 2 seq 2");
    require(ordered.received("order", "hero", 2), "sender did not release second receipt");
    require(!ordered.ack_durable("order", "hero", 2), "queue accepted an out-of-order durable ACK");
    require(ordered.ack_durable("order", "hero", 1), "queue rejected first durable ACK");
    require(ordered.ack_durable("order", "hero", 2), "queue rejected second durable ACK");
    equal(ask(leg, "MLD1 LOAD order hero"), "MLD1 STATE gen 2 seq 2 second");
    // A record leg also carries plain script frames under the same session.
    equal(ask(leg, "ping"), "ping");
    equal(ask(leg, "MLD1 NOPE"), "MLD1 ERR INVALID");
    // Every verdict lands on the closed result label set, and nothing else.
    const auto results = [&](const char* sample) {
        const auto body = http("http://"+endpoint(metrics)+"/metrics").body;
        const auto needle = std::string("mistlane_record_results_total{role=\"data\",result=\"")
            + sample + "\"} ";
        const auto at = body.find(needle);
        require(at != std::string::npos, std::string("missing metric ")+sample);
        return std::strtod(body.c_str()+at+needle.size(), nullptr);
    };
    require(results("accepted") == 5, "accepted counter mismatch");
    require(results("loaded") == 5, "loaded counter mismatch");
    require(results("duplicate") == 1, "duplicate counter mismatch");
    require(results("seq") == 2, "seq counter mismatch");
    require(results("stale") == 1, "stale counter mismatch");
    require(results("invalid") == 1, "invalid counter mismatch");
    require(results("empty") == 1, "empty counter mismatch");
    // Records are process-local until a persistence chain exists: a fresh
    // data process answers EMPTY, never a copied state.
    c.node("data", second, {"--script", c.path("echo.lua").string()});
    {
        RawSocket fresh(second);
        equal(ask(fresh, "MLD1 LOAD home hero"), "MLD1 EMPTY");
    }
    // A tokened data node gates the record leg behind MLS1; a refused
    // handshake closes the leg, so the identified path takes a new one.
    c.node("data", tokened, {"--script", c.path("echo.lua").string(), "--service-token", "s3cret"});
    {
        RawSocket raw(tokened);
        equal(ask(raw, "MLD1 SUBMIT home hero gen 1 seq 1 x"), "MLS1 ERR TOKEN");
    }
    {
        RawSocket raw(tokened);
        equal(ask(raw, "MLS1 gameServer gs-1 s3cret"), "MLS1 OK");
        equal(ask(raw, "MLD1 SUBMIT home hero gen 1 seq 1 x"), "MLD1 RECEIVED gen 1 seq 1");
    }
    // Other roles keep MLD1 as script traffic; only data owns the endpoint.
    c.node("gameServer", plain, {"--script", c.path("echo.lua").string()});
    {
        RawSocket game(plain);
        equal(ask(game, "MLD1 LOAD home hero"), "MLD1 LOAD home hero");
    }
}
namespace {
struct MigrationFixture {
    MigrationKey key{"home", "hero"};
    MigrationOwner source{{MigrationScope::Kind::Realm, "home", ""}, "gameServer", "gs-a", "town"};
    MigrationOwner target{{MigrationScope::Kind::CrossGroup, "", "arena"}, "crossServer", "xs-a", "room-1"};
    HomeGenerationAuthority home;
    MigrationFixture() { require(home.seed(key, 8), "home authority seed failed"); }
    MigrationTransaction transaction() { return MigrationTransaction(home, key, {source, 8}); }
    MigrationRequest request(std::string id = "move-1") { return {key, std::move(id), {source, 8}, target}; }
};
void migration_step(MigrationTransaction& tx, const MigrationToken& token, MigrationEvent event) {
    require(tx.apply(token, event) == MigrationResult::Applied, "migration step failed: " + std::to_string(static_cast<int>(event)));
}
void migration_ready(MigrationTransaction& tx, const MigrationToken& token) {
    for (auto event : {MigrationEvent::SourceDrained, MigrationEvent::HomeFence, MigrationEvent::DestinationReady})
        migration_step(tx, token, event);
}
}
void coordinator_migration_commit() {
    MigrationFixture f;
    auto tx = f.transaction();
    require(tx.state() == MigrationState::Idle && tx.begin_request(), "idle source not active");
    const auto request = f.request();
    require(tx.prepare(request) == MigrationResult::Applied, "prepare failed");
    const auto token = tx.entry(request.id)->token;
    require(token.generation == 9 && !tx.timers_running(), "home generation or source timer gate failed");
    require(!tx.write_allowed(f.source, 8) && !tx.write_allowed(f.target, 9), "prepare admitted a writer");
    require(tx.apply(token, MigrationEvent::SourceDrained) == MigrationResult::Order && tx.finish_request(),
        "source drain ignored in-flight handler");
    migration_ready(tx, token);
    require(tx.route() == request.expected && tx.write_allowed(f.target, 9), "target ACK prematurely published route");
    migration_step(tx, token, MigrationEvent::Commit);
    require(tx.state() == MigrationState::Committed && tx.route() == MigrationRoute{f.target, 9}
            && !tx.write_allowed(f.source, 8) && tx.timers_running(), "commit did not publish sole target owner");
    require(tx.apply(token, MigrationEvent::Commit) == MigrationResult::Duplicate
            && tx.prepare(request) == MigrationResult::Duplicate && f.home.issued(f.key) == 9, "commit/prepare replay changed generation");
    // Explicit scope/role/scene checks, and identical character ids in two realms.
    auto bad = MigrationFixture{}.request();
    bad.id = "bad-scope"; bad.destination.scope.realm = "home";
    require(tx.prepare(bad) == MigrationResult::Invalid, "mixed realm/group scope accepted");
    bad.destination = f.source; bad.destination.role = "crossServer";
    require(tx.prepare(bad) == MigrationResult::Invalid, "role incompatible with scope accepted");
    bad.destination = f.target; bad.destination.scene.clear();
    require(tx.prepare(bad) == MigrationResult::Invalid, "missing scene ownership accepted");
    const MigrationKey other_key{"other", "hero"};
    require(f.home.seed(other_key, 1), "realm-isolated seed failed");
    MigrationTransaction other(f.home, other_key, {f.source, 1});
    require(other.prepare({other_key, "move-1", {f.source, 1}, f.target}) == MigrationResult::Applied
            && other.entry("move-1")->token.generation == 2 && f.home.issued(f.key) == 9, "realm generation keys collided");
    std::cout << "migration.commit: drained -> fenced -> destination ACK -> route gen=9; scoped ownership [PASS]\n";
}
void coordinator_migration_abort() {
    for (bool drained : {false, true}) {
        MigrationFixture f;
        auto tx = f.transaction();
        require(tx.begin_request(), "abort fixture handler missing");
        require(tx.prepare(f.request()) == MigrationResult::Applied, "abort prepare failed");
        const auto token = tx.entry("move-1")->token;
        if (drained) {
            require(tx.finish_request(), "drain failed");
            migration_step(tx, token, MigrationEvent::SourceDrained);
        }
        migration_step(tx, token, MigrationEvent::Abort);
        require(tx.state() == MigrationState::Aborting && !tx.write_allowed(f.source, 8), "abort resumed before cleanup");
        require(tx.apply(token, MigrationEvent::HomeFence) == MigrationResult::Order, "abort raced into fence");
        if (!drained) {
            require(tx.apply(token, MigrationEvent::Cleanup) == MigrationResult::Pending && tx.finish_request(),
                "abort cleanup ignored executing handler");
        }
        migration_step(tx, token, MigrationEvent::Cleanup);
        require(tx.state() == MigrationState::Aborted && tx.route() == f.request().expected
                && tx.write_allowed(f.source, 8) && !tx.write_allowed(f.target, 9)
                && tx.timers_running() && !tx.entry("move-1")->destination_staged, "abort left staged target or frozen source");
        require(tx.apply(token, MigrationEvent::Abort) == MigrationResult::Duplicate, "abort replay failed");
        require(tx.prepare(f.request("move-2")) == MigrationResult::Applied
                && tx.entry("move-2")->token.generation == 10, "aborted generation was reused");
    }
    std::cout << "migration.abort: half-built target discarded, source gen=8 restored, next gen=10 [PASS]\n";
}
void coordinator_migration_recover() {
    // Crash cuts: prepare, source drain, fence, target ACK, committed,
    // abort requested, aborted. Restore trusted protocol/home checkpoints;
    // no test claims that the current data endpoint persists these facts.
    for (unsigned cut = 0; cut < 7; ++cut) {
        MigrationFixture f;
        std::optional<MigrationTransaction::Snapshot> checkpoint;
        MigrationToken token;
        {
            auto crashed = f.transaction();
            require(crashed.prepare(f.request()) == MigrationResult::Applied, "recover prepare failed");
            token = crashed.entry("move-1")->token;
            if (cut >= 1 && cut <= 4) migration_step(crashed, token, MigrationEvent::SourceDrained);
            if (cut >= 2 && cut <= 4) migration_step(crashed, token, MigrationEvent::HomeFence);
            if (cut >= 3 && cut <= 4) migration_step(crashed, token, MigrationEvent::DestinationReady);
            if (cut == 4) migration_step(crashed, token, MigrationEvent::Commit);
            if (cut >= 5) migration_step(crashed, token, MigrationEvent::Abort);
            if (cut == 6) migration_step(crashed, token, MigrationEvent::Cleanup);
            checkpoint = crashed.snapshot();
        }
        auto restored_home = f.home;
        MigrationTransaction restored(restored_home, *checkpoint);
        auto result = restored.apply(token, MigrationEvent::Recover);
        if (cut == 2) {
            require(result == MigrationResult::Pending && restored.state() == MigrationState::Recovering
                    && !restored.write_allowed(f.source, 8) && !restored.write_allowed(f.target, 9), "recovery invented destination ACK");
            require(restored.apply(token, MigrationEvent::Abort) == MigrationResult::Order, "post-fence recovery revived source");
            // A second crash while recovering remains pending until the target ACK.
            MigrationTransaction again(restored_home, restored.snapshot());
            require(again.apply(token, MigrationEvent::Recover) == MigrationResult::Pending, "recover replay changed decision");
            migration_step(again, token, MigrationEvent::DestinationReady);
            migration_step(again, token, MigrationEvent::Recover);
            require(again.state() == MigrationState::Committed && again.route() == MigrationRoute{f.target, 9}, "forward recovery did not commit");
        } else {
            require(result == MigrationResult::Applied || result == MigrationResult::Duplicate, "recovery failed");
            const bool forward = cut == 3 || cut == 4;
            require(restored.state() == (forward ? MigrationState::Committed : MigrationState::Aborted)
                    && restored.route() == (forward ? MigrationRoute{f.target, 9} : f.request().expected), "wrong crash terminal state");
            require(restored.apply(token, MigrationEvent::Recover) == MigrationResult::Duplicate, "terminal recovery not idempotent");
        }
        require(restored_home.issued(f.key) == 9, "recovery lost reserved generation");
    }
    std::cout << "migration.recover: 7 checkpoint cuts -> aborted/committed; fenced recovery waits for ACK [PASS]\n";
}
void coordinator_migration_stale() {
    MigrationFixture f;
    auto tx = f.transaction();
    require(tx.prepare(f.request()) == MigrationResult::Applied, "stale prepare failed");
    const auto first = tx.entry("move-1")->token;
    auto stale = first; stale.generation = 8;
    for (auto event : {MigrationEvent::HomeFence, MigrationEvent::DestinationReady, MigrationEvent::Commit,
                       MigrationEvent::Abort, MigrationEvent::Cleanup, MigrationEvent::Recover})
        require(tx.apply(stale, event) == MigrationResult::Stale, "late old-generation control event accepted");
    migration_ready(tx, first);
    migration_step(tx, first, MigrationEvent::Commit);
    require(!tx.write_allowed(f.source, 8) && !tx.write_allowed(f.target, 10), "old or unissued writer accepted");
    require(tx.prepare(f.request("stale-owner")) == MigrationResult::Stale, "stale expected owner/route accepted");
    require(tx.prepare({f.key, "move-2", tx.route(), f.source}) == MigrationResult::Applied, "return prepare failed");
    const auto second = tx.entry("move-2")->token;
    migration_ready(tx, second);
    migration_step(tx, second, MigrationEvent::Commit);
    require(tx.apply(first, MigrationEvent::Commit) == MigrationResult::Duplicate
            && tx.route() == MigrationRoute{f.source, 10} && !tx.write_allowed(f.target, 9), "late commit republished obsolete route");
    auto wrong_key = second; wrong_key.key.first = "other";
    require(tx.apply(wrong_key, MigrationEvent::Recover) == MigrationResult::Invalid, "cross-realm event accepted");
    std::cout << "migration.stale: old generation/control/owner rejected; old commit cannot republish [PASS]\n";
}
void coordinator_migration_order() {
    MigrationFixture f;
    auto tx = f.transaction();
    require(tx.prepare(f.request()) == MigrationResult::Applied, "order prepare failed");
    const auto token = tx.entry("move-1")->token;
    require(tx.prepare(f.request("concurrent")) == MigrationResult::Conflict, "concurrent migration accepted");
    auto changed = f.request(); changed.destination.instance = "xs-b";
    require(tx.prepare(changed) == MigrationResult::Conflict, "request id reused with another target");
    require(tx.apply(token, MigrationEvent::HomeFence) == MigrationResult::Order
            && tx.apply(token, MigrationEvent::DestinationReady) == MigrationResult::Order
            && tx.apply(token, MigrationEvent::Commit) == MigrationResult::Order, "pre-drain publication/grant accepted");
    migration_step(tx, token, MigrationEvent::SourceDrained);
    require(tx.apply(token, MigrationEvent::Commit) == MigrationResult::Order, "route published before home fence");
    migration_step(tx, token, MigrationEvent::HomeFence);
    require(tx.apply(token, MigrationEvent::Commit) == MigrationResult::Order
            && tx.apply(token, MigrationEvent::Abort) == MigrationResult::Order
            && !tx.write_allowed(f.source, 8), "fenced source revived or route published before target ACK");
    migration_step(tx, token, MigrationEvent::DestinationReady);
    require(tx.route() == f.request().expected, "route changed before commit");
    // Simulate a competing directory edit in a trusted checkpoint: CAS fails.
    auto changed_route = tx.snapshot(); changed_route.route.owner.instance = "gs-other";
    MigrationTransaction competing(f.home, changed_route);
    require(competing.apply(token, MigrationEvent::Commit) == MigrationResult::Order, "expected-route CAS bypassed");
    migration_step(tx, token, MigrationEvent::Commit);
    require(tx.route() == MigrationRoute{f.target, 9}, "ordered commit did not publish");
    std::cout << "migration.order: premature fence/activation/publication and route CAS conflict rejected [PASS]\n";
}
void coordinator_migration_retire() {
    // A member that leaves the registry cannot finish the transaction it was
    // moving a participant to or from, so the transaction has to end instead
    // of blocking every later prepare forever. The source's handler is dead
    // with the process, so it is force-drained here; a destination retirement
    // leaves a live source, whose handler still owns admission and is let
    // finish first, exactly like any other abort.
    {
        MigrationFixture f;
        auto tx = f.transaction();
        require(tx.begin_request(), "retire fixture handler missing");
        require(tx.prepare(f.request()) == MigrationResult::Applied, "retire prepare failed");
        const auto token = tx.entry("move-1")->token;
        require(tx.retire_member(token, f.source) == MigrationResult::Applied
                && tx.state() == MigrationState::Aborted,
                "a retired source did not drain its own handler and abort");
        auto owner = tx.lookup_ownership(f.key);
        require(tx.route() == f.request().expected && tx.write_allowed(f.source, 8)
                && !tx.write_allowed(f.target, 9) && tx.timers_running()
                && !tx.entry("move-1")->destination_staged
                && owner && *owner == HomeOwnership{f.source, 8, HomeOwnershipState::Active}
                && f.home.issued(f.key) == 9,
                "source retirement left the staged target or froze the source");

        // Takeover: the aborted generation is consumed and never reused, but
        // the route still names the participant, so a strictly newer
        // generation may move it onto a living member.
        MigrationOwner alive = f.target; alive.instance = "xs-b";
        MigrationRequest takeover{f.key, "takeover", f.request().expected, alive};
        require(tx.prepare(takeover) == MigrationResult::Applied
                && tx.entry("takeover")->token.generation == 10,
                "takeover did not reserve a newer generation");
        const auto next = tx.entry("takeover")->token;
        migration_ready(tx, next);
        migration_step(tx, next, MigrationEvent::Commit);
        require(tx.state() == MigrationState::Committed && tx.route() == MigrationRoute{alive, 10}
                && tx.write_allowed(alive, 10) && !tx.write_allowed(f.source, 8)
                && !tx.write_allowed(f.target, 9), "takeover did not publish the living destination");
        MigrationTransaction checkpoint(f.home, tx.snapshot());
        require(checkpoint.state() == MigrationState::Committed && checkpoint.route() == MigrationRoute{alive, 10}
                && checkpoint.apply(token, MigrationEvent::Recover) == MigrationResult::Duplicate
                && f.home.issued(f.key) == 10, "retirement checkpoint did not restore its journal");
    }
    {
        MigrationFixture f;
        auto tx = f.transaction();
        require(tx.begin_request(), "destination-retire fixture handler missing");
        require(tx.prepare(f.request()) == MigrationResult::Applied, "destination-retire prepare failed");
        const auto token = tx.entry("move-1")->token;
        require(tx.retire_member(token, f.target) == MigrationResult::Pending
                && tx.state() == MigrationState::Preparing && !tx.timers_running()
                && !tx.write_allowed(f.source, 8),
                "a retired destination advanced the transaction or resumed the source's writes");
        require(tx.finish_request(), "the live source handler did not finish");
        require(tx.retire_member(token, f.target) == MigrationResult::Applied
                && tx.state() == MigrationState::Aborted && tx.write_allowed(f.source, 8)
                && tx.timers_running() && !tx.entry("move-1")->destination_staged,
                "destination retirement did not abort with the source restored");
        require(tx.apply(token, MigrationEvent::Abort) == MigrationResult::Duplicate,
                "abort replay after retirement changed the outcome");
    }
    {
        MigrationFixture f;
        auto tx = f.transaction();
        require(tx.prepare(f.request()) == MigrationResult::Applied, "fenced-retire prepare failed");
        const auto token = tx.entry("move-1")->token;
        migration_step(tx, token, MigrationEvent::SourceDrained);
        migration_step(tx, token, MigrationEvent::HomeFence);
        auto fenced = tx.lookup_ownership(f.key);
        require(fenced && fenced->state == HomeOwnershipState::Fenced && !tx.write_allowed(f.source, 8),
                "fence fixture did not fence the source");
        require(tx.retire_member(token, f.target) == MigrationResult::Pending
                && tx.retire_member(token, f.source) == MigrationResult::Pending
                && tx.state() == MigrationState::Preparing && !tx.write_allowed(f.source, 8)
                && !tx.timers_running(),
                "post-fence retirement mutated the fenced transaction");
        auto still = tx.lookup_ownership(f.key);
        require(still && still->state == HomeOwnershipState::Fenced
                && tx.apply(token, MigrationEvent::Recover) == MigrationResult::Pending,
                "fence retirement invented an owner or a destination acknowledgement");
    }
    {
        MigrationFixture f;
        auto tx = f.transaction();
        require(tx.prepare(f.request()) == MigrationResult::Applied, "refusal prepare failed");
        const auto token = tx.entry("move-1")->token;
        MigrationOwner stranger = f.target; stranger.instance = "xs-b";
        require(tx.retire_member(token, stranger) == MigrationResult::Invalid,
                "retirement of a member the transaction does not depend on was accepted");
        MigrationOwner malformed = f.source; malformed.scene.clear();
        require(tx.retire_member(token, malformed) == MigrationResult::Invalid,
                "an unidentifiable member claim was accepted");
        MigrationToken old_generation = token; old_generation.generation = 8;
        require(tx.retire_member(old_generation, f.source) == MigrationResult::Stale,
                "a stale generation claimed a retirement");
        MigrationToken wrong_key = token; wrong_key.key.first = "other";
        require(tx.retire_member(wrong_key, f.source) == MigrationResult::Invalid,
                "a cross-realm retirement claim was accepted");
        migration_step(tx, token, MigrationEvent::Abort);
        migration_step(tx, token, MigrationEvent::Cleanup);
        require(tx.retire_member(token, f.source) == MigrationResult::Conflict
                && tx.state() == MigrationState::Aborted, "retirement changed a terminal transaction");
    }
    std::cout << "migration.retire: source/destination retirement aborts pre-fence, fenced stays pending, "
                 "takeover at gen 10 [PASS]\n";
}
void cross_return_queue() {
    const DurableAck ack{"realm-a", "same-id", 3, 5};
    const auto wire = encode_durable_ack(ack);
    equal(wire, "MLD1 ACK durable realm-a same-id gen 3 seq 5");
    const auto decoded = parse_durable_ack(wire);
    require(decoded.realm == ack.realm && decoded.character == ack.character && decoded.generation == 3 && decoded.sequence == 5,
        "durable ack roundtrip changed fence/key/cursor");
    for (const auto& bad : {"MLD1 ACK realm-a same-id gen 3 seq 5", "MLD1 ACK durable realm-a same-id gen 0 seq 5",
                            "MLD1 ACK durable realm-a same-id gen 3 seq 0", "MLD1 ACK durable realm-a same-id gen 3 seq 5 extra"})
        must_fail([&] { (void)parse_durable_ack(bad); });
    WriterLease source, destination;
    require(source.start(8), "lease initial grant failed");
    require(source.write_allowed(8) && !source.write_allowed(9), "active lease fence failed");
    require(source.begin_request(), "active in-flight request not admitted");
    require(source.request(9) && !source.timer_running(), "transfer did not stop timers/drain in-flight work");
    require(!source.request(10), "duplicate handoff request accepted");
    require(!source.relinquish() && source.finish_request(), "handoff did not wait for in-flight request completion");
    require(source.relinquish() && !source.write_allowed(8), "source was not fenced at tombstone");
    require(!destination.accept(9), "destination accepted before transfer grant");
    require(destination.start(1) && destination.request(9) && destination.relinquish() && destination.accept(9), "destination lease activation failed");
    require(!source.write_allowed(8) && destination.write_allowed(9), "late source write was not rejected after handoff");
    require(!destination.accept(9), "duplicate lease grant accepted");
    {
        // A retired member cannot finish its own in-flight handler, so the
        // coordinator force-completes it instead of waiting forever.
        WriterLease retired;
        require(retired.start(8) && retired.begin_request() && retired.request(9), "retired lease fixture failed");
        require(retired.drain_requests(), "a retired member could not force-drain");
        require(retired.relinquish() && !retired.write_allowed(8), "a force-drained handoff was not fenced");
        require(!retired.drain_requests(), "drain ran after the tombstone");
    }
    ReturnSendQueue queue(2);
    queue.seed("realm-a", "same-id", 4);
    queue.seed("realm-b", "same-id", 0);
    queue.enqueue({"realm-a", "same-id", 3, 6, "six"});
    queue.enqueue({"realm-b", "same-id", 1, 1, "other realm"});
    require(!queue.next("realm-a", "same-id"), "gap was not held for retry/fill");
    queue.enqueue({"realm-a", "same-id", 3, 5, "five"});
    must_fail([&] { queue.enqueue({"realm-a", "same-id", 3, 5, "duplicate"}); });
    auto first = queue.next("realm-a", "same-id");
    require(first && first->sequence == 5, "sender did not release filled gap");
    require(!queue.next("realm-a", "same-id"), "same participant had concurrent sends");
    require(queue.retry("realm-a", "same-id"), "first retry was not allowed");
    first = queue.next("realm-a", "same-id");
    require(first && first->sequence == 5, "retry changed sequence order");
    require(!queue.retry("realm-a", "same-id"), "retry limit was not enforced");
    require(!queue.next("realm-a", "same-id"), "retry exhaustion resent an item");
    auto other = queue.next("realm-b", "same-id");
    require(other && other->sequence == 1, "realm key interfered with same character id");
    ReturnSendQueue durable_queue(2);
    durable_queue.seed("realm-a", "same-id", 4);
    durable_queue.enqueue({"realm-a", "same-id", 3, 5, "five"});
    durable_queue.owner_generation("realm-a", "same-id", 3);
    require(durable_queue.next("realm-a", "same-id").has_value(), "durable test send missing");
    require(durable_queue.received("realm-a", "same-id", 5), "volatile receipt did not release transport slot");
    require(!durable_queue.ack_durable(DurableAck{"realm-a", "same-id", 2, 5}), "stale generation durable ack drained queue");
    require(durable_queue.ack_durable(ack), "durable acknowledgement failed to drain matching change");
}
void probe_control() {
    Context c;
    write(c.path("echo.lua"), echo);
    // Capacity 2 with a 0.5 readiness gate saturates at one session; two ticks flip state.
    const auto work=port(), probe=port(), work_metrics=port(), guarded=port(), guard_probe=port();
    c.node("gameServer", work, {"--script", c.path("echo.lua").string(), "--max-connections", "2",
        "--overload-ratio", "0.5", "--ready-hysteresis", "2", "--probe-listen", endpoint(probe),
        "--metrics-listen", endpoint(work_metrics)});
    const auto ask = [](unsigned listen, const std::string& line) {
        RawSocket raw(listen);
        raw.send(line + "\n");
        return raw.receive_line();
    };
    const auto metric_value = [](const std::string& body, const std::string& sample) {
        const auto at = body.find(sample);
        require(at != std::string::npos, "missing metric " + sample);
        return std::strtod(body.c_str() + at + sample.size(), nullptr);
    };
    equal(ask(probe, "PROBE1 n1 liveness"), "PROBE1 n1 ok alive");
    equal(ask(probe, "PROBE9 n2 liveness"), "PROBE1 n2 err version");
    equal(ask(probe, "PROBE1 n3 status"), "PROBE1 n3 err command");
    equal(ask(probe, "PROBE1 n4"), "PROBE1 n4 err malformed");
    equal(ask(probe, "PROBE1 bad!n liveness"), "PROBE1 - err malformed");
    equal(ask(probe, "hello world"), "PROBE1 world err version");
    // Every ask is a fresh connection, so reply buffers and teardown must hold
    // up across repeated probe lifetimes.
    for (int i = 0; i < 50; ++i) {
        const auto nonce = "loop" + std::to_string(i);
        equal(ask(probe, "PROBE1 " + nonce + " liveness"), "PROBE1 " + nonce + " ok alive");
    }
    const auto readiness = ask(probe, "PROBE1 n5 readiness");
    require(readiness.starts_with("PROBE1 n5 ok ready active=0 capacity=2 "),
        "unexpected readiness reply: " + readiness);
    require(readiness.find(" lag_ms=") != std::string::npos, "readiness lag summary missing");
    // A probe connection that never asks must be closed by the server read deadline.
    {
        RawSocket silent(probe);
        eventually([&] { return silent.closed(); });
    }
    // Saturating flips to notready, releasing the session flips back.
    auto session = tcp(work);
    equal(session->request("busy"), "busy");
    eventually([&] {
        return ask(probe, "PROBE1 n6 readiness").find(" ok notready active=1 capacity=2 ") != std::string::npos;
    });
    // The readiness gauge must follow the admission gate, not just start/stop.
    {
        const auto busy = http("http://"+endpoint(work_metrics)+"/metrics");
        require(metric_value(busy.body, "mistlane_ready{role=\"gameServer\"} ") == 0,
            "readiness gauge must drop with the admission gate: " + busy.body);
    }
    session.reset();
    eventually([&] {
        return ask(probe, "PROBE1 n7 readiness").find(" ok ready active=0 capacity=2 ") != std::string::npos;
    });
    {
        const auto recovered = http("http://"+endpoint(work_metrics)+"/metrics");
        require(metric_value(recovered.body, "mistlane_ready{role=\"gameServer\"} ") == 1,
            "readiness gauge must recover with the admission gate: " + recovered.body);
    }
    {
        // Minimal identity check: a token env makes unauthenticated probes fail.
        ScopedEnv token("MISTLANE_PROBE_TOKEN", "s3cret");
        c.node("gameServer", guarded, {"--script", c.path("echo.lua").string(), "--max-connections", "5",
            "--probe-listen", endpoint(guard_probe)});
    }
    equal(ask(guard_probe, "PROBE1 n8 readiness"), "PROBE1 n8 err auth");
    equal(ask(guard_probe, "PROBE1 n9 wrong readiness"), "PROBE1 n9 err auth");
    const auto authenticated = ask(guard_probe, "PROBE1 n10 s3cret readiness");
    require(authenticated.starts_with("PROBE1 n10 ok ready active=0 capacity=5 "),
        "unexpected authenticated readiness: " + authenticated);

    // Cross consults shard readiness for new rooms; established rooms keep flowing.
    const auto shard=port(), shard_probe=port(), ingress=port(), ingress_metrics=port(),
        ingress2=port(), plain=port(), broken=port();
    c.node("crossServer", shard, {"--script", c.path("echo.lua").string(), "--max-connections", "4",
        "--overload-ratio", "0.5", "--ready-hysteresis", "2", "--probe-listen", endpoint(shard_probe)});
    const auto upstream = endpoint(shard) + ":" + std::to_string(shard_probe);
    c.cross_gateway( ingress, {"--upstream", upstream, "--metrics-listen", endpoint(ingress_metrics)});
    c.cross_gateway( ingress2, {"--upstream", upstream});
    const auto join = [](cli::Client& peer, const std::string& room) {
        return peer.request("MLC1 1 realm g " + room);
    };
    auto room1 = tcp(ingress), room2 = tcp(ingress2);
    equal(join(*room1, "r1"), "MLC1 OK 1");
    equal(join(*room2, "r2"), "MLC1 OK 1");
    eventually([&] {
        return ask(shard_probe, "PROBE1 nr readiness").find(" ok notready active=2 ") != std::string::npos;
    }, 5s, "shard to reach notready at active=2");
    equal(join(*tcp(ingress), "r3"), "MLC1 ERR NOT_READY");
    equal(join(*tcp(ingress2), "r4"), "MLC1 ERR NOT_READY");
    // Admission rejections include cross-gated joins, not only local limits.
    {
        const auto rejects = http("http://"+endpoint(ingress_metrics)+"/metrics");
        require(metric_value(rejects.body, "mistlane_admission_rejections_total{role=\"gateway\"} ") >= 1,
            "cross admission rejects are not counted: " + rejects.body);
    }
    room1.reset();
    eventually([&] {
        return ask(shard_probe, "PROBE1 nr2 readiness").find(" ok ready active=1 ") != std::string::npos;
    }, 5s, "shard to recover ready at active=1");
    equal(join(*tcp(ingress), "r5"), "MLC1 OK 1");
    // Without a probe port the cross node cannot gate, so it keeps forwarding.
    c.cross_gateway( plain, {"--upstream", endpoint(shard)});
    equal(join(*tcp(plain), "r7"), "MLC1 OK 1");
    // A dead probe listener is an upstream failure, not a silent accept.
    c.cross_gateway( broken, {"--upstream", endpoint(shard) + ":" + std::to_string(port())});
    equal(join(*tcp(broken), "r8"), "MLC1 ERR UPSTREAM");

    // Bracketed IPv6 listen and upstream addresses, gated and ungated.
    const auto v6_shard=port(), v6_probe=port(), v6_gated=port(), v6_plain=port();
    auto& v6_node = c.spawn({self().string(), "--role", "crossServer",
        "--listen", "[::1]:" + std::to_string(v6_shard),
        "--script", c.path("echo.lua").string(), "--max-connections", "4",
        "--overload-ratio", "0.5", "--ready-hysteresis", "2",
        "--probe-listen", "[::1]:" + std::to_string(v6_probe)});
    v6_node.ready("ready crossServer");
    c.cross_gateway( v6_gated, {"--upstream",
        "[::1]:" + std::to_string(v6_shard) + ":" + std::to_string(v6_probe)});
    equal(join(*tcp(v6_gated), "v1"), "MLC1 OK 1");
    c.cross_gateway( v6_plain, {"--upstream", "[::1]:" + std::to_string(v6_shard)});
    equal(join(*tcp(v6_plain), "v2"), "MLC1 OK 1");

    // An oversized probe request line is closed without any reply bytes.
    {
        RawSocket long_line(probe);
        long_line.send(std::string(300, 'A') + "\n");
        must_fail([&] { (void)long_line.receive_line(); });
    }
    // Concurrent probe connections are capped; rejected sockets close at once.
    {
        std::vector<std::unique_ptr<RawSocket>> held;
        for (int i = 0; i < 64; ++i) held.push_back(std::make_unique<RawSocket>(probe));
        RawSocket over(probe);
        eventually([&] { return over.closed_now(); }, 5s,
            "peer cap did not close the 65th probe connection");
        held.clear();
        eventually([&] {
            return ask(probe, "PROBE1 freed liveness") == "PROBE1 freed ok alive";
        }, 5s, "probe slots did not free after held peers closed");
    }
}
void config_file() {
    Context c;
    write(c.path("echo.lua"), echo);
    const auto config = c.path("gameServer.env");
    const auto listen = port();
    write(config, "# mistlane-config 1\nROLE=gameServer\nLISTEN=" + endpoint(listen) +
        "\nSCRIPT=echo.lua\nTRANSPORT=tcp\nMAX_CONNECTIONS=5\n");
    {
        auto& node = c.spawn({self().string(), "--config", config.string()});
        node.ready("ready gameServer " + endpoint(listen));
        require(node.output().find("session admission limit 5") != std::string::npos,
            "config max_connections missing: " + node.output());
        equal(tcp(listen)->request("hello"), "hello");
        node.signal(SIGTERM);
        require(node.wait() == 0, "config node should stop cleanly");
    }
    {
        // A version header is required so a random shell env file is not consumed silently.
        write(config, "ROLE=gameServer\nLISTEN=" + endpoint(listen) + "\n");
        auto& bad = c.spawn({self().string(), "--config", config.string()});
        require(bad.wait() == 1, "missing version line must fail");
        require(bad.output().find("mistlane-config 1") != std::string::npos,
            "version error missing: " + bad.output());
    }
    {
        write(config, "# mistlane-config 1\nNOPE=1\n");
        auto& bad = c.spawn({self().string(), "--config", config.string()});
        require(bad.wait() == 1, "unknown key must fail");
        require(bad.output().find("unknown key NOPE") != std::string::npos,
            "unknown key error missing: " + bad.output());
    }
    {
        write(config, "# mistlane-config 1\nMAX_CONNECTIONS=5\nMAX_CONNECTIONS=6\n");
        auto& bad = c.spawn({self().string(), "--config", config.string()});
        require(bad.wait() == 1, "duplicate key must fail");
        require(bad.output().find("duplicate key MAX_CONNECTIONS") != std::string::npos,
            "duplicate key error missing: " + bad.output());
    }
    {
        // Environment beats the file; an explicit flag beats the environment.
        write(config, "# mistlane-config 1\nROLE=gameServer\nLISTEN=" + endpoint(listen) +
            "\nSCRIPT=echo.lua\nTRANSPORT=tcp\nMAX_CONNECTIONS=5\n");
        ScopedEnv around("MISTLANE_MAX_CONNECTIONS", "7");
        auto& node = c.spawn({self().string(), "--config", config.string()});
        node.ready("ready gameServer " + endpoint(listen));
        require(node.output().find("session admission limit 7") != std::string::npos,
            "environment override missing: " + node.output());
        node.signal(SIGTERM);
        require(node.wait() == 0, "overridden node should stop cleanly");
    }
    {
        auto& node = c.spawn({self().string(), "--config", config.string(), "--max-connections", "3"});
        node.ready("ready gameServer " + endpoint(listen));
        require(node.output().find("session admission limit 3") != std::string::npos,
            "flag override missing: " + node.output());
        node.signal(SIGTERM);
        require(node.wait() == 0, "flag node should stop cleanly");
    }
    {
        // ROOT keeps scaffold-style project-root-relative entries readable.
        const auto root = c.path("app");
        fs::create_directory(root);
        write(root / "echo.lua", echo);
        const auto rooted_port = port();
        write(c.path("rooted.env"), "# mistlane-config 1\nROOT=app\nROLE=gameServer\nLISTEN=" +
            endpoint(rooted_port) + "\nSCRIPT=echo.lua\n");
        auto& node = c.spawn({self().string(), "--config", c.path("rooted.env").string()});
        node.ready("ready gameServer " + endpoint(rooted_port));
        equal(tcp(rooted_port)->request("ping"), "ping");
        node.signal(SIGTERM);
        require(node.wait() == 0, "rooted node should stop cleanly");
    }
    {
        const auto metrics = port(), probe = port();
        write(c.path("diag.env"), "# mistlane-config 1\nROLE=gameServer\nLISTEN=" + endpoint(port()) +
            "\nSCRIPT=echo.lua\nMETRICS_LISTEN=" + endpoint(metrics) + "\nPROBE_LISTEN=" + endpoint(probe) + "\n");
        auto& node = c.spawn({self().string(), "--config", c.path("diag.env").string()});
        node.ready("ready gameServer");
        { RawSocket raw(probe); raw.send("PROBE1 cf liveness\n"); equal(raw.receive_line(), "PROBE1 cf ok alive"); }
        const auto scrape = http("http://"+endpoint(metrics)+"/metrics");
        require(scrape.status == 200 && scrape.body.find("mistlane_messages_total") != std::string::npos,
            "config metrics listener missing");
        node.signal(SIGTERM);
        require(node.wait() == 0, "diagnostics node should stop cleanly");
    }
}
void contract_tool() {
    Context c;
    const std::string avatar = R"(<Contracts>
  <Types>
    <Item><itemId id="0" type="UINT32"/><count id="1" type="UINT32" default="1"/></Item>
    <Inventory><capacity id="0" type="UINT16" default="20"/><items id="1" type="ARRAY" of="Item"/></Inventory>
  </Types>
  <Entity name="Avatar" id="1">
    <Properties>
      <level id="1" type="UINT16" default="1" flags="OWN_CLIENT" persistent="true"/>
      <gold id="2" type="INT64" default="-5" flags="OWN_CLIENT" persistent="true"/>
      <inventory id="3" type="Inventory" flags="OWN_CLIENT"/>
      <active id="4" type="BOOL" default="true"/>
      <tags id="5" type="ARRAY" of="UINT32"/>
      <!-- ids 20-24 keep the compat section's id-6 append and id-8 reservation
           lines unambiguous, while pinning every non-integer field kind. -->
      <nick id="20" type="STRING"/>
      <blob id="21" type="BYTES"/>
      <flags id="22" type="ARRAY" of="BOOL"/>
      <aliases id="23" type="ARRAY" of="STRING"/>
      <certs id="24" type="ARRAY" of="BYTES"/>
    </Properties>
    <ServerMethods><Method name="attack" id="1" mode="notify" caller="owner">
      <Arg name="target" id="0" type="EntityRef"/>
    </Method><Method name="getProfile" id="2" mode="request" caller="server">
      <Arg name="account" id="0" type="UINT32"/>
      <Returns><Field name="level" id="1" type="UINT16"/></Returns>
    </Method></ServerMethods>
    <ClientMethods><Method name="onReward" id="1" mode="notify" audience="owner"/></ClientMethods>
  </Entity>
</Contracts>)";
    const auto good = c.path("avatar.def");
    write(good, avatar);
    auto& validated = c.spawn({self().string(), "contract", "check", good.string()});
    require(validated.wait() == 0, "check rejected a valid document: "+validated.output());
    const auto broken = c.path("broken.def");
    write(broken, std::string("<Contracts><Entity name=\"A\" id=\"1\">"
        "<Properties><x id=\"1\" type=\"WAT\"/></Properties></Entity></Contracts>"));
    auto& rejected = c.spawn({self().string(), "contract", "check", broken.string()});
    require(rejected.wait() != 0, "check accepted an invalid document");
    require(rejected.output().find("neither built-in") != std::string::npos,
            "check did not name the failing rule: "+rejected.output());
    // build emits the three generators, byte-for-byte reproducible.
    const auto one = c.path("bundle");
    require(c.command({self().string(), "contract", "build", "--out", one.string(), good.string()}) == 0,
            "build failed on a valid document");
    for (const auto& name : {"avatar.xsd", "avatar.lock", "avatar.digest", "avatar.sproto", "avatar.bindings.hpp", "avatar.bindings.js", "avatar.bindings.cs", "avatar.client.js", "avatar.client.cs", "bundle.manifest"})
        require(fs::is_regular_file(one/name), std::string("missing artifact: ")+name);
    const auto two = c.path("rebuild");
    require(c.command({self().string(), "contract", "build", "--out", two.string(), good.string()}) == 0,
            "rebuild failed");
    equal(read(one/"avatar.xsd"), read(two/"avatar.xsd"));
    equal(read(one/"avatar.lock"), read(two/"avatar.lock"));
    equal(read(one/"avatar.digest"), read(two/"avatar.digest"));
    equal(read(one/"avatar.sproto"), read(two/"avatar.sproto"));
    equal(read(one/"avatar.bindings.hpp"), read(two/"avatar.bindings.hpp"));
    equal(read(one/"avatar.bindings.js"), read(two/"avatar.bindings.js"));
    equal(read(one/"avatar.bindings.cs"), read(two/"avatar.bindings.cs"));
    equal(read(one/"avatar.client.js"), read(two/"avatar.client.js"));
    equal(read(one/"avatar.client.cs"), read(two/"avatar.client.cs"));
    equal(read(one/"bundle.manifest"), read(two/"bundle.manifest"));
    const auto generated_bindings=read(one/"avatar.bindings.hpp");
    require(generated_bindings.find("struct Avatar_server_attackArgs") != std::string::npos &&
            generated_bindings.find("std::int64_t target;") != std::string::npos &&
            generated_bindings.find("struct Avatar_server_getProfileArgs") != std::string::npos &&
            generated_bindings.find("std::uint32_t account;") != std::string::npos,
            "generated field-specific typed argument objects missing");
    const auto js_bindings = read(one/"avatar.bindings.js");
    require(js_bindings.find("encodeRecord") != std::string::npos &&
            js_bindings.find("decodeRecord") != std::string::npos &&
            js_bindings.find("sprotoFields") != std::string::npos &&
            // The generated field tables carry the schema-aware kind/of data
            // the JS/C# codecs recurse through — every field kind, including
            // the BOOL/STRING/BYTES scalar and array shapes.
            js_bindings.find("kind: 'struct', of: 'Inventory'") != std::string::npos &&
            js_bindings.find("kind: 'struct-array', of: 'Item'") != std::string::npos &&
            js_bindings.find("kind: 'int-array'") != std::string::npos &&
            js_bindings.find("kind: 'bool'") != std::string::npos &&
            js_bindings.find("kind: 'string'") != std::string::npos &&
            js_bindings.find("kind: 'bytes'") != std::string::npos &&
            js_bindings.find("kind: 'bool-array'") != std::string::npos &&
            js_bindings.find("kind: 'string-array'") != std::string::npos &&
            js_bindings.find("kind: 'bytes-array'") != std::string::npos,
            "generated JS field tables lost kind/of");
    const auto cs_bindings = read(one/"avatar.bindings.cs");
    require(cs_bindings.find("record Avatar") != std::string::npos &&
            cs_bindings.find("SprotoCodec") != std::string::npos &&
            cs_bindings.find("AvatarSchema") != std::string::npos &&
            cs_bindings.find("SprotoKind.Struct, \"Inventory\"") != std::string::npos &&
            cs_bindings.find("SprotoKind.StructArray, \"Item\"") != std::string::npos &&
            cs_bindings.find("SprotoKind.IntArray, null") != std::string::npos &&
            cs_bindings.find("\"active\", SprotoKind.Bool, null") != std::string::npos &&
            cs_bindings.find("\"nick\", SprotoKind.String, null") != std::string::npos &&
            cs_bindings.find("\"blob\", SprotoKind.Bytes, null") != std::string::npos &&
            cs_bindings.find("\"flags\", SprotoKind.BoolArray, null") != std::string::npos &&
            cs_bindings.find("\"aliases\", SprotoKind.StringArray, null") != std::string::npos &&
            cs_bindings.find("\"certs\", SprotoKind.BytesArray, null") != std::string::npos,
            "generated C# field tables lost kind/sub");
    // Client-audience artifacts carry only what a client may see: OWN_CLIENT
    // properties and owner-callable/notify methods. The server-internal method
    // (caller="server") and every SERVER_ONLY property stay out. The codec body
    // is byte-identical to the server one, so wire conformance is inherited.
    const auto js_client = read(one/"avatar.client.js");
    require(js_client.find("name: 'level'") != std::string::npos &&
            js_client.find("name: 'gold'") != std::string::npos &&
            js_client.find("name: 'inventory'") != std::string::npos &&
            js_client.find("Avatar_attack_Args") != std::string::npos &&
            js_client.find("Avatar_onReward_Args") != std::string::npos &&
            js_client.find("export function encodeRecord") != std::string::npos,
            "client JS artifact lost its client-visible surface");
    require(js_client.find("getProfile") == std::string::npos &&
            js_client.find("name: 'active'") == std::string::npos &&
            js_client.find("name: 'nick'") == std::string::npos &&
            js_client.find("name: 'blob'") == std::string::npos &&
            js_client.find("name: 'flags'") == std::string::npos &&
            js_client.find("name: 'aliases'") == std::string::npos &&
            js_client.find("name: 'certs'") == std::string::npos &&
            js_client.find("name: 'tags'") == std::string::npos,
            "client JS artifact leaked a private property");
    const auto cs_client = read(one/"avatar.client.cs");
    require(cs_client.find("public sealed record Avatar(") != std::string::npos &&
            cs_client.find("AvatarSchema") != std::string::npos &&
            cs_client.find("SprotoKind.Struct, \"Inventory\"") != std::string::npos &&
            cs_client.find("\"level\"") != std::string::npos &&
            cs_client.find("\"gold\"") != std::string::npos &&
            cs_client.find("Avatar_attackArgs") != std::string::npos &&
            cs_client.find("Avatar_onRewardArgs") != std::string::npos,
            "client C# artifact lost its client-visible surface");
    require(cs_client.find("getProfile") == std::string::npos &&
            cs_client.find("\"active\"") == std::string::npos &&
            cs_client.find("\"nick\"") == std::string::npos &&
            cs_client.find("\"blob\"") == std::string::npos &&
            cs_client.find("\"certs\"") == std::string::npos,
            "client C# artifact leaked a private descriptor");
    const auto manifest = read(one/"bundle.manifest");
    require(manifest.find("# mistlane server bundle manifest\n# format 1\n") == 0,
            "bundle manifest header malformed: "+manifest);
    for (const auto* listed : {"avatar.xsd", "avatar.lock", "avatar.digest", "avatar.sproto",
            "avatar.bindings.hpp", "avatar.bindings.js", "avatar.bindings.cs", "avatar.client.js", "avatar.client.cs"})
        require(manifest.find(std::string("file ")+listed+" sha256 ") != std::string::npos,
                std::string("manifest lost ")+listed);
    require(manifest.find("file bundle.manifest ") == std::string::npos, "manifest digests itself");
    const auto old_lock = c.path("old.lock"), compatible_lock = c.path("compatible.lock"), breaking_lock = c.path("breaking.lock");
    const auto release_lock = read(one/"avatar.lock");
    auto with_digest = [](std::string text, std::string_view digest) {
        const auto at = text.find("# digest ");
        require(at != std::string::npos, "test release lock missing version digest");
        text.replace(at+9, 16, digest);
        return text;
    };
    write(old_lock, release_lock);
    write(compatible_lock, with_digest(release_lock+" prop Avatar 6 nickname STRING\n", "1111111111111111"));
    auto& compatible = c.spawn({self().string(), "contract", "compat", old_lock.string(), compatible_lock.string()});
    require(compatible.wait()==0, "compatible release lock was rejected: "+compatible.output());
    std::cout << compatible.output();
    auto breaking_text = release_lock;
    const auto gold_line = std::string(" prop Avatar 2 gold INT64\n");
    const auto gold_at = breaking_text.find(gold_line);
    require(gold_at != std::string::npos, "gold property missing from test release lock");
    breaking_text.replace(gold_at, gold_line.size(), " prop Avatar 2 gold STRING\n");
    write(breaking_lock, with_digest(breaking_text, "2222222222222222"));
    auto& breaking = c.spawn({self().string(), "contract", "compat", old_lock.string(), breaking_lock.string()});
    require(breaking.wait()!=0, "breaking release lock passed the publication gate");
    require(breaking.output().find("changed") != std::string::npos,
            "type change report did not name the change: "+breaking.output());
    std::cout << breaking.output();
    // A lock listing the same assignment twice is refused outright: resolving
    // it by whichever line came last would let the published type drift.
    const auto duplicated_lock = c.path("duplicated.lock");
    write(duplicated_lock, with_digest(release_lock+" prop Avatar 2 gold INT64\n", "8888888888888888"));
    auto& duplicated = c.spawn({self().string(), "contract", "compat", old_lock.string(), duplicated_lock.string()});
    require(duplicated.wait()!=0, "a duplicated assignment in a release lock was accepted");
    require(duplicated.output().find("duplicate") != std::string::npos,
            "duplicated assignment was not named as malformed: "+duplicated.output());
    std::cout << duplicated.output();
    const auto removed_lock = c.path("removed.lock");
    auto removed_text = release_lock;
    const auto property = removed_text.find(" prop Avatar 2 gold INT64\n");
    require(property != std::string::npos, "gold property missing from test release lock");
    removed_text.erase(property, std::string(" prop Avatar 2 gold INT64\n").size());
    write(removed_lock, with_digest(removed_text, "3333333333333333"));
    auto& removed = c.spawn({self().string(), "contract", "compat", old_lock.string(), removed_lock.string()});
    require(removed.wait()!=0, "removed property passed the publication gate");
    std::cout << removed.output();
    auto struct_changed_text = release_lock;
    const auto array_field = struct_changed_text.find("{1 items ARRAY of Item}");
    require(array_field != std::string::npos, "array struct field missing from test lock");
    struct_changed_text.replace(array_field, std::string("{1 items ARRAY of Item}").size(), "{1 items STRING}");
    const auto struct_changed_lock = c.path("struct-changed.lock");
    write(struct_changed_lock, with_digest(struct_changed_text, "4444444444444444"));
    auto& struct_changed = c.spawn({self().string(), "contract", "compat", old_lock.string(), struct_changed_lock.string()});
    require(struct_changed.wait()!=0, "changed struct field type passed the publication gate");
    std::cout << struct_changed.output();
    // Reserved lines participate in the gate: a retired id taken by a live
    // meaning blocks, and so does dropping the reservation itself.
    const auto retired_lock = c.path("retired.lock"), reuse_lock = c.path("reuse.lock"), dropped_lock = c.path("dropped.lock");
    const auto retired_lines = std::string(" reserved Avatar property 8\n reserved Avatar server 9\n");
    write(retired_lock, with_digest(release_lock+retired_lines, "5555555555555555"));
    write(reuse_lock, with_digest(release_lock+retired_lines+" prop Avatar 8 magic UINT32\n", "6666666666666666"));
    auto& reuse = c.spawn({self().string(), "contract", "compat", retired_lock.string(), reuse_lock.string()});
    require(reuse.wait()!=0, "retired id reuse passed the publication gate");
    require(reuse.output().find("reused") != std::string::npos &&
            reuse.output().find("COMPATIBILITY_GATE BLOCKED") != std::string::npos,
            "reuse report did not name the collision: "+reuse.output());
    std::cout << reuse.output();
    write(dropped_lock, with_digest(release_lock, "7777777777777777"));
    auto& dropped = c.spawn({self().string(), "contract", "compat", retired_lock.string(), dropped_lock.string()});
    require(dropped.wait()!=0, "dropped reservation passed the publication gate");
    require(dropped.output().find("dropped") != std::string::npos,
            "dropped report did not name the reservation: "+dropped.output());
    std::cout << dropped.output();
    require(read(one/"avatar.sproto").find("E1_Server_1 ") != std::string::npos &&
            read(one/"avatar.sproto").find("E1_Client_1 ") != std::string::npos &&
            read(one/"avatar.sproto").find("items 1 : *Item") != std::string::npos &&
            read(one/"avatar.sproto").find("active 4 : boolean") != std::string::npos &&
            read(one/"avatar.sproto").find("nick 20 : string") != std::string::npos &&
            read(one/"avatar.sproto").find("blob 21 : binary") != std::string::npos &&
            read(one/"avatar.sproto").find("flags 22 : *boolean") != std::string::npos &&
            read(one/"avatar.sproto").find("aliases 23 : *string") != std::string::npos &&
            read(one/"avatar.sproto").find("certs 24 : *binary") != std::string::npos &&
            read(one/"avatar.sproto").find("E1_Server_2 514") != std::string::npos,
            "sproto schema lost stable E1 side/method identifiers");
    // The lock's digest line, the digest file and the library digest agree.
    const auto lock_text = read(one/"avatar.lock");
    const auto at = lock_text.find("# digest ");
    require(at != std::string::npos, "lock lost its digest line: "+lock_text);
    equal(lock_text.substr(at+9, 16), read(one/"avatar.digest").substr(0, 16));
    std::ostringstream hex;
    hex << std::hex << std::setw(16) << std::setfill('0') << EntityContract(avatar).digest();
    equal(hex.str(), read(one/"avatar.digest").substr(0, 16));
    // Server bundle loading is all-or-nothing: a clean build reports ready,
    // and a tampered artifact, a missing file, an unlisted stray, an
    // internally inconsistent re-signed manifest and an unknown manifest
    // format each refuse readiness and name the cause.
    auto sha256_of = [](const std::string& data) {
        unsigned char md[EVP_MAX_MD_SIZE]; std::size_t len = 0;
        require(EVP_Q_digest(nullptr, "SHA256", nullptr, data.data(), data.size(), md, &len) == 1 && len == 32,
                "sha256 of test data failed");
        static const char* digits = "0123456789abcdef";
        std::string out;
        for (unsigned int i = 0; i < len; ++i) { out += digits[md[i] >> 4]; out += digits[md[i] & 15]; }
        return out;
    };
    auto& ready = c.spawn({self().string(), "contract", "load", one.string()});
    require(ready.wait() == 0, "bundle load rejected a clean build: "+ready.output());
    require(ready.output().find("BUNDLE_READY") != std::string::npos,
            "ready load did not report readiness: "+ready.output());
    std::cout << ready.output();
    write(one/"avatar.bindings.js", read(one/"avatar.bindings.js")+"\n");
    auto& tampered = c.spawn({self().string(), "contract", "load", one.string()});
    require(tampered.wait() != 0, "a tampered artifact became ready");
    require(tampered.output().find("avatar.bindings.js") != std::string::npos &&
            tampered.output().find("digest mismatch") != std::string::npos,
            "tamper report did not name the artifact: "+tampered.output());
    write(one/"avatar.bindings.js", read(two/"avatar.bindings.js"));
    fs::remove(one/"avatar.sproto");
    auto& incomplete = c.spawn({self().string(), "contract", "load", one.string()});
    require(incomplete.wait() != 0 && incomplete.output().find("avatar.sproto") != std::string::npos,
            "a bundle with a missing artifact became ready: "+incomplete.output());
    write(one/"avatar.sproto", read(two/"avatar.sproto"));
    write(one/"junk.txt", "x");
    auto& swept = c.spawn({self().string(), "contract", "load", one.string()});
    require(swept.wait() != 0 && swept.output().find("junk.txt") != std::string::npos,
            "an unlisted file became ready: "+swept.output());
    fs::remove(one/"junk.txt");
    const auto resigned = c.path("resigned");
    fs::create_directories(resigned);
    for (const auto& entry : fs::directory_iterator(two))
        if (entry.is_regular_file())
            fs::copy_file(entry.path(), resigned/entry.path().filename(), fs::copy_options::overwrite_existing);
    const std::string lied_digest = "deadbeefdeadbeef\n";
    const auto digest_entry = std::string("file avatar.digest sha256 ");
    auto resigned_manifest = read(resigned/"bundle.manifest");
    write(resigned/"avatar.digest", lied_digest);
    const auto entry_at = resigned_manifest.find(digest_entry);
    require(entry_at != std::string::npos, "manifest lost the digest entry");
    resigned_manifest.replace(entry_at+digest_entry.size(), 64, sha256_of(lied_digest));
    write(resigned/"bundle.manifest", resigned_manifest);
    auto& resigned_load = c.spawn({self().string(), "contract", "load", resigned.string()});
    require(resigned_load.wait() != 0, "a re-signed internally inconsistent bundle became ready");
    require(resigned_load.output().find("model digest") != std::string::npos,
            "disagreement report did not name the model digest: "+resigned_load.output());
    const auto reformatted = c.path("reformatted");
    fs::create_directories(reformatted);
    for (const auto& entry : fs::directory_iterator(two))
        if (entry.is_regular_file())
            fs::copy_file(entry.path(), reformatted/entry.path().filename(), fs::copy_options::overwrite_existing);
    auto reformatted_manifest = read(reformatted/"bundle.manifest");
    const auto format_at = reformatted_manifest.find("# format 1");
    require(format_at != std::string::npos, "manifest lost its format line");
    reformatted_manifest.replace(format_at, 10, "# format 2");
    write(reformatted/"bundle.manifest", reformatted_manifest);
    auto& reformatted_load = c.spawn({self().string(), "contract", "load", reformatted.string()});
    require(reformatted_load.wait() != 0, "an unknown manifest format became ready");
    require(reformatted_load.output().find("unsupported bundle format") != std::string::npos,
            "format report did not name the version: "+reformatted_load.output());
    std::cout << "contract.bundle load: ready/tampered/missing/unlisted/resigned/format refusals [PASS]\n";
    // types.xml composite references: a <TypesSource path> inside <Contracts>
    // pulls reusable structs from a sibling type document. The tool resolves
    // the path (the XML reader never opens files, so external entities stay
    // disabled); the merge is a per-document snapshot folded into that
    // document's model, lock and digest; "a type is defined once" refuses a
    // name that already exists; missing and escaping paths are refused.
    const auto types_ctx = c.path("types-ctx");
    fs::create_directories(types_ctx/"shared");
    write(types_ctx/"shared"/"common.xml",
        "<Types><Gold><amount id=\"0\" type=\"UINT32\"/><total id=\"1\" type=\"INT64\"/></Gold>"
        "<Bag><capacity id=\"0\" type=\"UINT16\"/><slots id=\"1\" type=\"ARRAY\" of=\"Gold\"/></Bag></Types>");
    const auto hero_def = types_ctx/"shared"/"hero.def";
    write(hero_def, "<Contracts><TypesSource path=\"common.xml\"/>"
        "<Entity name=\"Hero\" id=\"2\"><Properties><pouch id=\"1\" type=\"Bag\"/><coins id=\"2\" type=\"Gold\"/></Properties>"
        "</Entity></Contracts>");
    const auto hero_out = types_ctx/"hero";
    require(c.command({self().string(), "contract", "build", "--out", hero_out.string(), hero_def.string()}) == 0,
            "build rejected a def with a types.xml include");
    const auto hero_bindings = read(hero_out/"hero.bindings.js");
    require(hero_bindings.find("Gold: [{ id: 0, name: 'amount'") != std::string::npos &&
            hero_bindings.find("Bag: [{ id: 0, name: 'capacity'") != std::string::npos &&
            hero_bindings.find("name: 'pouch'") != std::string::npos &&
            hero_bindings.find("name: 'coins'") != std::string::npos,
            "included types did not reach the JS field tables");
    require(read(hero_out/"hero.xsd").find("name=\"Gold\"") != std::string::npos &&
            read(hero_out/"hero.lock").find("struct Gold") != std::string::npos &&
            read(hero_out/"hero.lock").find("struct Bag") != std::string::npos,
            "included types did not reach the schema and release lock");
    const auto mage_def = types_ctx/"shared"/"mage.def";
    write(mage_def, "<Contracts><TypesSource path=\"common.xml\"/>"
        "<Entity name=\"Mage\" id=\"3\"><Properties><purse id=\"1\" type=\"Gold\"/></Properties></Entity></Contracts>");
    const auto mage_out = types_ctx/"mage";
    require(c.command({self().string(), "contract", "build", "--out", mage_out.string(), mage_def.string()}) == 0,
            "a second document sharing a type file failed");
    const auto hero_rebuild = types_ctx/"hero2";
    require(c.command({self().string(), "contract", "build", "--out", hero_rebuild.string(), hero_def.string()}) == 0,
            "hero rebuild failed");
    equal(read(hero_out/"hero.bindings.js"), read(hero_rebuild/"hero.bindings.js"));
    equal(read(hero_out/"hero.lock"), read(hero_rebuild/"hero.lock"));
    const auto dup_def = types_ctx/"shared"/"dup.def";
    write(dup_def, "<Contracts><Types><Bag><capacity id=\"0\" type=\"UINT16\"/></Bag></Types>"
        "<TypesSource path=\"common.xml\"/>"
        "<Entity name=\"Dup\" id=\"4\"><Properties><x id=\"1\" type=\"Bag\"/></Properties></Entity></Contracts>");
    auto& dup = c.spawn({self().string(), "contract", "check", dup_def.string()});
    require(dup.wait() != 0 && dup.output().find("defined once") != std::string::npos,
            "a duplicated included type passed: "+dup.output());
    const auto missing_def = types_ctx/"missing.def";
    write(missing_def, "<Contracts><TypesSource path=\"nope.xml\"/>"
        "<Entity name=\"M\" id=\"5\"><Properties><x id=\"1\" type=\"UINT32\"/></Properties></Entity></Contracts>");
    auto& missing = c.spawn({self().string(), "contract", "check", missing_def.string()});
    require(missing.wait() != 0 && missing.output().find("nope.xml") != std::string::npos,
            "a missing type source passed: "+missing.output());
    const auto escape_def = types_ctx/"escape.def";
    write(escape_def, "<Contracts><TypesSource path=\"../../evil.xml\"/>"
        "<Entity name=\"E\" id=\"6\"><Properties><x id=\"1\" type=\"UINT32\"/></Properties></Entity></Contracts>");
    auto& escape = c.spawn({self().string(), "contract", "check", escape_def.string()});
    require(escape.wait() != 0 && escape.output().find("escapes") != std::string::npos,
            "an escaping type source passed: "+escape.output());
    try {
        EntityContract("<Contracts><TypesSource path=\"x\"/>"
            "<Entity name=\"A\" id=\"1\"><Properties><x id=\"1\" type=\"UINT32\"/></Properties></Entity></Contracts>");
        require(false, "TypesSource without a resolver was accepted");
    } catch (const ContractError&) {}
    std::cout << "contract.types source include: resolve/shared/dup/missing/escape [PASS]\n";
    require(read(one/"avatar.xsd").find("name=\"Avatar\"") != std::string::npos,
            "schema lost the entity");
    const EntityContract dispatch_contract(avatar);
    EntityDispatcher dispatcher{dispatch_contract};
    dispatcher.bind("Avatar", "server", 2, [](const EntityCaller&, const EntityTarget&, const EntityValues&) {
        return EntityValues{{"level", std::int64_t{7}}};
    });
    EntityInvocation call{{1,1,1,42},{"server",9},"server",2,true,{}};
    require(dispatcher.dispatch(call).error == EntityError::INVALID, "negative: missing required field rejected as INVALID");
    std::cout << "sproto.dispatch negative missing_required: INVALID [PASS]\n";
    call.arguments={{"account",std::string("wrong")}};
    require(dispatcher.dispatch(call).error == EntityError::INVALID, "negative: wrong field type rejected as INVALID");
    std::cout << "sproto.dispatch negative wrong_field_type: INVALID [PASS]\n";
    call.arguments={{}}; call.encoded_ok=false;
    require(dispatcher.dispatch(call).error == EntityError::INVALID, "negative: damaged encoding rejected as INVALID");
    std::cout << "sproto.dispatch negative damaged_encoding: INVALID [PASS]\n";
    call.encoded_ok=true; call.side="server"; call.method_id=99;
    require(dispatcher.dispatch(call).error == EntityError::INVALID, "negative: unknown method rejected as INVALID");
    std::cout << "sproto.dispatch negative unknown_method: INVALID [PASS]\n";
    call.method_id=2; call.caller={"server",0}; call.arguments={{"account",std::int64_t{1}}};
    require(dispatcher.dispatch(call).error == EntityError::FORBIDDEN, "negative: missing authorized caller context rejected as FORBIDDEN");
    std::cout << "sproto.dispatch negative missing_caller_context: FORBIDDEN [PASS]\n";
    call.caller={"server",9}; call.arguments={{"account",std::int64_t{1}}};
    const auto dispatched=dispatcher.dispatch(call);
    require(dispatched.ok, "request dispatch error code="+std::to_string(static_cast<int>(dispatched.error)));
    require(std::get<std::int64_t>(dispatched.response.at("level").data)==7,
            "request dispatch did not return typed handler response");
    // A failed build writes nothing, so a broken change cannot clobber a bundle.
    const auto before_broken = c.path("before-broken");
    require(c.command({self().string(), "contract", "build", "--out", before_broken.string(),
                       good.string(), broken.string()}) != 0, "build accepted a broken document");
    require(!fs::exists(before_broken) || fs::is_empty(before_broken),
            "a failed build must not write artifacts");
    // Usage errors are non-zero.
    require(c.command({self().string(), "contract"}) != 0, "missing verb accepted");
    require(c.command({self().string(), "contract", "frobnicate", good.string()}) != 0,
            "unknown verb accepted");
    require(c.command({self().string(), "contract", "build", good.string()}) != 0,
            "build without --out accepted");
}
void sproto_golden() {
    // Fixture-only canonical TLV vectors. These intentionally are NOT sproto
    // wire bytes; retain them as an internal-format fixture independent of the
    // upstream sproto vectors. Each record is tag:u8, kind:u8, length:u16be, bytes.
    auto record = [](unsigned tag, unsigned kind, std::string_view value) {
        std::string out{static_cast<char>(tag), static_cast<char>(kind),
            static_cast<char>((value.size() >> 8) & 255), static_cast<char>(value.size() & 255)};
        out.append(value); return out;
    };
    auto hex = [](std::string_view bytes) {
        constexpr char digits[]="0123456789abcdef"; std::string out;
        for (unsigned char b : bytes) { out.push_back(digits[b>>4]); out.push_back(digits[b&15]); }
        return out;
    };
    const auto scalar = record(0, 1, std::string("\x7f\xff\xff\xff", 4));
    const auto array = record(1, 2, std::string("\x00\x02\x00\x00\xff\xff", 6));
    const auto nested = record(2, 3, record(0, 1, std::string("\x00\x00\x00\x01", 4)));
    const auto boundary = record(3, 1, std::string("\x80\x00\x00\x00", 4));
    equal(hex(scalar), "000100047fffffff");
    equal(hex(array), "0102000600020000ffff");
    equal(hex(nested), "020300080001000400000001");
    equal(hex(boundary), "0301000480000000");
    // The length parser is the decoding seam: decode and re-encode exactly.
    auto roundtrip = [&](const std::string& bytes) {
        require(bytes.size() >= 4, "truncated vector header");
        const auto len = (static_cast<unsigned char>(bytes[2]) << 8) |
            static_cast<unsigned char>(bytes[3]);
        require(bytes.size() == len + 4, "vector length mismatch");
        return record(static_cast<unsigned char>(bytes[0]), static_cast<unsigned char>(bytes[1]),
                      std::string_view(bytes).substr(4));
    };
    equal(roundtrip(scalar), scalar); equal(roundtrip(array), array);
    equal(roundtrip(nested), nested); equal(roundtrip(boundary), boundary);
}

void scaffold_init() {
    Context c; const auto root=c.path("sample service");
    require(c.command({self().string(),"init",root.string()})==0,"initializer failed");
    for (const auto& name : {"scripts/echo.lua","scripts/http.lua","scripts/cross.lua","scripts/login.lua","scripts/record.lua",
        "config/instances/gateway.env","config/instances/gateway-ws.env","config/instances/gateway-wss.env",
        "config/instances/login.env","config/instances/data.env","config/instances/coordinator.env",
        "config/instances/gameServer-mixed.env","ops/systemd/mistlane@.service"})
        require(fs::is_regular_file(root/name),"missing template: "+std::string(name));
    for (const auto& name : {"ops/run-instance.sh","ops/cluster.sh","run-local.sh","run-cross-local.sh"}) {
        require(::access((root/name).c_str(),X_OK)==0,"launcher is not executable");
        require(c.command({"sh","-n",(root/name).string()})==0,"invalid shell syntax");
    }
    const auto capture=c.path("args"), recorder=c.path("recorder.sh");
    write(recorder,"#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$MISTLANE_CAPTURE\"\n");
    fs::permissions(recorder,fs::perms::owner_all);
    ScopedEnv bin("MISTLANE_BIN",recorder.string()), output("MISTLANE_CAPTURE",capture.string());
    require(c.command({(root/"ops/run-instance.sh").string(),"gateway-ws"})==0,"WS template launch failed");
    require(read(capture).find("--transport\nws\n")!=std::string::npos,"WS transport not forwarded");
    require(c.command({(root/"ops/run-instance.sh").string(),"gateway-wss"})!=0,"missing certificate accepted");
    const auto cert=root/"certs/server chain.pem", key=c.path("server key.pem");
    write(cert,"fixture"); write(key,"fixture");
    auto config=read(root/"config/instances/gateway-wss.env");
    config.replace(config.find("CERT=certs/fullchain.pem"),std::string("CERT=certs/fullchain.pem").size(),"CERT='certs/server chain.pem'");
    config.replace(config.find("KEY=certs/private-key.pem"),std::string("KEY=certs/private-key.pem").size(),"KEY='"+key.string()+"'");
    write(root/"config/instances/gateway-wss.env",config);
    require(c.command({(root/"ops/run-instance.sh").string(),"gateway-wss"})==0,"WSS template launch failed");
    require(read(capture).find("--cert\n"+cert.string())!=std::string::npos,"relative certificate path not resolved");
    require(read(capture).find("--key\n"+key.string())!=std::string::npos,"absolute key path not resolved");
    require(c.command({(root/"ops/run-instance.sh").string(),"gameServer-mixed"})==0,"mixed template launch failed");
    require(read(capture).find("--listener\nws://127.0.0.1:7082\n")!=std::string::npos,"LISTENERS not forwarded");
    require(c.command({(root/"ops/run-instance.sh").string(),"login"})==0,"login template launch failed");
    require(read(capture).find("--coordinator\n127.0.0.1:7500\n")!=std::string::npos,"COORDINATOR not forwarded");
    require(read(capture).find("--scope\nmain\n")!=std::string::npos,"SCOPE not forwarded");
    require(c.command({(root/"ops/run-instance.sh").string(),"coordinator"})==0,"coordinator template launch failed");
    require(read(capture).find("--script")==std::string::npos,"the coordinator launched a script");
    require(c.command({(root/"ops/run-instance.sh").string(),"data"})==0,"data template launch failed");
    require(read(capture).find("--coordinator")==std::string::npos,"data claimed a coordinator");
    {
        ScopedEnv real_binary("MISTLANE_BIN",self().string());
        const auto listen=port();
        write(root/"config/instances/gameServer.env","ROLE=gameServer\nLISTEN="+endpoint(listen)+"\nSCRIPT=scripts/echo.lua\nTRANSPORT=tcp\nUPSTREAMS=\nROUTE_EPOCH=\n");
        auto& node=c.spawn({(root/"ops/run-instance.sh").string(),"gameServer"});
        node.ready(); equal(tcp(listen)->request("template-path-check"),"template-path-check");
        node.signal(SIGTERM); require(node.wait()==0,"generated gameServer shutdown failed");
    }
    {
        // The registry trio boots from its own templates: the coordinator is
        // scriptless, login asks it through the sample script and answers the
        // client from the placement callback.
        ScopedEnv real_binary("MISTLANE_BIN",self().string());
        const auto registry=port(), login_listen=port();
        write(root/"config/instances/coordinator.env","ROLE=coordinator\nLISTEN="+endpoint(registry)+"\nSCRIPT=\nTRANSPORT=tcp\nUPSTREAMS=\nROUTE_EPOCH=\n");
        write(root/"config/instances/login.env","ROLE=login\nLISTEN="+endpoint(login_listen)+"\nSCRIPT=scripts/login.lua\nTRANSPORT=tcp\nUPSTREAMS=\nROUTE_EPOCH=\nCOORDINATOR="+endpoint(registry)+"\nSCOPE=main\n");
        auto& registry_node=c.spawn({(root/"ops/run-instance.sh").string(),"coordinator"});
        registry_node.ready();
        auto& login_node=c.spawn({(root/"ops/run-instance.sh").string(),"login"});
        login_node.ready();
        equal(tcp(login_listen)->request("place gameServer"),"ERR EMPTY");
        login_node.signal(SIGTERM); require(login_node.wait()==0,"generated login shutdown failed");
        registry_node.signal(SIGTERM); require(registry_node.wait()==0,"generated coordinator shutdown failed");
    }
    {
        // cluster.sh gates each start on the instance reporting readiness and
        // rolls back instances started by the same command on failure.
        ScopedEnv real_binary("MISTLANE_BIN",self().string());
        const auto cluster=(root/"ops/cluster.sh").string();
        const auto listen=port();
        write(root/"config/instances/gameServer.env","ROLE=gameServer\nLISTEN="+endpoint(listen)+"\nSCRIPT=scripts/echo.lua\nTRANSPORT=tcp\nUPSTREAMS=\nROUTE_EPOCH=\n");
        const auto forwarder=root/"config/instances/gateway.env";
        const auto original_gateway=read(forwarder);
        write(forwarder,"ROLE=gateway\nLISTEN="+endpoint(port())+"\nSCRIPT=scripts/missing.lua\nTRANSPORT=tcp\nUPSTREAMS="+endpoint(listen)+"\nROUTE_EPOCH=\n");
        auto& started=c.spawn({cluster,"start","gameServer"});
        started.reap_descendants(false);  // the daemon must outlive its launcher
        require(started.wait()==0,"cluster start failed");
        equal(tcp(listen)->request("ready-gate"),"ready-gate");
        auto& preserved=c.spawn({cluster,"start","gameServer","gateway"});
        require(preserved.wait()!=0,"broken forwarder instance was accepted");
        require(preserved.output().find("stopping instances started by this command")!=std::string::npos,
            "rollback banner missing");
        auto& preserved_status=c.spawn({cluster,"status","gameServer","gateway"});
        preserved_status.wait();
        require(preserved_status.output().find("gameServer running")!=std::string::npos,
            "pre-existing instance was rolled back");
        require(preserved_status.output().find("gateway stopped")!=std::string::npos,
            "broken instance reported running");
        auto& stopped=c.spawn({cluster,"stop","gameServer"});
        require(stopped.wait()==0,"cluster stop failed");
        auto& rolled_back=c.spawn({cluster,"start","gameServer","gateway"});
        require(rolled_back.wait()!=0,"broken sequence was accepted");
        auto& final_status=c.spawn({cluster,"status","gameServer","gateway"});
        final_status.wait();
        require(final_status.output().find("gameServer stopped")!=std::string::npos,
            "freshly started instance survived rollback");
        require(final_status.output().find("gateway stopped")!=std::string::npos,
            "forwarder reported running after failure");
        write(forwarder,original_gateway);
    }
    write(root/"keep.txt","preserve");
    require(c.command({self().string(),"init",root.string()})!=0,"initializer overwrote nonempty project");
    equal(read(root/"keep.txt"),"preserve");
}
void cli_bot() {
    Context c; write(c.path("echo.lua"),echo);
    const auto listen=port(); c.node("gameServer",listen,{"--script",c.path("echo.lua").string()});
    auto& bot=c.spawn({self().string(),"bot","--url","tcp://"+endpoint(listen),
        "--clients","4","--count","3","--payload","native-bot","--expect-echo"});
    require(bot.wait()==0 && bot.output().find("completed=12")!=std::string::npos,"bot request accounting failed");
    require(c.command({self().string(),"bot","--url","tcp://"+endpoint(listen),"--expect","mismatch"})!=0,
        "bot ignored response mismatch");
    require(c.command({self().string(),"bot","--count","0"})!=0,"bot accepted invalid count");
    require(c.command({self().string(),"test","--case","does-not-exist"})!=0,"unknown test returned success");
    // A listening socket that never replies validates a bounded receive timeout.
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor stalled(io,{boost::asio::ip::address_v4::loopback(),0});
    const auto stalled_port=stalled.local_endpoint().port();
    auto& timeout=c.spawn({self().string(),"bot","--url","tcp://"+endpoint(stalled_port),"--timeout-ms","100"});
    require(timeout.wait(3s)!=0,"bot receive deadline was not enforced");
}
void crashpad_startup(const fs::path& handler) {
    require(fs::is_regular_file(handler),"provide --crashpad-handler FILE for the crashpad suite");
    Context c; write(c.path("echo.lua"),echo); const auto listen=port();
    auto& node=c.node("gameServer",listen,{"--script",c.path("echo.lua").string(),"--crashpad-handler",handler.string(),
        "--crashpad-database",c.path("reports").string()});
    require(fs::is_directory(c.path("reports")) && !fs::is_empty(c.path("reports")),"Crashpad database missing");
    equal(tcp(listen)->request("crashpad"),"crashpad");
    node.signal(SIGTERM); require(node.wait()==0,"Crashpad-enabled gameServer shutdown failed");
}
void crashpad_minidump(const fs::path& handler) {
    require(fs::is_regular_file(handler),"provide --crashpad-handler FILE for the crashpad suite");
    const auto sample=self().parent_path()/"mistlane-crash-sample";
    require(fs::is_regular_file(sample),"build mistlane-crash-sample for the crashpad suite");
    Context c; const auto database=c.path("reports");
    auto& victim=c.spawn({sample.string(),"--crashpad-handler",handler.string(),
        "--crashpad-database",database.string()});
    // The handler is a descendant that must stay alive long enough to finish
    // writing the dump, so opt out of the automatic group kill.
    victim.reap_descendants(false);
    const pid_t group=victim.group();
    bool found=false;
    eventually([&] {
        if (!fs::is_directory(database)) return false;
        for (auto entry : fs::recursive_directory_iterator(database)) {
            if (!entry.is_regular_file()) continue;
            std::ifstream stream(entry.path(),std::ios::binary); char magic[4]{};
            stream.read(magic,sizeof(magic));
            // Minidumps start with the MDMP magic and are far larger than a header.
            if (std::string(magic,magic+sizeof(magic))=="MDMP"
                && fs::file_size(entry.path())>4096) return found=true;
        }
        return false;
    },10s);
    // Give the handler a moment to finish the file before tearing it down.
    std::this_thread::sleep_for(100ms);
    require(victim.wait()==128+SIGSEGV,"crash sample did not die from SIGSEGV");
    require(found,"crashpad database never received a minidump");
    // The handler outlives the sample on purpose; do not leak it.
    if (group>0) ::kill(-group,SIGKILL);
}
}
