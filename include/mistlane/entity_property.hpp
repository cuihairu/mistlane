// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "mistlane/entity_dispatch.hpp"
#include <algorithm>
#include <stdexcept>

namespace mistlane {
// One instance's controlled property store, driven entirely by the contract
// descriptor. Values are addressed by declared name (and by the stable
// property id); every write passes the same type rules as EntityDispatcher,
// so a value that could never ride the wire can never be stored. Retired
// property ids from the descriptor's Reserved group stay addressable but
// read-only: a defined refusal instead of a live slot. All property access
// for an entity goes through this gate — no business code holds the map.
class PropertyStore {
public:
    PropertyStore(const EntityContract& contract, std::uint32_t type_id) {
        contract_ = &contract;
        const auto entity = std::find_if(contract.entities().begin(), contract.entities().end(),
                                         [&](const ContractEntity& e) { return e.id == type_id; });
        if (entity == contract.entities().end())
            throw std::invalid_argument("entity type id "+std::to_string(type_id)+" is not in the contract");
        entity_ = &*entity;
        for (const auto& p : entity_->properties) {
            values_.emplace(p.name, default_value(p.type, p.of, p.default_value));
            ids_.emplace(p.id, p.name);
        }
    }
    const ContractEntity& entity() const { return *entity_; }
    const EntityContract& contract() const { return *contract_; }
    const EntityValue* get(const std::string& name) const {
        const auto it = values_.find(name);
        return it == values_.end() ? nullptr : &it->second;
    }
    bool set(const std::string& name, EntityValue value) {
        const auto prop = std::find_if(entity_->properties.begin(), entity_->properties.end(),
                                       [&](const ContractProperty& p) { return p.name == name; });
        if (prop == entity_->properties.end()) {
            error_ = "property '"+name+"' is not a declared property on entity "+entity_->name;
            return false;
        }
        if (!EntityDispatcher::value_valid(prop->type, prop->of, value, contract_->structs())) {
            error_ = "property '"+name+"' is rejected by "
                     +(prop->type == "ARRAY" ? "ARRAY of "+prop->of : prop->type)+" type rules";
            return false;
        }
        values_.find(name)->second = std::move(value);
        error_.clear();
        return true;
    }
    const EntityValue* by_id(std::uint32_t id) const {
        const auto it = ids_.find(id);
        return it == ids_.end() ? nullptr : get(it->second);
    }
    bool set_by_id(std::uint32_t id, EntityValue value) {
        if (reserved(id)) {
            error_ = "property id "+std::to_string(id)+" is reserved and read-only";
            return false;
        }
        const auto it = ids_.find(id);
        if (it == ids_.end()) {
            error_ = "property id "+std::to_string(id)+" is not a declared property on entity "+entity_->name;
            return false;
        }
        return set(it->second, std::move(value));
    }
    // Read-only reserved proxy: a retired id answers true here, serves no
    // value and refuses every write.
    bool reserved(std::uint32_t id) const {
        return std::find(entity_->reserved_properties.begin(), entity_->reserved_properties.end(), id)
               != entity_->reserved_properties.end();
    }
    static bool client_visible(const ContractProperty& p, bool is_owner) {
        return p.flags == "ALL_CLIENTS" || (is_owner && p.flags == "OWN_CLIENT");
    }
    EntityValues snapshot() const { return values_; }
    // Client audience snapshot: OWN_CLIENT+ALL_CLIENTS for the owner,
    // ALL_CLIENTS for everyone else, SERVER_ONLY never leaves the server.
    EntityValues client_view(bool is_owner) const {
        EntityValues view;
        for (const auto& p : entity_->properties)
            if (client_visible(p, is_owner)) view.emplace(p.name, values_.find(p.name)->second);
        return view;
    }
    const std::string& error() const { return error_; }
private:
    // Missing defaults follow the type rules: zero/false/empty string, empty
    // array and per-field structure initialization; explicit defaults
    // (already validated by the contract parser) override them.
    EntityValue default_value(const std::string& type, const std::string& of, const std::string& def) const {
        if (type == "BOOL") return EntityValue(def == "true");
        if (type == "INT32" || type == "UINT16" || type == "UINT32" || type == "INT64" || type == "EntityRef") {
            if (def.empty()) return EntityValue(std::int64_t{0});
            return EntityValue(static_cast<std::int64_t>(std::stoll(def)));
        }
        if (type == "STRING" || type == "BYTES") return EntityValue(def);
        if (type == "ARRAY") { (void)of; return EntityValue(EntityArray{}); }
        const auto spec = contract_->structs().find(type);
        if (spec == contract_->structs().end()) return EntityValue(EntityObject{});
        EntityObject object;
        for (const auto& f : spec->second)
            object.emplace(f.name, default_value(f.type, f.of, f.default_value));
        return EntityValue(std::move(object));
    }
    const EntityContract* contract_ = nullptr;
    const ContractEntity* entity_ = nullptr;
    EntityValues values_;
    std::map<std::uint32_t, std::string> ids_;
    std::string error_;
};
}
