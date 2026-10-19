// SPDX-License-Identifier: Apache-2.0
#include "mistlane/entity_lua.hpp"
#include <algorithm>
#include <set>
#include <utility>

namespace mistlane {
namespace {
constexpr const char* kProxyMeta = "mistlane.entity.properties";
constexpr const char* kMethodKeys[] = {"names", "view", "reserved"};

PropertyStore* proxy(lua_State* state, int index) {
    auto* slot = static_cast<PropertyStore**>(lua_touserdata(state, index));
    if (!slot || !lua_getmetatable(state, index)) {
        luaL_error(state, "property proxy expected");
        return nullptr;
    }
    luaL_getmetatable(state, kProxyMeta);
    const bool matches = lua_rawequal(state, -1, -2) == 1;
    lua_pop(state, 2);
    if (!matches) luaL_error(state, "property proxy expected");
    return *slot;
}

// One EntityValue becomes one Lua value: scalars keep their kind, arrays and
// structs become plain tables.
void push_value(lua_State* state, const EntityValue& value) {
    if (const auto* v = std::get_if<bool>(&value.data)) { lua_pushboolean(state, *v ? 1 : 0); return; }
    if (const auto* v = std::get_if<std::int64_t>(&value.data)) { lua_pushinteger(state, *v); return; }
    if (const auto* v = std::get_if<std::string>(&value.data)) { lua_pushlstring(state, v->data(), v->size()); return; }
    if (const auto* v = std::get_if<EntityArray>(&value.data)) {
        lua_createtable(state, static_cast<int>(v->size()), 0);
        int at = 1;
        for (const auto& item : *v) { push_value(state, item); lua_rawseti(state, -2, at++); }
        return;
    }
    const auto& object = std::get<EntityObject>(value.data);
    lua_createtable(state, 0, static_cast<int>(object.size()));
    for (const auto& [name, item] : object) {
        lua_pushlstring(state, name.data(), name.size());
        push_value(state, item);
        lua_rawset(state, -3);
    }
}

// One Lua value becomes one EntityValue of the declared shape. The empty
// string means success; any other string is a refusal reason the caller
// prefixes with the property name. A wrong Lua kind never coerces: a number
// never becomes a string field, a non-integer never rounds.
std::string convert(lua_State* state, int index, const std::string& type, const std::string& of,
                    const EntityContract& contract, EntityValue* out) {
    if (type == "ARRAY") {
        if (lua_type(state, index) != LUA_TTABLE) return "expects an array table";
        EntityArray items;
        std::size_t seen = 0;
        lua_pushnil(state);
        while (lua_next(state, index) != 0) {
            ++seen;
            if (lua_type(state, -2) != LUA_TNUMBER ||
                lua_tointeger(state, -2) != static_cast<lua_Integer>(seen)) {
                lua_pop(state, 2);
                return "expects a dense array with keys 1.."+std::to_string(seen);
            }
            EntityValue item(std::int64_t{0});
            const auto reason = convert(state, -1, of, {}, contract, &item);
            if (!reason.empty()) { lua_pop(state, 2); return "element "+std::to_string(seen)+" "+reason; }
            items.push_back(std::move(item));
            lua_pop(state, 1);
        }
        *out = EntityValue(std::move(items));
        return {};
    }
    if (type == "BOOL") {
        if (lua_type(state, index) != LUA_TBOOLEAN) return "expects a boolean";
        *out = EntityValue(lua_toboolean(state, index) != 0);
        return {};
    }
    if (type == "INT32" || type == "UINT16" || type == "UINT32" || type == "INT64" || type == "EntityRef") {
        if (lua_type(state, index) != LUA_TNUMBER || !lua_isinteger(state, index)) return "expects an integer";
        const auto value = lua_tointeger(state, index);
        if ((type == "UINT16" && (value < 0 || value > 65535)) ||
            (type == "UINT32" && (value < 0 || static_cast<std::uint64_t>(value) > 4294967295ull)) ||
            (type == "INT32" && (value < -2147483647LL - 1 || value > 2147483647LL)))
            return "value "+std::to_string(value)+" is outside "+type;
        *out = EntityValue(static_cast<std::int64_t>(value));
        return {};
    }
    if (type == "STRING" || type == "BYTES") {
        if (lua_type(state, index) != LUA_TSTRING) return "expects a string";
        std::size_t size = 0;
        const auto* bytes = lua_tolstring(state, index, &size);
        *out = EntityValue(std::string(bytes, size));
        return {};
    }
    // Struct: every declared field exactly once, no extras.
    const auto spec = contract.structs().find(type);
    if (spec == contract.structs().end()) return "resolves to an unknown struct";
    if (lua_type(state, index) != LUA_TTABLE) return "expects a table for struct "+type;
    EntityObject object;
    lua_pushnil(state);
    while (lua_next(state, index) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING) { lua_pop(state, 2); return "expects string field names"; }
        std::size_t size = 0;
        const auto* bytes = lua_tolstring(state, -2, &size);
        const std::string field(bytes, size);
        const auto shape = std::find_if(spec->second.begin(), spec->second.end(),
                                        [&](const ContractField& f) { return f.name == field; });
        if (shape == spec->second.end()) { lua_pop(state, 2); return "has unexpected field '"+field+"'"; }
        EntityValue member(std::int64_t{0});
        const auto reason = convert(state, -1, shape->type, shape->of, contract, &member);
        if (!reason.empty()) { lua_pop(state, 2); return "field '"+field+"' "+reason; }
        if (!object.emplace(field, std::move(member)).second) { lua_pop(state, 2); return "repeats field '"+field+"'"; }
        lua_pop(state, 1);
    }
    for (const auto& f : spec->second)
        if (!object.count(f.name)) return "is missing field '"+f.name+"'";
    *out = EntityValue(std::move(object));
    return {};
}

std::string key_of(lua_State* state) {
    std::size_t size = 0;
    const auto* bytes = lua_tolstring(state, 2, &size);
    return std::string(bytes, size);
}

const ContractProperty* declared(const PropertyStore& store, const std::string& name) {
    const auto& props = store.entity().properties;
    const auto prop = std::find_if(props.begin(), props.end(),
                                   [&](const ContractProperty& p) { return p.name == name; });
    return prop == props.end() ? nullptr : &*prop;
}

int names_method(lua_State* state);
int reserved_method(lua_State* state);
int view_method(lua_State* state);

int index_proxy(lua_State* state) {
    auto* store = proxy(state, 1);
    if (!store || lua_type(state, 2) != LUA_TSTRING) return luaL_error(state, "property keys are strings");
    const auto name = key_of(state);
    // Binding methods answer first; push_entity_properties refuses contracts
    // that would have one shadowed by a same-named property.
    if (name == "names") { lua_pushcfunction(state, names_method); return 1; }
    if (name == "view") { lua_pushcfunction(state, view_method); return 1; }
    if (name == "reserved") { lua_pushcfunction(state, reserved_method); return 1; }
    const auto* prop = declared(*store, name);
    if (!prop)
        return luaL_error(state, "property '%s' is not a declared property on entity %s",
                          name.c_str(), store->entity().name.c_str());
    push_value(state, *store->get(name));
    return 1;
}

int newindex_proxy(lua_State* state) {
    auto* store = proxy(state, 1);
    if (!store || lua_type(state, 2) != LUA_TSTRING) return luaL_error(state, "property keys are strings");
    const auto name = key_of(state);
    for (const char* method : kMethodKeys)
        if (name == method) return luaL_error(state, "'%s' is a binding method and read-only", method);
    const auto* prop = declared(*store, name);
    if (!prop)
        return luaL_error(state, "property '%s' is not a declared property on entity %s",
                          name.c_str(), store->entity().name.c_str());
    EntityValue value(std::int64_t{0});
    const auto reason = convert(state, 3, prop->type, prop->of, store->contract(), &value);
    if (!reason.empty())
        return luaL_error(state, "property '%s' %s", name.c_str(), reason.c_str());
    if (!store->set(name, std::move(value)))
        return luaL_error(state, "%s", store->error().c_str());
    return 0;
}

int names_method(lua_State* state) {
    auto* store = proxy(state, 1);
    if (!store) return 0;
    const auto& props = store->entity().properties;
    lua_createtable(state, static_cast<int>(props.size()), 0);
    int at = 1;
    for (const auto& p : props) {
        lua_pushlstring(state, p.name.data(), p.name.size());
        lua_rawseti(state, -2, at++);
    }
    return 1;
}

int reserved_method(lua_State* state) {
    auto* store = proxy(state, 1);
    if (!store) return 0;
    const auto& ids = store->entity().reserved_properties;
    lua_createtable(state, static_cast<int>(ids.size()), 0);
    int at = 1;
    for (const auto id : ids) {
        lua_pushinteger(state, id);
        lua_rawseti(state, -2, at++);
    }
    return 1;
}

int view_method(lua_State* state) {
    auto* store = proxy(state, 1);
    if (!store) return 0;
    luaL_checktype(state, 2, LUA_TBOOLEAN);
    const auto view = store->client_view(lua_toboolean(state, 2) != 0);
    lua_createtable(state, 0, static_cast<int>(view.size()));
    for (const auto& [name, value] : view) {
        lua_pushlstring(state, name.data(), name.size());
        push_value(state, value);
        lua_rawset(state, -3);
    }
    return 1;
}
}

bool push_entity_properties(lua_State* state, PropertyStore* store) {
    // A declared property named like a binding method would be shadowed by
    // the method; refuse the binding instead of hiding the property.
    for (const auto& p : store->entity().properties)
        for (const char* method : kMethodKeys)
            if (p.name == method) return false;
    auto* slot = static_cast<PropertyStore**>(lua_newuserdatauv(state, sizeof(PropertyStore*), 0));
    *slot = store;
    if (luaL_newmetatable(state, kProxyMeta)) {
        lua_pushcfunction(state, index_proxy); lua_setfield(state, -2, "__index");
        lua_pushcfunction(state, newindex_proxy); lua_setfield(state, -2, "__newindex");
    }
    lua_setmetatable(state, -2);
    return true;
}
}
