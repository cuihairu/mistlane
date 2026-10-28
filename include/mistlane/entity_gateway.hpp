// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>

namespace mistlane {
// Bounded outer-to-inner correlation table for the gateway's entity path
// (docs/entity-contract.md, "The inner entity correlation_id is deliberately
// independent of the outer transport correlation: the gateway maps the client's
// outer correlation onto the inner entity request and back through its own
// bounded table, so one client channel can multiplex many entity requests
// without consuming transport correlation ids"). Each outbound request allocates
// a fresh inner entity correlation, remembered against the origin connection and
// outer transport correlation; the reply resolves the inner correlation back to
// that pair. The table is bounded so a stalled or hostile upstream cannot grow
// it without limit: when full, allocate refuses (the gateway answers CAPACITY
// without forwarding) instead of evicting a live request. A disconnect drops
// every pending mapping for that connection. Slots are one-shot — a reply
// answers its correlation exactly once.
class GatewayEntityCorrelation {
public:
    static constexpr std::size_t kCapacity = 4096;
    // Allocates a fresh inner correlation for an outbound request. Returns 0 —
    // never a valid entity correlation, since 0 is the notify sentinel — when
    // the table is full or the same (connection, outer) pair is already pending
    // on that channel.
    std::uint64_t allocate(std::uint64_t connection, std::uint64_t outer) {
        if (by_outer_.size() >= kCapacity) return 0;
        if (by_outer_.count(std::make_pair(connection, outer)) != 0) return 0;
        std::uint64_t inner = next_++;
        while (inner == 0 || by_inner_.count(inner) != 0) inner = next_++;
        by_inner_.emplace(inner, Entry{connection, outer});
        by_outer_.emplace(std::make_pair(connection, outer), inner);
        return inner;
    }
    // Resolves a reply's inner correlation to its origin; the slot is consumed
    // so a stale or duplicate reply is rejected rather than double-delivered.
    std::optional<std::pair<std::uint64_t, std::uint64_t>> resolve(std::uint64_t inner) {
        const auto it = by_inner_.find(inner);
        if (it == by_inner_.end()) return std::nullopt;
        const auto entry = it->second;
        by_inner_.erase(it);
        by_outer_.erase(std::make_pair(entry.connection, entry.outer));
        return std::make_pair(entry.connection, entry.outer);
    }
    // Drops every pending mapping for a disconnected client connection.
    void drop(std::uint64_t connection) {
        for (auto it = by_outer_.begin(); it != by_outer_.end();) {
            if (it->first.first == connection) {
                by_inner_.erase(it->second);
                it = by_outer_.erase(it);
            } else ++it;
        }
    }
    std::size_t pending() const { return by_inner_.size(); }
private:
    struct Entry { std::uint64_t connection, outer; };
    std::map<std::uint64_t, Entry> by_inner_;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> by_outer_;
    std::uint64_t next_ = 1;
};
}
