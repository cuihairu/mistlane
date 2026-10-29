// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <lua.hpp>
#include "mistlane/entity_property.hpp"

namespace mistlane {
// Push a table-style Lua proxy for one PropertyStore: reading a declared
// property maps the value to Lua kinds, writing validates through the same
// store (and therefore the same wire type rules), and unknown or method-key
// names raise errors instead of silently creating fields. Struct and array
// properties read back as live nested proxies over the same storage —
// props.bag.count and props.inventory.items[1] address the store, never a
// detached copy — and arrays change size only through the explicit
// :insert/:append/:remove methods (1-based positions, like Lua); a nil
// write or a string key on an array is a refused error. The read-only
// method keys names()/view(is_owner)/reserved() expose the descriptor
// surface. Returns false without pushing anything when a declared property
// name collides with a method key — the refusal happens at push time, never
// as silent shadowing. The proxy borrows the store: the owner must outlive
// the Lua side's use of it.
bool push_entity_properties(lua_State* state, PropertyStore* store);

// One EntityValue becomes one Lua value (scalars keep their kind; arrays and
// structs become plain tables, arrays 1-based). Used by the property proxy and
// by entity method arguments.
void push_entity_value(lua_State* state, const EntityValue& value);

// One Lua value becomes one EntityValue of the declared shape (type/of against
// the contract's struct table). The empty string means success; any other
// string is a refusal reason the caller prefixes. A wrong Lua kind never
// coerces: a number never becomes a string field, a non-integer never rounds.
std::string convert_entity_value(lua_State* state, int index, const std::string& type,
                                 const std::string& of, const EntityContract& contract,
                                 EntityValue* out);
}
