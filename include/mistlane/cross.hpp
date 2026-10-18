// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
namespace mistlane {
struct CrossContext { std::string realm, group, room; unsigned epoch = 0; };
inline CrossContext cross_context(std::string_view payload) {
    if (payload.size() > 256) throw std::invalid_argument("INVALID");
    std::istringstream in{std::string(payload)};
    std::string magic, epoch, extra;
    CrossContext c;
    if (!(in >> magic >> epoch >> c.realm >> c.group >> c.room) || (in >> extra) || magic != "MLC1")
        throw std::invalid_argument("INVALID");
    auto identifier = [](const std::string& s) {
        if (s.empty() || s.size() > 64) return false;
        for (unsigned char ch : s)
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) return false;
        return true;
    };
    if (!identifier(c.realm) || !identifier(c.group) || !identifier(c.room)) throw std::invalid_argument("INVALID");
    std::uint64_t n = 0;
    for (unsigned char ch : epoch) {
        if (ch < '0' || ch > '9') throw std::invalid_argument("INVALID");
        n = n*10 + ch-'0';
        if (n > 1000000000) throw std::invalid_argument("INVALID");
    }
    if (!n) throw std::invalid_argument("INVALID");
    c.epoch = static_cast<unsigned>(n);
    return c;
}
// FNV-1a is fixed across processes/platforms; realm intentionally does not affect room affinity.
inline std::size_t cross_shard(const CrossContext& c, std::size_t count) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char ch : c.group + "/" + c.room) { hash ^= ch; hash *= 1099511628211ULL; }
    return hash % count;
}
// Single Asio thread only. Quotas count admitted connections, not authenticated unique players.
class CrossAdmission {
public:
    CrossAdmission(std::size_t shards, std::size_t room, std::size_t group, std::size_t shard)
      : shards_(shards), room_limit_(room), group_limit_(group), shard_limit_(shard) {}
    std::string acquire(void* session, const CrossContext& c, std::size_t shard) {
        const auto room = c.group + "/" + c.room;
        auto room_it = rooms_.find(room), group_it = groups_.find(c.group);
        if (room_it != rooms_.end() && room_it->second >= room_limit_) return "ROOM_FULL";
        if (group_it != groups_.end() && group_it->second >= group_limit_) return "GROUP_FULL";
        if (shards_.at(shard) >= shard_limit_) return "SHARD_FULL";
        leases_.emplace(session, Lease{room, c.group, shard});
        ++rooms_[room]; ++groups_[c.group]; ++shards_[shard];
        return {};
    }
    void release(void* session) {
        auto it = leases_.find(session);
        if (it == leases_.end()) return;
        auto& lease = it->second;
        if (--rooms_.at(lease.room) == 0) rooms_.erase(lease.room);
        if (--groups_.at(lease.group) == 0) groups_.erase(lease.group);
        --shards_[lease.shard]; leases_.erase(it);
    }
private:
    struct Lease { std::string room, group; std::size_t shard; };
    std::vector<std::size_t> shards_;
    std::size_t room_limit_, group_limit_, shard_limit_;
    std::map<std::string, std::size_t> rooms_, groups_;
    std::map<void*, Lease> leases_;
};
}
