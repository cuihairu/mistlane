// SPDX-License-Identifier: Apache-2.0
#include "test_common.hpp"
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
    // A write through an index a remove has vacated must be refused like the
    // read path: descend bounds-checks every intermediate step (stale proxy).
    require(store.remove_at("inventory", {field("items")}, std::size_t{1}), "nested remove refused");
    require(!store.set_path("inventory", {field("items"), index(1), field("count")}, std::int64_t{5}),
            "write through a vacated index accepted: "+store.error());
    require(!store.insert_at("inventory", {field("items")}, std::size_t{3}, std::int64_t{1}),
            "insert beyond the shrunk array accepted");
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
// Hosting-side transport binding (MLE1): an invocation rides the existing
// bounded frames as a marker + envelope payload, the endpoint fences before it
// decodes arguments, and every request answers exactly one typed EntityReply
// carrying the inner correlation id so a gateway can route it back.
namespace {
struct StubCodec : EntityCodec {
    bool decode_args(const ContractArgs& args, std::string_view payload, EntityValues& out, std::string& reason) const override {
        if (args.empty()) {
            if (!payload.empty()) { reason = "unexpected payload"; return false; }
            return true;
        }
        if (args.size() == 1 && args[0].type == "INT32" && payload.size() == 4) {
            const auto b = [&](std::size_t i) { return static_cast<unsigned char>(payload[i]); };
            out.emplace(args[0].name, std::int64_t{static_cast<std::int32_t>(b(0) << 24 | b(1) << 16 | b(2) << 8 | b(3))});
            return true;
        }
        reason = "unsupported argument shape";
        return false;
    }
    std::optional<std::string> encode_returns(const ContractArgs& returns, const EntityValues& values) const override {
        if (returns.size() != 1 || returns[0].type != "UINT16") return std::nullopt;
        const auto it = values.find(returns[0].name);
        if (it == values.end()) return std::nullopt;
        const auto* n = std::get_if<std::int64_t>(&it->second.data);
        if (!n || *n < 0 || *n > 0xffff) return std::nullopt;
        std::string out;
        out.push_back(static_cast<char>(*n >> 8));
        out.push_back(static_cast<char>(*n & 0xff));
        return out;
    }
};
}
void entity_endpoint() {
    const EntityContract contract(R"(<Contracts><Entity name="Avatar" id="1"><ServerMethods><Method name="get" id="2" mode="request" caller="server"><Arg name="account" id="0" type="INT32"/><Returns><Field name="level" id="1" type="UINT16"/></Returns></Method><Method name="touch" id="3" mode="notify" caller="server"><Arg name="x" id="0" type="INT32"/></Method></ServerMethods></Entity></Contracts>)");
    StubCodec codec;
    EntityEndpoint endpoint(contract, &codec);
    endpoint.runtime().dispatcher().bind("Avatar", "server", 2,
        [](const EntityCaller&, const EntityTarget&, const EntityValues&) { return EntityValues{{"level", std::int64_t{7}}}; });
    bool touched = false;
    endpoint.runtime().dispatcher().bind("Avatar", "server", 3,
        [&](const EntityCaller&, const EntityTarget&, const EntityValues& args) {
            touched = std::get<std::int64_t>(args.at("x").data) == 4;
            return EntityValues{};
        });
    require(endpoint.runtime().spawn({1, 1, 3, 42}, 5), "endpoint spawn failed");
    const auto account4 = std::string("\x00\x00\x00\x04", 4);
    const auto request = [&](std::uint64_t correlation, std::uint32_t generation, std::uint32_t method,
                             std::uint32_t type_id, const std::string& payload) {
        return "MLE1" + encode_entity_envelope({1, type_id, 3, 42, kEntitySideServer,
            static_cast<std::uint16_t>(method), correlation, generation, 0, payload});
    };
    const auto reply = [](const std::optional<std::string>& frame) -> EntityReply {
        require(frame.has_value() && frame->starts_with("MLE1") && frame->size() >= 12,
                "expected an MLE1 reply frame");
        const auto decoded = decode_entity_reply(std::string_view(*frame).substr(12));
        require(decoded.has_value(), "reply body did not decode");
        return *decoded;
    };
    const auto correlation = [](const std::optional<std::string>& frame) {
        require(frame.has_value() && frame->size() >= 12, "reply frame too short");
        std::uint64_t value = 0;
        for (std::size_t i = 4; i < 12; ++i) value = value << 8 | static_cast<unsigned char>((*frame)[i]);
        return value;
    };
    std::string result;
    auto ok = endpoint.handle(request(10, 5, 2, 1, account4), {"server", 9}, result);
    require(result == "ok" && correlation(ok) == 10, "happy reply lost the correlation id");
    const auto got = reply(ok);
    require(!got.is_error && got.payload == std::string("\x00\x07", 2), "typed return payload mismatch");
    std::cout << "entity.endpoint request: MLE1 envelope, typed return, correlation routed [PASS]\n";

    require(!endpoint.handle("MLD1 LOAD x y", {"server", 9}, result).has_value(), "non-entity frame was consumed");
    std::cout << "entity.endpoint marker: non-entity frames pass through [PASS]\n";

    auto malformed = endpoint.handle("MLE1short", {"server", 9}, result);
    require(result == "malformed" && correlation(malformed) == 0
            && reply(malformed).is_error && reply(malformed).error == EntityError::INVALID
            && reply(malformed).reason == "malformed envelope", "malformed envelope was not a typed INVALID");
    std::cout << "entity.endpoint malformed: typed INVALID with a reason, correlation zero [PASS]\n";

    auto stale = endpoint.handle(request(11, 4, 2, 1, account4), {"server", 9}, result);
    require(result == "stale" && correlation(stale) == 11 && reply(stale).error == EntityError::STALE,
            "older hosting generation was not fenced");
    std::cout << "entity.endpoint fence: stale generation answered STALE [PASS]\n";

    auto unrouted = endpoint.handle(request(12, 5, 2, 7, account4), {"server", 9}, result);
    require(result == "unrouted" && reply(unrouted).error == EntityError::UNROUTED, "unknown entity was not UNROUTED");
    std::cout << "entity.endpoint route: unknown entity answered UNROUTED [PASS]\n";

    auto duplicate = endpoint.handle(request(10, 5, 2, 1, account4), {"server", 9}, result);
    require(result == "conflict" && reply(duplicate).error == EntityError::CONFLICT,
            "duplicate correlation was not CONFLICT");
    std::cout << "entity.endpoint slot: repeated correlation answered CONFLICT [PASS]\n";

    auto forbidden = endpoint.handle(request(13, 5, 2, 1, account4), {"server", 0}, result);
    require(result == "forbidden" && reply(forbidden).error == EntityError::FORBIDDEN,
            "zero caller id was not FORBIDDEN");
    std::cout << "entity.endpoint permission: zero caller id answered FORBIDDEN [PASS]\n";

    // The FORBIDDEN refusal burns no correlation slot: the same id still
    // dispatches for an authorized caller afterwards.
    auto legal13 = endpoint.handle(request(13, 5, 2, 1, account4), {"server", 9}, result);
    require(result == "ok" && !reply(legal13).is_error, "a refused call consumed the correlation slot");
    std::cout << "entity.endpoint permission: a FORBIDDEN call burns no slot [PASS]\n";

    // The tombstone set is bounded per entity incarnation: past the window the
    // oldest correlations age out, so an aged replay may deliver again (the
    // bounded at-least-once contract) instead of growing without bound.
    {
        constexpr std::uint64_t window = 4096, evict = 256;
        for (std::uint64_t id = 1000; id < 1000 + window + evict + 8; ++id)
            require(endpoint.handle(request(id, 5, 2, 1, account4), {"server", 9}, result).has_value(),
                "eviction loop call failed");
        auto replay = endpoint.handle(request(10, 5, 2, 1, account4), {"server", 9}, result);
        require(result == "ok", "an aged correlation still refused; the window is not bounded");
        (void)replay;
    }
    std::cout << "entity.endpoint slot: tombstones age out past the per-entity window [PASS]\n";

    const auto notified = endpoint.handle(request(0, 5, 3, 1, account4), {"server", 9}, result);
    require(notified.has_value() && notified->empty() && result == "notify" && touched,
            "notify was not handled without a reply");
    const auto notify_fail = endpoint.handle(request(0, 5, 3, 7, account4), {"server", 9}, result);
    require(notify_fail.has_value() && notify_fail->empty() && result == "notify-error",
            "unrouted notify did not surface a label");
    std::cout << "entity.endpoint notify: handled and answered nothing, failure surfaced [PASS]\n";
}

void entity_codec() {
    // The schema-driven sproto codec byte-locks against the same vectors the
    // vendored upstream encoder produces (src/sproto_probe.cpp sdk-vector mode
    // and the JS/C# SDK goldens): every wire form here is the exact hex locked
    // there. The contract below declares the Item/Inventory shapes the nested
    // goldens walk; each vector encodes its own top-level field shape.
    const EntityContract contract(R"(<Contracts><Types><Item><itemId id="0" type="UINT32"/><count id="1" type="UINT32"/></Item><Inventory><capacity id="0" type="UINT16"/><items id="1" type="ARRAY" of="Item"/></Inventory></Types><Entity name="Avatar" id="1"><ServerMethods><Method name="get" id="1" mode="request" caller="server"><Arg name="account" id="0" type="UINT32"/><Returns><Field name="level" id="1" type="UINT16"/></Returns></Method></ServerMethods></Entity></Contracts>)");
    const auto field = [](unsigned id, const char* name, const char* type, const char* of = "") {
        ContractField f; f.id = id; f.name = name; f.type = type; f.of = of; return f;
    };
    const auto hex = [](const std::string& bytes) {
        std::ostringstream out;
        for (const unsigned char byte : bytes) out << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
        return out.str();
    };
    const auto codec = SprotoCodec(contract);
    const auto encode = [&](const ContractArgs& shape, const EntityValues& values) {
        return codec.encode_returns(shape, values);
    };
    const auto decode = [&](const ContractArgs& shape, const std::string& payload) {
        EntityValues out; std::string reason;
        const bool ok = codec.decode_args(shape, payload, out, reason);
        return std::make_pair(ok, out);
    };
    const auto decode_why = [&](const ContractArgs& shape, const std::string& payload) {
        EntityValues out; std::string reason;
        const bool ok = codec.decode_args(shape, payload, out, reason);
        require(!ok, "malformed payload decoded");
        return reason;
    };
    // The Item shape for nested vectors: two in-band ints.
    const ContractArgs item_shape{field(0, "itemId", "UINT32"), field(1, "count", "UINT32")};
    // Every locked wire vector, in probe order.
    const auto one = encode(ContractArgs{field(1, "f1", "INT32")}, EntityValues{{"f1", std::int64_t{1}}});
    require(one && hex(*one) == "020001000400", "one: in-band integer word mismatch");
    const auto negative = encode(ContractArgs{field(4, "f4", "INT64")}, EntityValues{{"f4", std::int64_t{-9007199254740993LL}}});
    require(negative && hex(*negative) == "02000700000008000000ffffffffffffdfff", "negative: 8-byte two's-complement mismatch");
    const auto negative32 = encode(ContractArgs{field(1, "f1", "INT32")}, EntityValues{{"f1", std::int64_t{-1}}});
    require(negative32 && hex(*negative32) == "02000100000004000000ffffffff", "negative32: 4-byte two's-complement mismatch");
    const auto positive = encode(ContractArgs{field(1, "f1", "INT32"), field(4, "f4", "INT64")},
        EntityValues{{"f1", std::int64_t{65535}}, {"f4", std::int64_t{9007199254740993LL}}});
    require(positive && hex(*positive) == "0400010000000300000004000000ffff0000080000000100000000002000", "positive: length-prefixed ints mismatch");
    const auto nested = encode(ContractArgs{field(2, "inv", "Inventory")},
        EntityValues{{"inv", EntityObject{{"capacity", std::int64_t{1}},
            {"items", EntityArray{EntityObject{{"itemId", std::int64_t{7}}, {"count", std::int64_t{2}}},
                                   EntityObject{{"itemId", std::int64_t{0}}, {"count", std::int64_t{1}}}}}}}});
    require(nested && hex(*nested) == "0200030000001e000000020004000000140000000600000002001000060006000000020002000400",
            "nested: struct and struct-array mismatch");
    const auto tags = encode(ContractArgs{field(1, "f1", "INT32"), field(5, "f5", "ARRAY", "INT32")},
        EntityValues{{"f1", std::int64_t{1}}, {"f5", EntityArray{std::int64_t{0}, std::int64_t{1}, std::int64_t{2147483647}}}});
    require(tags && hex(*tags) == "040001000400050000000d000000040000000001000000ffffff7f", "tags: int array 32-bit mismatch");
    const auto totals = encode(ContractArgs{field(6, "f6", "ARRAY", "INT64")},
        EntityValues{{"f6", EntityArray{std::int64_t{-1}, std::int64_t{2147483648LL}, std::int64_t{4294967296LL}}}});
    require(totals && hex(*totals) == "02000b0000001900000008ffffffffffffffff00000080000000000000000001000000",
            "totals: int array 64-bit mismatch");
    const auto neg_array = encode(ContractArgs{field(5, "f5", "ARRAY", "INT32")},
        EntityValues{{"f5", EntityArray{std::int64_t{-1}, std::int64_t{-2147483648LL}}}});
    require(neg_array && hex(*neg_array) == "0200090000000900000004ffffffff00000080", "neg_array: negative int array 32-bit mismatch");
    const auto empty_tags = encode(ContractArgs{field(5, "f5", "ARRAY", "INT32")}, EntityValues{{"f5", EntityArray{}}});
    require(empty_tags && hex(*empty_tags) == "02000900000000000000", "empty_tags: zero-element int array mismatch");
    const auto empty_items = encode(ContractArgs{field(2, "inv", "Inventory")},
        EntityValues{{"inv", EntityObject{{"capacity", std::int64_t{0}}, {"items", EntityArray{}}}}});
    require(empty_items && hex(*empty_items) == "0200030000000a00000002000200000000000000", "empty_items: zero-element struct array mismatch");
    const auto text_set = encode(ContractArgs{field(7, "active", "BOOL"), field(8, "name", "STRING"),
            field(9, "blob", "BYTES"), field(10, "flags", "ARRAY", "BOOL"),
            field(11, "aliases", "ARRAY", "STRING"), field(12, "blobs", "ARRAY", "BYTES")},
        EntityValues{{"active", EntityValue(true)}, {"name", EntityValue(std::string("中文"))},
            {"blob", EntityValue(std::string("\0\xff", 2))},
            {"flags", EntityArray{EntityValue(true), EntityValue(false), EntityValue(true)}},
            {"aliases", EntityArray{EntityValue(std::string("latin")), EntityValue(std::string())}},
            {"blobs", EntityArray{EntityValue(std::string("\x01\x02", 2)), EntityValue(std::string())}}});
    require(text_set && hex(*text_set) == "07000d0004000000000000000000000006000000e4b8ade696870200000000ff030000000100010d000000050000006c6174696e000000000a00000002000000010200000000",
            "text_set: bool/string/bytes/arrays mismatch");
    const auto false_flag = encode(ContractArgs{field(7, "active", "BOOL")}, EntityValues{{"active", EntityValue(false)}});
    require(false_flag && hex(*false_flag) == "02000d000200", "false_flag: in-band false mismatch");
    const auto all_empty = encode(ContractArgs{field(8, "name", "STRING"), field(9, "blob", "BYTES"),
            field(10, "flags", "ARRAY", "BOOL"), field(11, "aliases", "ARRAY", "STRING"), field(12, "blobs", "ARRAY", "BYTES")},
        EntityValues{{"name", EntityValue(std::string())}, {"blob", EntityValue(std::string())},
            {"flags", EntityArray{}}, {"aliases", EntityArray{}}, {"blobs", EntityArray{}}});
    require(all_empty && hex(*all_empty) == "06000f00000000000000000000000000000000000000000000000000000000000000",
            "all_empty: empty string/bytes/arrays mismatch");
    std::cout << "entity.codec encode: eleven wire vectors byte-lock against upstream [PASS]\n";

    // Decode each vector back to typed values, then re-encode to the same bytes.
    const auto [one_ok, one_v] = decode(ContractArgs{field(1, "f1", "INT32")}, *one);
    require(one_ok && std::get<std::int64_t>(one_v.at("f1").data) == 1, "one did not decode");
    const auto [neg_ok, neg_v] = decode(ContractArgs{field(4, "f4", "INT64")}, *negative);
    require(neg_ok && std::get<std::int64_t>(neg_v.at("f4").data) == -9007199254740993LL, "negative did not decode");
    const auto [neg32_ok, neg32_v] = decode(ContractArgs{field(1, "f1", "INT32")}, *negative32);
    require(neg32_ok && std::get<std::int64_t>(neg32_v.at("f1").data) == -1, "negative32 did not sign-extend to -1");
    const auto [pos_ok, pos_v] = decode(ContractArgs{field(1, "f1", "INT32"), field(4, "f4", "INT64")}, *positive);
    require(pos_ok && std::get<std::int64_t>(pos_v.at("f1").data) == 65535
            && std::get<std::int64_t>(pos_v.at("f4").data) == 9007199254740993LL, "positive did not decode");
    const auto [nested_ok, nested_v] = decode(ContractArgs{field(2, "inv", "Inventory")}, *nested);
    require(nested_ok, "nested did not decode");
    {
        const auto& inv = std::get<EntityObject>(nested_v.at("inv").data);
        require(std::get<std::int64_t>(inv.at("capacity").data) == 1, "nested capacity wrong");
        const auto& items = std::get<EntityArray>(inv.at("items").data);
        require(items.size() == 2
                && std::get<std::int64_t>(std::get<EntityObject>(items[0].data).at("itemId").data) == 7
                && std::get<std::int64_t>(std::get<EntityObject>(items[0].data).at("count").data) == 2
                && std::get<std::int64_t>(std::get<EntityObject>(items[1].data).at("itemId").data) == 0
                && std::get<std::int64_t>(std::get<EntityObject>(items[1].data).at("count").data) == 1,
                "nested struct-array elements wrong");
    }
    const auto [tags_ok, tags_v] = decode(ContractArgs{field(1, "f1", "INT32"), field(5, "f5", "ARRAY", "INT32")}, *tags);
    require(tags_ok && std::get<std::int64_t>(tags_v.at("f1").data) == 1, "tags f1 wrong");
    {
        const auto& array = std::get<EntityArray>(tags_v.at("f5").data);
        require(array.size() == 3 && std::get<std::int64_t>(array[0].data) == 0
                && std::get<std::int64_t>(array[1].data) == 1
                && std::get<std::int64_t>(array[2].data) == 2147483647, "tags int array wrong");
    }
    {
        const auto [ok, values] = decode(ContractArgs{field(5, "f5", "ARRAY", "INT32")}, *neg_array);
        require(ok, "neg_array did not decode");
        const auto& array = std::get<EntityArray>(values.at("f5").data);
        require(array.size() == 2 && std::get<std::int64_t>(array[0].data) == -1
                && std::get<std::int64_t>(array[1].data) == -2147483648LL, "neg_array elements not sign-extended");
    }
    const auto [totals_ok, totals_v] = decode(ContractArgs{field(6, "f6", "ARRAY", "INT64")}, *totals);
    {
        require(totals_ok, "totals did not decode");
        const auto& array = std::get<EntityArray>(totals_v.at("f6").data);
        require(array.size() == 3 && std::get<std::int64_t>(array[0].data) == -1
                && std::get<std::int64_t>(array[1].data) == 2147483648LL
                && std::get<std::int64_t>(array[2].data) == 4294967296LL, "totals int array wrong");
    }
    const auto [empty_ok, empty_v] = decode(ContractArgs{field(5, "f5", "ARRAY", "INT32")}, *empty_tags);
    require(empty_ok && std::get<EntityArray>(empty_v.at("f5").data).empty(), "empty_tags did not decode empty");
    const auto [eitems_ok, eitems_v] = decode(ContractArgs{field(2, "inv", "Inventory")}, *empty_items);
    require(eitems_ok && std::get<EntityObject>(eitems_v.at("inv").data).size() == 2
            && std::get<EntityArray>(std::get<EntityObject>(eitems_v.at("inv").data).at("items").data).empty(),
            "empty_items did not decode empty struct array");
    const auto [text_ok, text_v] = decode(ContractArgs{field(7, "active", "BOOL"), field(8, "name", "STRING"),
            field(9, "blob", "BYTES"), field(10, "flags", "ARRAY", "BOOL"),
            field(11, "aliases", "ARRAY", "STRING"), field(12, "blobs", "ARRAY", "BYTES")}, *text_set);
    require(text_ok, "text_set did not decode");
    {
        require(std::get<bool>(text_v.at("active").data) && std::get<std::string>(text_v.at("name").data) == "中文"
                && std::get<std::string>(text_v.at("blob").data) == std::string("\0\xff", 2), "text_set scalars wrong");
        const auto& flags = std::get<EntityArray>(text_v.at("flags").data);
        const auto& aliases = std::get<EntityArray>(text_v.at("aliases").data);
        const auto& blobs = std::get<EntityArray>(text_v.at("blobs").data);
        require(flags.size() == 3 && std::get<bool>(flags[0].data) && !std::get<bool>(flags[1].data)
                && std::get<bool>(flags[2].data), "text_set bool array wrong");
        require(aliases.size() == 2 && std::get<std::string>(aliases[0].data) == "latin"
                && std::get<std::string>(aliases[1].data).empty(), "text_set string array wrong");
        require(blobs.size() == 2 && std::get<std::string>(blobs[0].data) == std::string("\x01\x02", 2)
                && std::get<std::string>(blobs[1].data).empty(), "text_set bytes array wrong");
    }
    const auto [false_ok, false_v] = decode(ContractArgs{field(7, "active", "BOOL")}, *false_flag);
    require(false_ok && !std::get<bool>(false_v.at("active").data), "false_flag did not decode false");
    const auto [all_ok, all_v] = decode(ContractArgs{field(8, "name", "STRING"), field(9, "blob", "BYTES"),
            field(10, "flags", "ARRAY", "BOOL"), field(11, "aliases", "ARRAY", "STRING"), field(12, "blobs", "ARRAY", "BYTES")}, *all_empty);
    require(all_ok && std::get<std::string>(all_v.at("name").data).empty()
            && std::get<EntityArray>(all_v.at("flags").data).empty(), "all_empty did not decode empty");
    std::cout << "entity.codec decode: eleven wire vectors decode to typed values [PASS]\n";

    // Roundtrip: decode then re-encode reproduces the locked bytes.
    const auto roundtrip = [&](const ContractArgs& shape, const std::string& wire) {
        const auto [ok, values] = decode(shape, wire);
        require(ok, "roundtrip source did not decode");
        const auto re = encode(shape, values);
        require(re && hex(*re) == hex(wire), "roundtrip re-encode drifted");
    };
    roundtrip(ContractArgs{field(1, "f1", "INT32")}, *one);
    roundtrip(ContractArgs{field(4, "f4", "INT64")}, *negative);
    roundtrip(ContractArgs{field(1, "f1", "INT32")}, *negative32);
    roundtrip(ContractArgs{field(1, "f1", "INT32"), field(4, "f4", "INT64")}, *positive);
    roundtrip(ContractArgs{field(2, "inv", "Inventory")}, *nested);
    roundtrip(ContractArgs{field(1, "f1", "INT32"), field(5, "f5", "ARRAY", "INT32")}, *tags);
    roundtrip(ContractArgs{field(6, "f6", "ARRAY", "INT64")}, *totals);
    roundtrip(ContractArgs{field(5, "f5", "ARRAY", "INT32")}, *neg_array);
    roundtrip(ContractArgs{field(5, "f5", "ARRAY", "INT32")}, *empty_tags);
    roundtrip(ContractArgs{field(2, "inv", "Inventory")}, *empty_items);
    roundtrip(ContractArgs{field(7, "active", "BOOL"), field(8, "name", "STRING"), field(9, "blob", "BYTES"),
        field(10, "flags", "ARRAY", "BOOL"), field(11, "aliases", "ARRAY", "STRING"), field(12, "blobs", "ARRAY", "BYTES")}, *text_set);
    roundtrip(ContractArgs{field(7, "active", "BOOL")}, *false_flag);
    roundtrip(ContractArgs{field(8, "name", "STRING"), field(9, "blob", "BYTES"), field(10, "flags", "ARRAY", "BOOL"),
        field(11, "aliases", "ARRAY", "STRING"), field(12, "blobs", "ARRAY", "BYTES")}, *all_empty);
    std::cout << "entity.codec roundtrip: decode then re-encode reproduces locked bytes [PASS]\n";

    // Malformed inputs fail closed with a reason (INVALID on the wire).
    require(decode_why(ContractArgs{field(1, "f1", "INT32")}, std::string("\x01", 1)) == "truncated tag count",
            "truncated count was not rejected");
    require(decode_why(ContractArgs{field(1, "f1", "INT32")}, std::string("\x04\x00", 2)) == "truncated tags",
            "truncated tags were not rejected");
    require(decode_why(ContractArgs{field(1, "f1", "INT32")}, *one + std::string("\x00", 1)) == "trailing bytes after record",
            "trailing bytes were not rejected");
    require(decode_why(ContractArgs{field(8, "s", "STRING")}, std::string("\x02\x00\x0f\x00\x04\x00", 6)) == "in-band non-integer field",
            "in-band string was not rejected");
    require(decode_why(ContractArgs{field(8, "s", "STRING")}, std::string("\x02\x00\x0f\x00\x00\x00\x01\x00\x00\x00\xff", 11)) == "invalid utf-8 string",
            "invalid UTF-8 string was not rejected");
    require(decode_why(ContractArgs{field(8, "s", "STRING")}, std::string("\x02\x00\x0f\x00\x00\x00\x04\x00\x00\x00\xf5\x80\x80\x80", 14)) == "invalid utf-8 string",
            "UTF-8 code point above U+10FFFF was not rejected");
    require(decode_why(ContractArgs{field(10, "flags", "ARRAY", "BOOL")}, std::string("\x02\x00\x13\x00\x00\x00\x01\x00\x00\x00\x02", 11)) == "bool array element",
            "bool array element above one was not rejected");
    require(decode_why(ContractArgs{field(1, "f1", "INT32")}, std::string("\x02\x00\x01\x00\x00\x00\x02\x00\x00\x00\x00\x00", 12)) == "integer width",
            "bad integer width was not rejected");
    // Unknown tags are skipped, and a length-prefixed unknown still advances the
    // data cursor so a later known field stays aligned: tag 8 is not declared,
    // its length-prefixed byte is skipped, and in-band tag 9 still decodes.
    {
        const auto [ok, values] = decode(ContractArgs{field(9, "f9", "INT32")},
            std::string("\x03\x00\x0f\x00\x00\x00\x06\x00\x01\x00\x00\x00\x41", 13));
        require(ok && std::get<std::int64_t>(values.at("f9").data) == 2,
                "unknown length-prefixed field misaligned the data cursor");
    }
    std::cout << "entity.codec malformed: fail-closed with a reason, unknown tags skipped [PASS]\n";
}

void entity_snapshot() {
    // Property-snapshot codec: the full declared property set of one entity is
    // a wire record whose field ids are the property ids, in the same
    // schema-driven sproto form the invocation codec uses. This is the
    // snapshot half of the "type storage and nested sync" boundary — what a
    // baseline or a data peer would carry for one entity.
    const EntityContract contract(R"(<Contracts><Types><Item><itemId id="0" type="UINT32"/><count id="1" type="UINT32"/></Item><Inventory><capacity id="0" type="UINT16"/><items id="1" type="ARRAY" of="Item"/></Inventory></Types><Entity name="Avatar" id="1"><Properties><level id="1" type="UINT16" flags="OWN_CLIENT"/><inventory id="2" type="Inventory"/><score id="3" type="INT32" flags="OWN_CLIENT"/><exp id="4" type="INT64"/><tags id="5" type="ARRAY" of="UINT32"/><totals id="6" type="ARRAY" of="INT64"/><active id="7" type="BOOL" flags="OWN_CLIENT"/><name id="8" type="STRING" flags="OWN_CLIENT"/><blob id="9" type="BYTES"/><flags id="10" type="ARRAY" of="BOOL"/><aliases id="11" type="ARRAY" of="STRING"/><blobs id="12" type="ARRAY" of="BYTES"/></Properties></Entity></Contracts>)");
    const auto& entity = contract.entities().at(0);
    const auto codec = SprotoCodec(contract);
    const auto hex = [](const std::string& bytes) {
        std::ostringstream out;
        for (const unsigned char byte : bytes) out << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
        return out.str();
    };

    // Encode a snapshot with a nested struct, a struct array, scalar arrays
    // and the scalar kinds; the field tags are the property ids in order.
    const EntityValues values{
        {"level", std::int64_t{65535}},
        {"inventory", EntityObject{{"capacity", std::int64_t{100}},
            {"items", EntityArray{EntityObject{{"itemId", std::int64_t{7}}, {"count", std::int64_t{2}}},
                                   EntityObject{{"itemId", std::int64_t{0}}, {"count", std::int64_t{1}}}}}}},
        {"score", std::int64_t{-1}},
        {"exp", std::int64_t{9007199254740993LL}},
        {"tags", EntityArray{std::int64_t{0}, std::int64_t{1}, std::int64_t{2147483647}}},
        {"totals", EntityArray{std::int64_t{-1}, std::int64_t{2147483648LL}, std::int64_t{4294967296LL}}},
        {"active", true},
        {"name", std::string("mistlane")},
        {"blob", std::string("\x01\x02", 2)},
        {"flags", EntityArray{true, false, true}},
        {"aliases", EntityArray{std::string("a"), std::string("b")}},
        {"blobs", EntityArray{std::string("\x00", 1), std::string("\xff", 1)}},
    };
    const auto wire = codec.encode_properties(entity, values);
    require(bool(wire), "snapshot encode rejected a valid property set");

    // The exact locked record: tags for ids 1..12, then the data segment in
    // property-id order (the same sproto record form the invocation codec
    // locks; here the property ids are the field ids). The wire is generated
    // against the same declared shapes the invocation codec already locks, so
    // the expected bytes are assembled from the per-field word/data pieces.
    const std::string locked =
        "0d00010000000000000000000000000004000000000000000000000004000000ffff00001e0000000200ca00000014000000060000000200100006000600000002000200040004000000ffffffff0800000001000000000020000d000000040000000001000000ffffff7f1900000008ffffffffffffffff00000080000000000000000001000000080000006d6973746c616e65020000000102030000000100010a000000010000006101000000620a000000010000000001000000ff";
 // blobs bytes array
 // blobs bytes array
    require(hex(*wire) == locked, "snapshot wire mismatch: got "+hex(*wire)+" want "+locked);

    // Decode round-trips to the same typed values.
    EntityValues back; std::string reason;
    require(codec.decode_properties(entity, *wire, back, reason), "snapshot decode failed: "+reason);
    require(std::get<std::int64_t>(back.at("level").data) == 65535, "level did not round-trip");
    require(std::get<std::int64_t>(back.at("score").data) == -1, "score did not round-trip");
    require(std::get<std::int64_t>(back.at("exp").data) == 9007199254740993LL, "exp did not round-trip");
    require(std::get<bool>(back.at("active").data) == true, "active did not round-trip");
    require(std::get<std::string>(back.at("name").data) == "mistlane", "name did not round-trip");
    require(std::get<std::string>(back.at("blob").data) == std::string("\x01\x02", 2), "blob did not round-trip");
    const auto& inv = std::get<EntityObject>(back.at("inventory").data);
    require(std::get<std::int64_t>(inv.at("capacity").data) == 100, "inventory capacity drifted");
    const auto& items = std::get<EntityArray>(inv.at("items").data);
    require(items.size() == 2 && std::get<std::int64_t>(std::get<EntityObject>(items[0].data).at("itemId").data) == 7,
            "inventory items drifted");
    const auto& tags = std::get<EntityArray>(back.at("tags").data);
    require(tags.size() == 3 && std::get<std::int64_t>(tags[2].data) == 2147483647, "tags drifted");
    const auto& totals = std::get<EntityArray>(back.at("totals").data);
    require(totals.size() == 3 && std::get<std::int64_t>(totals[0].data) == -1, "totals drifted");
    const auto& aliases = std::get<EntityArray>(back.at("aliases").data);
    require(aliases.size() == 2 && std::get<std::string>(aliases[1].data) == "b", "aliases drifted");
    const auto& blobs = std::get<EntityArray>(back.at("blobs").data);
    require(blobs.size() == 2 && std::get<std::string>(blobs[1].data) == "\xff", "blobs drifted");

    // Re-encode reproduces the locked bytes.
    const auto re = codec.encode_properties(entity, back);
    require(re && hex(*re) == locked, "snapshot re-encode drifted");

    // A snapshot missing a declared property is refused; trailing bytes are
    // refused; a malformed payload is refused with a reason.
    EntityValues partial = values;
    partial.erase("name");
    require(!codec.encode_properties(entity, partial), "snapshot with a missing property encoded");
    EntityValues malformed; std::string why;
    require(!codec.decode_properties(entity, *wire + std::string("\x00", 1), malformed, why) && why == "trailing bytes after record",
            "snapshot trailing bytes were not refused");
    require(!codec.decode_properties(entity, std::string("\x01", 1), malformed, why) && why == "truncated tag count",
            "snapshot truncated record was not refused");

    // The largest sproto field tag (32766, the 16-bit ceiling from
    // (tag+1)*2 < 0x10000) is representable: the gap from id 1 to id 32766
    // must round-trip rather than truncating the gap word.
    const EntityContract sparse(R"(<Contracts><Entity name="Sparse" id="2"><Properties>
        <first id="1" type="UINT32"/><last id="32766" type="UINT32"/>
        </Properties></Entity></Contracts>)");
    const auto& sparse_entity = sparse.entities().at(0);
    const SprotoCodec sparse_codec(sparse);
    const EntityValues sparse_values{{"first", std::int64_t{1}}, {"last", std::int64_t{2}}};
    const auto sparse_wire = sparse_codec.encode_properties(sparse_entity, sparse_values);
    EntityValues sparse_back; std::string sparse_reason;
    require(sparse_wire && sparse_codec.decode_properties(sparse_entity, *sparse_wire, sparse_back, sparse_reason),
            "maximum sproto field-tag gap did not round-trip: "+sparse_reason);
    require(std::get<std::int64_t>(sparse_back.at("last").data) == 2,
            "maximum sproto field-tag gap lost its field");

    // A tag beyond the 16-bit ceiling is refused at contract parse time, and
    // the codec itself fails closed rather than emitting a truncated gap word.
    bool overflow_refused = false;
    try {
        EntityContract(R"(<Contracts><Entity name="Overflow" id="3"><Properties>
            <first id="1" type="UINT32"/><last id="65535" type="UINT32"/>
            </Properties></Entity></Contracts>)");
    } catch (const ContractError&) {
        overflow_refused = true;
    }
    require(overflow_refused, "an unrepresentable sproto property tag was accepted");
    auto unsafe_entity = sparse_entity;
    unsafe_entity.properties.back().id = 65535;
    require(!sparse_codec.encode_properties(unsafe_entity, sparse_values),
            "the codec truncated an unrepresentable sproto property tag");
    std::cout << "entity.snapshot property-snapshot codec: encode/decode wire lock, roundtrip, fail-closed [PASS]\n";
}

void entity_gateway() {
    // The gateway maps each client's outer transport correlation onto a fresh
    // inner entity correlation and back through a bounded table, so one channel
    // multiplexes many entity requests (docs/entity-contract.md).
    GatewayEntityCorrelation map;
    require(map.pending() == 0, "fresh map is not empty");
    const auto a = map.allocate(5, 10);
    require(a != 0 && map.pending() == 1, "first allocate did not create a slot");
    const auto resolved = map.resolve(a);
    require(resolved && resolved->first == 5 && resolved->second == 10 && map.pending() == 0,
            "resolve did not return the origin and consume the slot");
    std::cout << "entity.gateway allocate/resolve: outer correlation round-trips and consumes [PASS]\n";

    // The same outer correlation on different channels is a different request;
    // one channel can multiplex several outers.
    const auto a1 = map.allocate(5, 10), b1 = map.allocate(5, 11), c1 = map.allocate(6, 10);
    require(a1 != 0 && b1 != 0 && c1 != 0 && a1 != b1 && b1 != c1 && a1 != c1 && map.pending() == 3,
            "multiplexed allocations collided");
    const auto ra = map.resolve(a1), rb = map.resolve(b1), rc = map.resolve(c1);
    require(ra && ra->first == 5 && ra->second == 10 && rb && rb->first == 5 && rb->second == 11
            && rc && rc->first == 6 && rc->second == 10, "multiplexed origins misresolved");
    std::cout << "entity.gateway multiplex: one channel fans out, origins stay distinct [PASS]\n";

    // Slots are one-shot: a stale or duplicate reply is rejected.
    require(!map.resolve(a1), "a consumed slot resolved again");
    require(!map.resolve(9999), "an unknown inner correlation resolved");
    std::cout << "entity.gateway one-shot: stale reply and unknown correlation rejected [PASS]\n";

    // A duplicate (connection, outer) pair while pending is refused, not re-mapped.
    const auto d0 = map.allocate(5, 10), d1 = map.allocate(5, 11);
    require(d0 != 0 && d1 != 0 && map.pending() == 2, "pre-duplicate allocations failed");
    const auto dup = map.allocate(5, 10);
    require(dup == 0 && map.pending() == 2, "duplicate outer on a channel was not refused");
    map.drop(5);
    require(map.pending() == 0, "duplicate test did not clean up");
    std::cout << "entity.gateway duplicate: repeated outer correlation refused while pending [PASS]\n";

    // Disconnecting a client drops only its own pending mappings.
    const auto d2 = map.allocate(5, 20), e1 = map.allocate(6, 20);
    map.drop(5);
    require(!map.resolve(d2) && map.resolve(e1) && map.pending() == 0, "drop did not isolate the connection");
    std::cout << "entity.gateway drop: disconnect cleans only its own channel [PASS]\n";

    // The table is bounded: full means refuse, not evict; a freed slot reuses.
    GatewayEntityCorrelation full;
    for (std::uint64_t i = 1; i <= GatewayEntityCorrelation::kCapacity; ++i)
        require(full.allocate(1, i) == i, "capacity fill stopped early");
    require(full.pending() == GatewayEntityCorrelation::kCapacity, "capacity fill miscounted");
    require(full.allocate(1, GatewayEntityCorrelation::kCapacity + 1000) == 0,
            "a full table did not refuse an allocation");
    require(full.resolve(1) && full.allocate(1, GatewayEntityCorrelation::kCapacity + 1000) != 0,
            "a freed slot was not reused");
    std::cout << "entity.gateway capacity: bounded, refuses when full, reuses a freed slot [PASS]\n";
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

}
