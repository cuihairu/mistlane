// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "mistlane/entity_contract.hpp"
#include "mistlane/entity_sync.hpp"
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>

namespace mistlane {
// The client side of protocol coexistence (docs/entity-contract.md, "Client
// handshakes identify their compiled contract digest/version"): a host admits
// a client whose published release lock is a pure subset of its own model —
// exactly the `contract compat` direction, so the publication gate stays the
// single source of compatibility truth. An additions-only client decodes every
// frame the host sends for the ids it knows and the host projects each
// observer's property stream onto the client's declared set (SyncView), so an
// old observer on a new host never receives an id its compiled contract cannot
// decode. A client compiled against a newer lock is never admitted by an older
// host: the diff runs client -> host, so the host's missing entries are
// breaking. Unknown digests refuse the same way. Ignoring unknown fields is
// not what makes these clients compatible — the ids they share are identical
// by the gate, and the ids they lack are filtered, not ignored.
class EntityVersionGate {
public:
    explicit EntityVersionGate(const EntityContract& host)
        : digest_(host.digest()), host_(&host) {}

    // Registers one client version by its published lock text. Throws
    // ContractError on a malformed lock or on a diff with breaking lines, so a
    // misconfigured host refuses startup instead of negotiating lies later.
    void add_client_lock(std::string_view lock_text) {
        const auto entries = parse_lock(lock_text);
        const auto diff = compat_diff(entries, host_->entries());
        if (!diff.compatible())
            throw ContractError("client lock is not a subset of the host contract: "
                                + (diff.breaking.empty() ? diff.additive.front() : diff.breaking.front()));
        SyncView view;
        for (const auto& entity : host_->entities())
            for (const auto& property : entity.properties)
                if (declared(entries, "prop", entity.name, property.name, property.id)) {
                    view.ids.insert(property.id);
                    view.names.insert(property.name);
                }
        views_[lock_digest(lock_text)] = std::move(view);
    }

    bool admits(std::uint64_t digest) const {
        return digest == digest_ || views_.count(digest) != 0;
    }

    // The projection for one negotiated client digest: null for the host's own
    // version (and any unknown caller — admission is checked separately) means
    // the unfiltered host stream.
    const SyncView* view_for(std::uint64_t digest) const {
        const auto found = views_.find(digest);
        return found == views_.end() ? nullptr : &found->second;
    }

    std::uint64_t digest() const { return digest_; }

private:
    static bool declared(const std::vector<ContractEntry>& entries, std::string_view kind,
                         const std::string& owner, const std::string& name, unsigned id) {
        for (const auto& entry : entries)
            if (entry.kind == kind && entry.owner == owner && entry.name == name && entry.id == id)
                return true;
        return false;
    }
    // The lock's own "# digest <hex>" line — the identity a client presents in
    // its handshake. A lock without the line is malformed for negotiation.
    static std::uint64_t lock_digest(std::string_view text) {
        const std::string marker = "# digest ";
        std::size_t at = text.find(marker);
        if (at == std::string_view::npos)
            throw ContractError("client lock carries no digest line");
        at += marker.size();
        const auto end = text.find('\n', at);
        const auto hex = text.substr(at, end == std::string_view::npos ? std::string_view::npos : end-at);
        if (hex.size() != 16) throw ContractError("client lock digest is not 16 hex digits");
        std::uint64_t value = 0;
        for (unsigned char ch : hex) {
            unsigned digit;
            if (ch >= '0' && ch <= '9') digit = static_cast<unsigned>(ch - '0');
            else if (ch >= 'a' && ch <= 'f') digit = static_cast<unsigned>(ch - 'a' + 10);
            else throw ContractError("client lock digest is not 16 hex digits");
            value = value << 4 | digit;
        }
        return value;
    }

    std::uint64_t digest_ = 0;
    const EntityContract* host_ = nullptr;
    std::map<std::uint64_t, SyncView> views_;
};
}
