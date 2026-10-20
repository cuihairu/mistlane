// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "mistlane/entity_dispatch.hpp"
#include <algorithm>
#include <stdexcept>

namespace mistlane {
// One step of a location inside a property: a struct field name or an array
// position. Arrays grow only through ordered inserts; struct fields must
// already exist.
struct TreeStep {
    bool by_index = false;
    std::string field;
    std::size_t index = 0;
};
inline bool operator==(const TreeStep& a, const TreeStep& b) {
    return a.by_index == b.by_index && (a.by_index ? a.index == b.index : a.field == b.field);
}
using TreePath = std::vector<TreeStep>;
// One journal entry. SET carries the new leaf value at (property, path);
// INSERT/REMOVE carry the value/position applied to the array addressed by
// (property, path). Entries replay in application order, so a receiver
// walking the journal lands on the sender's state without rebasing.
struct TreeChange {
    enum class Kind { SET, INSERT, REMOVE };
    std::string property;
    TreePath path;
    Kind kind = Kind::SET;
    std::size_t position = 0;
    EntityValue value{std::int64_t{0}};
};
// A container handle handed out to a controlled proxy. Any structural change
// to its property (insert, remove, whole-property replacement) bumps the
// generation, so a handle from before the change is refused: old container
// handles never survive the mutation that moved members.
struct ContainerHandle {
    std::string property;
    unsigned generation = 0;
};
// Shape of the value at one location: the declared type the descriptor
// expects there, plus the element type when that type is ARRAY. ok is false
// when the location itself is not addressable (unknown field, index into a
// non-array). Controlled proxies use it to tell a scalar from a nested
// container before touching storage.
struct ValueShape {
    bool ok = false;
    std::string type, of;
};
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
        const auto* prop = descriptor(name);
        if (!prop) {
            error_ = "property '"+name+"' is not a declared property on entity "+entity_->name;
            return false;
        }
        if (!valid_shape(*prop, name, {}, value)) return false;
        values_.find(name)->second = std::move(value);
        // A whole-property replacement re-creates the subtree: it absorbs the
        // entries inside and is itself a structural change for old handles.
        record_set(name, {}, EntityValue(values_.find(name)->second));
        structural(name);
        error_.clear();
        return true;
    }
    // Read a value at a path of struct fields / array positions.
    const EntityValue* get_path(const std::string& name, const TreePath& path) const {
        const auto root = values_.find(name);
        if (root == values_.end()) { error_ = "property '"+name+"' is not a declared property on entity "+entity_->name; return nullptr; }
        const EntityValue* at = &root->second;
        for (const auto& step : path) {
            if (step.by_index) {
                const auto* array = std::get_if<EntityArray>(&at->data);
                if (!array || step.index >= array->size()) { error_ = "path index out of range"; return nullptr; }
                at = &(*array)[step.index];
            } else {
                const auto* object = std::get_if<EntityObject>(&at->data);
                if (!object) { error_ = "path field '"+step.field+"' on a non-struct"; return nullptr; }
                const auto field = object->find(step.field);
                if (field == object->end()) { error_ = "unknown struct field '"+step.field+"'"; return nullptr; }
                at = &field->second;
            }
        }
        return at;
    }
    // Shape of the value at (property, path): what the descriptor expects
    // there, without touching storage.
    ValueShape shape_at(const std::string& name, const TreePath& path) const {
        const auto* prop = descriptor(name);
        if (!prop) {
            error_ = "property '"+name+"' is not a declared property on entity "+entity_->name;
            return {};
        }
        return leaf_shape(*prop, name, path);
    }
    // Set one leaf at a path; an empty path replaces the whole property.
    bool set_path(const std::string& name, TreePath path, EntityValue value) {
        const auto* prop = descriptor(name);
        if (!prop) { error_ = "property '"+name+"' is not a declared property on entity "+entity_->name; return false; }
        if (path.empty()) return set(name, std::move(value));
        // Validate the leaf against the shape the descriptor expects there
        // before touching storage, so a refused write lands nowhere.
        const auto leaf = leaf_shape(*prop, name, path);
        if (!leaf.ok) return false;
        if (!EntityDispatcher::value_valid(leaf.type, leaf.of, value, contract_->structs())) {
            error_ = "property '"+name+"' path "+path_text(path)+" is rejected by "
                     +(leaf.type == "ARRAY" ? "ARRAY of "+leaf.of : leaf.type)+" type rules";
            return false;
        }
        EntityValue* at = &values_.find(name)->second;
        for (std::size_t i = 0; i + 1 < path.size(); ++i) at = descend(at, path[i]);
        if (!assign_child(at, path.back(), value)) {
            error_ = "property '"+name+"' path "+path_text(path)
                     +": index out of range; arrays grow through insert/append";
            return false;
        }
        record_set(name, std::move(path), EntityValue(value));
        return true;
    }
    // Insert an element before `position` in the array addressed by path
    // (position == size appends). Arrays grow only through ordered inserts.
    bool insert_at(const std::string& name, const TreePath& path, std::size_t position, EntityValue value) {
        const auto* prop = descriptor(name);
        if (!prop) { error_ = "property '"+name+"' is not a declared property on entity "+entity_->name; return false; }
        const auto array_shape = array_at(*prop, name, path);
        if (!array_shape.ok) return false;
        if (!EntityDispatcher::value_valid(array_shape.of, {}, value, contract_->structs())) {
            error_ = "property '"+name+"' path "+path_text(path)+" element is rejected by "
                     +array_shape.of+" type rules";
            return false;
        }
        auto* array = array_at_value(name, path);
        if (position > array->size()) { error_ = "insert position "+std::to_string(position)+" is beyond array size"; return false; }
        // Journal before the array takes the element: the entry must carry
        // the payload, not a moved-from husk.
        changes_.push_back(TreeChange{name, path, TreeChange::Kind::INSERT, position, EntityValue(value)});
        array->insert(array->begin()+static_cast<std::ptrdiff_t>(position), std::move(value));
        structural(name);
        return true;
    }
    bool remove_at(const std::string& name, const TreePath& path, std::size_t position) {
        const auto* prop = descriptor(name);
        if (!prop) { error_ = "property '"+name+"' is not a declared property on entity "+entity_->name; return false; }
        const auto array_shape = array_at(*prop, name, path);
        if (!array_shape.ok) return false;
        auto* array = array_at_value(name, path);
        if (position >= array->size()) { error_ = "remove position "+std::to_string(position)+" is beyond array size"; return false; }
        array->erase(array->begin()+static_cast<std::ptrdiff_t>(position));
        changes_.push_back(TreeChange{name, path, TreeChange::Kind::REMOVE, position, EntityValue(std::int64_t{0})});
        structural(name);
        return true;
    }
    // Ordered change journal since the last rebaseline/take.
    std::vector<TreeChange> take_changes() {
        std::vector<TreeChange> taken;
        taken.swap(changes_);
        return taken;
    }
    // Journal length without draining — the position a fresh consumer's
    // baseline already covers.
    std::size_t pending_changes() const { return changes_.size(); }
    void rebaseline() { changes_.clear(); }
    // Container handles die on the next structural change of their property.
    ContainerHandle handle(const std::string& name) const { return {name, generations_.count(name) ? generations_.at(name) : 0}; }
    bool valid(const ContainerHandle& handle) const {
        const auto gen = generations_.find(handle.property);
        const unsigned current = gen == generations_.end() ? 0 : gen->second;
        return values_.count(handle.property) && current == handle.generation;
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
    const ContractProperty* descriptor(const std::string& name) const {
        const auto prop = std::find_if(entity_->properties.begin(), entity_->properties.end(),
                                       [&](const ContractProperty& p) { return p.name == name; });
        return prop == entity_->properties.end() ? nullptr : &*prop;
    }
    bool valid_shape(const ContractProperty& prop, const std::string& name,
                     const TreePath& path, const EntityValue& value) {
        const auto leaf = leaf_shape(prop, name, path);
        if (!leaf.ok) return false;
        if (!EntityDispatcher::value_valid(leaf.type, leaf.of, value, contract_->structs())) {
            error_ = "property '"+name+"'"+(path.empty() ? "" : " path "+path_text(path))
                     +" is rejected by "+(leaf.type == "ARRAY" ? "ARRAY of "+leaf.of : leaf.type)+" type rules";
            return false;
        }
        return true;
    }
    // Resolve the type at (property, path), validating every step against the
    // descriptor as it descends. A wrong kind, an out-of-range-free field or
    // an index through a non-array is a refused path before any write.
    ValueShape leaf_shape(const ContractProperty& prop, const std::string& name, const TreePath& path) const {
        std::string type = prop.type, of = prop.of;
        for (std::size_t i = 0; i < path.size(); ++i) {
            const auto& step = path[i];
            const bool leaf_step = i + 1 == path.size();
            if (step.by_index) {
                if (type != "ARRAY") { error_ = "property '"+name+"' path "+path_text(path)+": index into non-array"; return {}; }
                // The parser already refuses ARRAY of ARRAY, so an indexed
                // element is a scalar or struct; its own of is irrelevant.
                type = of;
                of.clear();
            } else {
                const auto structs = contract_->structs();
                const auto spec = structs.find(type);
                if (spec == structs.end()) { error_ = "property '"+name+"' path "+path_text(path)+": field '"+step.field+"' on a non-struct"; return {}; }
                const auto field = std::find_if(spec->second.begin(), spec->second.end(),
                                                [&](const ContractField& f) { return f.name == step.field; });
                if (field == spec->second.end()) { error_ = "property '"+name+"' path "+path_text(path)+": unknown struct field '"+step.field+"'"; return {}; }
                type = field->type;
                of = field->of;
            }
            (void)leaf_step;
        }
        return ValueShape{true, type, of};
    }
    ValueShape array_at(const ContractProperty& prop, const std::string& name, const TreePath& path) const {
        ValueShape shape = leaf_shape(prop, name, path);
        if (!shape.ok) return {};
        if (shape.type != "ARRAY") { error_ = "property '"+name+"' path "+path_text(path)+" does not address an array"; return {}; }
        return shape;
    }
    EntityArray* array_at_value(const std::string& name, const TreePath& path) {
        EntityValue* at = &values_.find(name)->second;
        for (const auto& step : path) at = descend(at, step);
        return std::get_if<EntityArray>(&at->data);
    }
    static EntityValue* descend(EntityValue* at, const TreeStep& step) {
        if (step.by_index) return &std::get<EntityArray>(at->data)[step.index];
        return &std::get<EntityObject>(at->data).at(step.field);
    }
    static bool assign_child(EntityValue* at, const TreeStep& step, EntityValue value) {
        if (step.by_index) {
            auto* array = std::get_if<EntityArray>(&at->data);
            if (!array || step.index >= array->size()) return false;
            (*array)[step.index] = std::move(value);
        } else {
            auto* object = std::get_if<EntityObject>(&at->data);
            if (!object || !object->count(step.field)) return false;
            object->find(step.field)->second = std::move(value);
        }
        return true;
    }
    static std::string path_text(const TreePath& path) {
        std::string text;
        for (const auto& step : path) {
            text += step.by_index ? "["+std::to_string(step.index)+"]" : "."+step.field;
        }
        return text;
    }
    // SET journal with path merge: a whole-subtree replacement absorbs prior
    // entries inside that subtree, and two consecutive SETs at one path
    // collapse to the newer value. INSERT/REMOVE always append.
    void record_set(const std::string& name, TreePath path, EntityValue value) {
        changes_.erase(std::remove_if(changes_.begin(), changes_.end(), [&](const TreeChange& entry) {
            if (entry.property != name || entry.path.size() <= path.size()) return false;
            return std::equal(path.begin(), path.end(), entry.path.begin());
        }), changes_.end());
        if (!changes_.empty()) {
            auto& last = changes_.back();
            if (last.property == name && last.kind == TreeChange::Kind::SET && last.path == path) {
                last.value = std::move(value);
                return;
            }
        }
        changes_.push_back(TreeChange{name, std::move(path), TreeChange::Kind::SET, 0, std::move(value)});
    }
    void structural(const std::string& name) { ++generations_[name]; }
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
    std::map<std::string, unsigned> generations_;
    std::vector<TreeChange> changes_;
    mutable std::string error_;
};
}
