// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "mistlane/entity_dispatch.hpp"
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <tuple>

namespace mistlane {
// Bounded entity host-route table for a gateway (docs/entity-contract.md,
// "host routing"): maps the target entity address to the index of the --upstream
// whose host owns it. A gateway fronting several hosts consults this before
// forwarding an MLE1 frame, so an entity request reaches the host that actually
// spawned the entity instead of the session's round-robin upstream; a miss
// leaves the frame on the session's primary upstream, where the host answers
// UNROUTED. The index names a position in the gateway's own --upstream list, so
// a route is a forwarding decision, never a socket the gateway does not own. The
// table is bounded so a hostile script cannot grow it without limit: when full,
// set refuses instead of evicting a live route (a full gateway falls back to
// round-robin and the host answers UNROUTED, never a wrong host).
class EntityHostRoutes {
public:
    static constexpr std::size_t kCapacity = 4096;
    using Key = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::int64_t>;
    static Key key(const EntityTarget& target) {
        return {target.scope, target.type_id, target.incarnation, target.entity_id};
    }
    // Records that the host at upstream index owns the entity. Re-setting the
    // same address to the same index is idempotent; a different index on an
    // already-routed address rewrites the route (the entity moved hosts). False
    // only when the table is full.
    bool set(const EntityTarget& target, std::size_t index) {
        if (by_index_.size() >= kCapacity) return false;
        by_index_[key(target)] = index;
        return true;
    }
    bool remove(const EntityTarget& target) { return by_index_.erase(key(target)) != 0; }
    // Rebuilds the table in place: a coordinator-published refresh replaces the
    // whole view so a withdrawn entity no longer routes to its old host. The
    // sessions hold the table by pointer, so the object is never swapped, only
    // cleared and refilled (all on the node's event loop).
    void clear() { by_index_.clear(); }
    std::optional<std::size_t> route(const EntityTarget& target) const {
        const auto found = by_index_.find(key(target));
        return found == by_index_.end() ? std::nullopt
                                        : std::optional<std::size_t>(found->second);
    }
    std::size_t size() const { return by_index_.size(); }
private:
    std::map<Key, std::size_t> by_index_;
};
}
