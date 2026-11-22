// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "mistlane/entity_envelope.hpp"
#include <optional>
#include <string>
#include <string_view>

namespace mistlane {
// Wire codec boundary between the endpoint and the schema-driven sproto
// decoder/encoder. The codec is a separate slice; until one is attached the
// endpoint serves declarations with no arguments and no returns and answers a
// typed INVALID for anything that would need the codec.
//
// This interface lives in its own header, and stays a leaf, on purpose: both
// entity_endpoint.hpp (the host side) and entity_codec.hpp (the sproto
// implementation) include it. If the interface sat in entity_endpoint.hpp,
// the implementation could not include it — the endpoint pulls in
// entity_runtime.hpp, which now owns the persistence chain, and that chain
// (entity_save.hpp, entity_state.hpp) reaches back to entity_codec.hpp. Two
// headers that include each other across the pragma-once boundary no longer
// see the declaration they derive from.
class EntityCodec {
public:
    virtual ~EntityCodec() = default;
    // Decodes the wire payload into typed arguments for the method's declared
    // argument shape; false plus a reason on malformed input, which the
    // endpoint answers as the only error code allowed to carry a reason.
    virtual bool decode_args(const ContractArgs& args, std::string_view payload,
                             EntityValues& out, std::string& reason) const = 0;
    // Encodes typed returns to the wire payload; nullopt when a declared
    // return cannot be encoded.
    virtual std::optional<std::string> encode_returns(const ContractArgs& returns,
                                                      const EntityValues& values) const = 0;
};
}
