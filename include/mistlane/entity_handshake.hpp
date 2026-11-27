// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mistlane {
// Session-level contract negotiation (docs/entity-contract.md). The frame is
// sent once before the first business frame on an entity host's session and
// leaves the 39-byte MLE1 envelope, the reply codec and the MLP1 push bytes
// untouched, so every existing client and golden vector keeps working
// verbatim:
//   "MLV1 <16 lowercase hex client digest>"
//   "MLV1 OK <16 lowercase hex host digest>"
//   "MLV1 ERR INVALID|VERSION|LATE"
constexpr std::string_view kEntityHandshakePrefix = "MLV1 ";

inline std::optional<std::uint64_t> parse_entity_handshake(std::string_view frame) {
    if (!frame.starts_with(kEntityHandshakePrefix)) return std::nullopt;
    const auto hex = frame.substr(kEntityHandshakePrefix.size());
    if (hex.size() != 16) return std::nullopt;
    std::uint64_t value = 0;
    for (unsigned char ch : hex) {
        unsigned digit;
        if (ch >= '0' && ch <= '9') digit = static_cast<unsigned>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') digit = static_cast<unsigned>(ch - 'a' + 10);
        else return std::nullopt;
        value = value << 4 | digit;
    }
    return value;
}

inline std::string entity_handshake_ok(std::uint64_t digest) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out = "MLV1 OK ";
    for (int shift = 60; shift >= 0; shift -= 4) out.push_back(digits[(digest >> shift) & 15]);
    return out;
}

inline std::string entity_handshake_error(std::string_view reason) {
    return "MLV1 ERR " + std::string(reason);
}
}
