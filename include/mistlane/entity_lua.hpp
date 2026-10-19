// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <lua.hpp>
#include "mistlane/entity_property.hpp"

namespace mistlane {
// Push a table-style Lua proxy for one PropertyStore: reading a declared
// property maps the value to Lua kinds, writing validates through the same
// store (and therefore the same wire type rules), and unknown or method-key
// names raise errors instead of silently creating fields. The read-only
// method keys names()/view(is_owner)/reserved() expose the descriptor
// surface. Returns false without pushing anything when a declared property
// name collides with a method key — the refusal happens at push time, never
// as silent shadowing. The proxy borrows the store: the owner must outlive
// the Lua side's use of it.
bool push_entity_properties(lua_State* state, PropertyStore* store);
}
