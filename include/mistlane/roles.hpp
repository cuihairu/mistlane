// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <string_view>
namespace mistlane {
struct RoleDefinition { std::string_view name; bool runnable; bool script; };
inline constexpr std::array role_definitions{
    RoleDefinition{"gateway", true, false},
    RoleDefinition{"gameServer", true, true},
    RoleDefinition{"crossServer", true, true},
    RoleDefinition{"apiServer", true, true},
    RoleDefinition{"coordinator", true, false},
    RoleDefinition{"login", false, false},
    RoleDefinition{"data", false, false},
};
inline const RoleDefinition* find_role(std::string_view name) {
    for (const auto& role : role_definitions) if (role.name == name) return &role;
    return nullptr;
}
}
