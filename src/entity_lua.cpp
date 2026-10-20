// SPDX-License-Identifier: Apache-2.0
#include "mistlane/entity_lua.hpp"
#include <algorithm>
#include <new>
#include <set>
#include <utility>

namespace mistlane {
namespace {
constexpr const char* kProxyMeta = "mistlane.entity.properties";
constexpr const char* kMethodKeys[] = {"names", "view", "reserved"};
constexpr const char* kArrayMethods[] = {"insert", "append", "remove"};

// One userdata is one live view of a location in the store. The root proxy
// (flat) addresses declared properties; a nested proxy addresses the struct
// field or array cell the descriptor expects at (property, path). Scalar
// reads answer values; container reads answer fresh proxies over the same
// storage, so nothing nested is ever a detached copy.
struct ProxyRef {
    PropertyStore* store = nullptr;
    std::string property;
    TreePath path;
    bool is_array = false;
    bool flat = false;
};

ProxyRef* proxy(lua_State* state, int index) {
    auto* ref = static_cast<ProxyRef*>(lua_touserdata(state, index));
    if (!ref || !lua_getmetatable(state, index)) {
        luaL_error(state, "property proxy expected");
        return nullptr;
    }
    luaL_getmetatable(state, kProxyMeta);
    const bool matches = lua_rawequal(state, -1, -2) == 1;
    lua_pop(state, 2);
    if (!matches) luaL_error(state, "property proxy expected");
    return ref;
}

bool scalar_type(const std::string& type) {
    return type == "BOOL" || type == "INT32" || type == "UINT16" || type == "UINT32" ||
           type == "INT64" || type == "EntityRef" || type == "STRING" || type == "BYTES";
}

// "property 'bag'.items[0].count" — the address an error is talking about.
std::string location(const ProxyRef& ref, const TreePath& path) {
    std::string text = "property '"+ref.property+"'";
    for (const auto& step : path)
        text += step.by_index ? "["+std::to_string(step.index)+"]" : "."+step.field;
    return text;
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
int array_insert(lua_State* state);
int array_append(lua_State* state);
int array_remove(lua_State* state);

// A fresh userdata over one location; the metatable is already registered.
void push_node(lua_State* state, PropertyStore* store, std::string property, TreePath path, bool is_array) {
    auto* ref = static_cast<ProxyRef*>(lua_newuserdatauv(state, sizeof(ProxyRef), 0));
    new (ref) ProxyRef{store, std::move(property), std::move(path), is_array, false};
    luaL_getmetatable(state, kProxyMeta);
    lua_setmetatable(state, -2);
}

int index_root(lua_State* state, ProxyRef& ref) {
    const auto name = key_of(state);
    // Binding methods answer first; push_entity_properties refuses contracts
    // that would have one shadowed by a same-named property.
    if (name == "names") { lua_pushcfunction(state, names_method); return 1; }
    if (name == "view") { lua_pushcfunction(state, view_method); return 1; }
    if (name == "reserved") { lua_pushcfunction(state, reserved_method); return 1; }
    const auto* prop = declared(*ref.store, name);
    if (!prop)
        return luaL_error(state, "property '%s' is not a declared property on entity %s",
                          name.c_str(), ref.store->entity().name.c_str());
    // Containers come back as live proxies, not copied tables.
    if (prop->type == "ARRAY") { push_node(state, ref.store, name, {}, true); return 1; }
    if (!scalar_type(prop->type)) { push_node(state, ref.store, name, {}, false); return 1; }
    push_value(state, *ref.store->get(name));
    return 1;
}

int index_array(lua_State* state, ProxyRef& ref) {
    if (lua_type(state, 2) == LUA_TSTRING) {
        const auto name = key_of(state);
        // Growth methods are the only names an array answers; array cells are
        // numbers, so these can never shadow an element.
        if (name == "insert") { lua_pushcfunction(state, array_insert); return 1; }
        if (name == "append") { lua_pushcfunction(state, array_append); return 1; }
        if (name == "remove") { lua_pushcfunction(state, array_remove); return 1; }
        return luaL_error(state, "array elements are indexed by number, not '%s'", name.c_str());
    }
    if (!lua_isinteger(state, 2) || lua_tointeger(state, 2) < 1)
        return luaL_error(state, "array indexes are 1-based");
    TreePath path = ref.path;
    path.push_back(TreeStep{true, "", static_cast<std::size_t>(lua_tointeger(state, 2) - 1)});
    const auto* value = ref.store->get_path(ref.property, path);
    if (!value) { lua_pushnil(state); return 1; }  // absent cells read as nil, like tables
    const auto shape = ref.store->shape_at(ref.property, path);
    if (shape.ok && !scalar_type(shape.type))
        push_node(state, ref.store, ref.property, std::move(path), shape.type == "ARRAY");
    else
        push_value(state, *value);
    return 1;
}

int index_struct(lua_State* state, ProxyRef& ref) {
    TreePath path = ref.path;
    path.push_back(TreeStep{false, key_of(state), 0});
    const auto shape = ref.store->shape_at(ref.property, path);
    if (!shape.ok) return luaL_error(state, "%s", ref.store->error().c_str());
    if (shape.type == "ARRAY") { push_node(state, ref.store, ref.property, std::move(path), true); return 1; }
    if (!scalar_type(shape.type)) { push_node(state, ref.store, ref.property, std::move(path), false); return 1; }
    const auto* value = ref.store->get_path(ref.property, path);
    if (!value) return luaL_error(state, "%s", ref.store->error().c_str());
    push_value(state, *value);
    return 1;
}

int index_proxy(lua_State* state) {
    auto* ref = proxy(state, 1);
    if (!ref) return 0;
    if (ref->flat) {
        if (lua_type(state, 2) != LUA_TSTRING) return luaL_error(state, "property keys are strings");
        return index_root(state, *ref);
    }
    if (ref->is_array) return index_array(state, *ref);
    if (lua_type(state, 2) != LUA_TSTRING) return luaL_error(state, "struct fields are addressed by name");
    return index_struct(state, *ref);
}

int newindex_root(lua_State* state, ProxyRef& ref) {
    const auto name = key_of(state);
    for (const char* method : kMethodKeys)
        if (name == method) return luaL_error(state, "'%s' is a binding method and read-only", method);
    const auto* prop = declared(*ref.store, name);
    if (!prop)
        return luaL_error(state, "property '%s' is not a declared property on entity %s",
                          name.c_str(), ref.store->entity().name.c_str());
    EntityValue value(std::int64_t{0});
    const auto reason = convert(state, 3, prop->type, prop->of, ref.store->contract(), &value);
    if (!reason.empty())
        return luaL_error(state, "property '%s' %s", name.c_str(), reason.c_str());
    if (!ref.store->set(name, std::move(value)))
        return luaL_error(state, "%s", ref.store->error().c_str());
    return 0;
}

int newindex_proxy(lua_State* state) {
    auto* ref = proxy(state, 1);
    if (!ref) return 0;
    if (lua_isnoneornil(state, 3))
        return luaL_error(state, "nil writes are refused; take array elements out with :remove");
    if (ref->flat) {
        if (lua_type(state, 2) != LUA_TSTRING) return luaL_error(state, "property keys are strings");
        return newindex_root(state, *ref);
    }
    TreePath path = ref->path;
    if (ref->is_array) {
        if (!lua_isinteger(state, 2) || lua_tointeger(state, 2) < 1)
            return luaL_error(state, "array elements are indexed by number, 1-based");
        path.push_back(TreeStep{true, "", static_cast<std::size_t>(lua_tointeger(state, 2) - 1)});
    } else {
        if (lua_type(state, 2) != LUA_TSTRING) return luaL_error(state, "struct fields are addressed by name");
        path.push_back(TreeStep{false, key_of(state), 0});
    }
    const auto shape = ref->store->shape_at(ref->property, path);
    if (!shape.ok) return luaL_error(state, "%s", ref->store->error().c_str());
    EntityValue value(std::int64_t{0});
    const auto reason = convert(state, 3, shape.type, shape.of, ref->store->contract(), &value);
    if (!reason.empty())
        return luaL_error(state, "%s %s", location(*ref, path).c_str(), reason.c_str());
    if (!ref->store->set_path(ref->property, std::move(path), std::move(value)))
        return luaL_error(state, "%s", ref->store->error().c_str());
    return 0;
}

// Shared tail of :insert/:append: convert one element of the array's element
// type and grow the array at an (already 0-based) position.
int array_grow(lua_State* state, ProxyRef& ref, std::size_t position, int value_at) {
    const auto shape = ref.store->shape_at(ref.property, ref.path);
    if (!shape.ok) return luaL_error(state, "%s", ref.store->error().c_str());
    EntityValue value(std::int64_t{0});
    const auto reason = convert(state, value_at, shape.of, {}, ref.store->contract(), &value);
    if (!reason.empty())
        return luaL_error(state, "%s element %s", location(ref, ref.path).c_str(), reason.c_str());
    if (!ref.store->insert_at(ref.property, ref.path, position, std::move(value)))
        return luaL_error(state, "%s", ref.store->error().c_str());
    return 0;
}

// t:insert(pos, v) inserts before the 1-based position; t:insert(v) appends.
// Arrays grow only through these explicit calls, so cell numbers stay stable
// between structural changes and the journal sees one INSERT per call.
int array_insert(lua_State* state) {
    auto* ref = proxy(state, 1);
    if (!ref) return 0;
    const auto* at = ref->store->get_path(ref->property, ref->path);
    const auto* items = at ? std::get_if<EntityArray>(&at->data) : nullptr;
    if (!items) return luaL_error(state, "%s", ref->store->error().empty()
                                                 ? "array proxy no longer addresses an array"
                                                 : ref->store->error().c_str());
    std::size_t position = items->size();  // append by default
    int value_at = 2;
    if (lua_gettop(state) >= 3) {
        if (!lua_isinteger(state, 2) || lua_tointeger(state, 2) < 1)
            return luaL_error(state, "insert position is 1-based");
        position = static_cast<std::size_t>(lua_tointeger(state, 2) - 1);
        value_at = 3;
    }
    return array_grow(state, *ref, position, value_at);
}

int array_append(lua_State* state) {
    auto* ref = proxy(state, 1);
    if (!ref) return 0;
    const auto* at = ref->store->get_path(ref->property, ref->path);
    const auto* items = at ? std::get_if<EntityArray>(&at->data) : nullptr;
    if (!items) return luaL_error(state, "%s", ref->store->error().empty()
                                                 ? "array proxy no longer addresses an array"
                                                 : ref->store->error().c_str());
    return array_grow(state, *ref, items->size(), 2);
}

// t:remove(pos) takes the 1-based element out; it answers the removed value,
// exactly like the element read that would have preceded the removal.
int array_remove(lua_State* state) {
    auto* ref = proxy(state, 1);
    if (!ref) return 0;
    if (!lua_isinteger(state, 2) || lua_tointeger(state, 2) < 1)
        return luaL_error(state, "remove position is 1-based");
    const auto position = static_cast<std::size_t>(lua_tointeger(state, 2) - 1);
    TreePath at = ref->path;
    at.push_back(TreeStep{true, "", position});
    const auto* element = ref->store->get_path(ref->property, at);
    if (!element) return luaL_error(state, "%s", ref->store->error().c_str());
    push_value(state, *element);
    if (!ref->store->remove_at(ref->property, ref->path, position))
        return luaL_error(state, "%s", ref->store->error().c_str());
    return 1;
}

int names_method(lua_State* state) {
    auto* ref = proxy(state, 1);
    if (!ref) return 0;
    const auto& props = ref->store->entity().properties;
    lua_createtable(state, static_cast<int>(props.size()), 0);
    int at = 1;
    for (const auto& p : props) {
        lua_pushlstring(state, p.name.data(), p.name.size());
        lua_rawseti(state, -2, at++);
    }
    return 1;
}

int reserved_method(lua_State* state) {
    auto* ref = proxy(state, 1);
    if (!ref) return 0;
    const auto& ids = ref->store->entity().reserved_properties;
    lua_createtable(state, static_cast<int>(ids.size()), 0);
    int at = 1;
    for (const auto id : ids) {
        lua_pushinteger(state, id);
        lua_rawseti(state, -2, at++);
    }
    return 1;
}

int view_method(lua_State* state) {
    auto* ref = proxy(state, 1);
    if (!ref) return 0;
    luaL_checktype(state, 2, LUA_TBOOLEAN);
    const auto view = ref->store->client_view(lua_toboolean(state, 2) != 0);
    lua_createtable(state, 0, static_cast<int>(view.size()));
    for (const auto& [name, value] : view) {
        lua_pushlstring(state, name.data(), name.size());
        push_value(state, value);
        lua_rawset(state, -3);
    }
    return 1;
}

// #t is the live cell count of an array proxy; struct and root proxies have
// no length.
int len_proxy(lua_State* state) {
    auto* ref = proxy(state, 1);
    if (!ref) return 0;
    if (!ref->is_array) return luaL_error(state, "length is defined on array properties only");
    const auto* at = ref->store->get_path(ref->property, ref->path);
    const auto* items = at ? std::get_if<EntityArray>(&at->data) : nullptr;
    if (!items) return luaL_error(state, "%s", ref->store->error().empty()
                                                 ? "array proxy no longer addresses an array"
                                                 : ref->store->error().c_str());
    lua_pushinteger(state, static_cast<lua_Integer>(items->size()));
    return 1;
}

int gc_proxy(lua_State* state) {
    if (auto* ref = static_cast<ProxyRef*>(lua_touserdata(state, 1))) ref->~ProxyRef();
    return 0;
}
}

bool push_entity_properties(lua_State* state, PropertyStore* store) {
    // A declared property named like a binding method would be shadowed by
    // the method; refuse the binding instead of hiding the property.
    for (const auto& p : store->entity().properties)
        for (const char* method : kMethodKeys)
            if (p.name == method) return false;
    if (luaL_newmetatable(state, kProxyMeta)) {
        lua_pushcfunction(state, index_proxy); lua_setfield(state, -2, "__index");
        lua_pushcfunction(state, newindex_proxy); lua_setfield(state, -2, "__newindex");
        lua_pushcfunction(state, len_proxy); lua_setfield(state, -2, "__len");
        lua_pushcfunction(state, gc_proxy); lua_setfield(state, -2, "__gc");
    }
    lua_pop(state, 1);
    auto* ref = static_cast<ProxyRef*>(lua_newuserdatauv(state, sizeof(ProxyRef), 0));
    new (ref) ProxyRef{store, "", {}, false, true};
    luaL_getmetatable(state, kProxyMeta);
    lua_setmetatable(state, -2);
    return true;
}
}
