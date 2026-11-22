// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "mistlane/entity_sync.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mistlane {
// Byte-level wire form for observer sync streams (docs/entity-contract.md,
// "type storage and nested sync" — the sending side of the client receiver).
// The byte accounting is the one the performance fixture already pins
// (src/test_contract.cpp, wire_bytes/delta_bytes): a u32 property id, a u16
// field id or u32 index per path step, a u8 kind, a u32 position and
// fixed-width values. There are no type tags and no per-delta sequence number
// on the wire: shapes come from the contract (so a sender and a receiver in
// different languages agree on a byte stream) and a stream is gapless, so a
// batch's head sequence is carried once.
//
//   u8  kind              0 = baseline, 1 = deltas
//   baseline:
//     u32 count, then per visible property: u32 property id, value
//   deltas:
//     u64 head sequence, u32 count, then per delta:
//       u32 property id
//       u8  path length, then per step: u8 tag (0 field / 1 index),
//           u16 field id or u32 index
//       u8  kind (0 set / 1 insert / 2 remove)
//       u32 position
//       value (absent for remove)
//   value at (type, of):
//     BOOL -> 1 byte; an integer type -> 8 bytes big-endian; STRING/BYTES ->
//     u32 length + bytes; ARRAY of X -> u32 count + X items; a struct -> per
//     declared field in id order: u16 field id + value
//
// Decoding is schema-driven and refuses on any malformed step: an unknown id,
// a field step into a non-struct, an index step into a non-array, a
// truncated value, a trailing byte or a value that cannot be encoded at its
// declared shape is a refusal with a reason, never a silent partial read.
class SyncWireCodec {
public:
    SyncWireCodec(const EntityContract& contract, std::uint32_t type_id)
        : contract_(contract), entity_(find_entity(contract, type_id)) {}

    std::optional<std::string> encode(const SyncMessage& message) const {
        if (!entity_) return std::nullopt;
        std::string out;
        out.push_back(static_cast<char>(message.kind == SyncMessage::Kind::BASELINE ? 0 : 1));
        if (message.kind == SyncMessage::Kind::BASELINE) {
            if (message.baseline.size() > kMaxCount) return std::nullopt;
            put_u32(out, static_cast<std::uint32_t>(message.baseline.size()));
            for (const auto& property : entity_->properties) {  // declaration order, ids ascend
                const auto value = message.baseline.find(property.name);
                if (value == message.baseline.end()) continue;
                put_u32(out, property.id);
                if (!put_value(out, property.type, property.of, value->second)) return std::nullopt;
            }
        } else {
            if (message.deltas.size() > kMaxCount) return std::nullopt;
            put_u64(out, message.sequence);
            put_u32(out, static_cast<std::uint32_t>(message.deltas.size()));
            for (const auto& delta : message.deltas) {
                if (delta.path.size() > kMaxPath) return std::nullopt;
                const auto* property = descriptor(delta.property_id);
                if (!property) return std::nullopt;
                put_u32(out, delta.property_id);
                out.push_back(static_cast<char>(delta.path.size()));
                const auto leaf = put_path(out, ValueShape{true, property->type, property->of}, delta.path);
                if (!leaf.ok) return std::nullopt;
                out.push_back(static_cast<char>(delta.kind == SyncDelta::Kind::SET      ? 0
                                                : delta.kind == SyncDelta::Kind::INSERT ? 1
                                                                                        : 2));
                put_u32(out, static_cast<std::uint32_t>(delta.position));
                // A slice operation (insert/remove) addresses the element of the
                // array the path ends on, so its value takes the element shape.
                if (delta.kind != SyncDelta::Kind::SET && leaf.type != "ARRAY") return std::nullopt;
                const auto value_shape = delta.kind == SyncDelta::Kind::SET
                                             ? leaf
                                             : ValueShape{true, leaf.of, ""};
                if (delta.kind != SyncDelta::Kind::REMOVE)
                    if (!put_value(out, value_shape.type, value_shape.of, delta.value)) return std::nullopt;
            }
        }
        return out;
    }

    // false plus a reason on a malformed stream. Out's deltas carry their
    // recomputed sequence (head + position), so a decoded batch is gapless.
    bool decode(std::string_view bytes, SyncMessage& out, std::string& reason) const {
        if (!entity_) { reason = "no entity descriptor"; return false; }
        std::size_t pos = 0;
        if (bytes.size() < 1) { reason = "empty stream"; return false; }
        const char kind = bytes[pos++];
        out = SyncMessage{};
        if (kind == 0) {
            out.kind = SyncMessage::Kind::BASELINE;
            std::uint32_t count = 0;
            if (!get_u32(bytes, pos, count) || count > kMaxCount) { reason = "bad baseline count"; return false; }
            for (std::uint32_t i = 0; i < count; ++i) {
                std::uint32_t property_id = 0;
                if (!get_u32(bytes, pos, property_id)) { reason = "truncated baseline property id"; return false; }
                const auto* property = descriptor(property_id);
                if (!property) { reason = "unknown baseline property id"; return false; }
                EntityValue value{std::int64_t{0}};
                if (!get_value(bytes, pos, property->type, property->of, value, reason)) return false;
                if (!out.baseline.emplace(property->name, std::move(value)).second) {
                    reason = "duplicate baseline property id";
                    return false;
                }
            }
        } else if (kind == 1) {
            out.kind = SyncMessage::Kind::DELTAS;
            std::uint64_t head = 0;
            std::uint32_t count = 0;
            if (!get_u64(bytes, pos, head) || !get_u32(bytes, pos, count) || count > kMaxCount) {
                reason = "bad delta header";
                return false;
            }
            out.sequence = head;
            for (std::uint32_t i = 0; i < count; ++i) {
                SyncDelta delta;
                if (!get_u32(bytes, pos, delta.property_id)) { reason = "truncated delta property id"; return false; }
                const auto* property = descriptor(delta.property_id);
                if (!property) { reason = "unknown delta property id"; return false; }
                if (pos >= bytes.size()) { reason = "truncated delta path length"; return false; }
                const auto path_len = static_cast<std::uint8_t>(bytes[pos++]);
                if (path_len > kMaxPath) { reason = "path too long"; return false; }
                const auto leaf = get_path(bytes, pos, ValueShape{true, property->type, property->of}, path_len,
                                           delta.path, reason);
                if (!leaf.ok) return false;
                if (pos >= bytes.size()) { reason = "truncated delta kind"; return false; }
                const auto kind_byte = static_cast<std::uint8_t>(bytes[pos++]);
                if (kind_byte > 2) { reason = "unknown delta kind"; return false; }
                delta.kind = kind_byte == 0 ? SyncDelta::Kind::SET
                            : kind_byte == 1 ? SyncDelta::Kind::INSERT
                                             : SyncDelta::Kind::REMOVE;
                std::uint32_t position = 0;
                if (!get_u32(bytes, pos, position)) { reason = "truncated delta position"; return false; }
                delta.position = position;
                if (delta.kind != SyncDelta::Kind::SET && leaf.type != "ARRAY") {
                    reason = "slice operation on a non-array";
                    return false;
                }
                const auto value_shape = delta.kind == SyncDelta::Kind::SET
                                             ? leaf
                                             : ValueShape{true, leaf.of, ""};
                if (delta.kind != SyncDelta::Kind::REMOVE)
                    if (!get_value(bytes, pos, value_shape.type, value_shape.of, delta.value, reason)) return false;
                delta.sequence = head + i;
                out.deltas.push_back(std::move(delta));
            }
        } else {
            reason = "unknown stream kind";
            return false;
        }
        if (pos != bytes.size()) { reason = "trailing bytes after stream"; return false; }
        return true;
    }

private:
    static constexpr std::uint32_t kMaxCount = 1u << 24;
    static constexpr std::size_t kMaxPath = 255;

    static const ContractEntity* find_entity(const EntityContract& contract, std::uint32_t type_id) {
        for (const auto& entity : contract.entities())
            if (entity.id == type_id) return &entity;
        return nullptr;
    }
    const ContractProperty* descriptor(std::uint32_t property_id) const {
        for (const auto& property : entity_->properties)
            if (property.id == property_id) return &property;
        return nullptr;
    }
    static const ContractField* struct_field(const EntityContract& contract, const std::string& struct_name,
                                      std::uint32_t field_id) {
        const auto spec = contract.structs().find(struct_name);
        if (spec == contract.structs().end()) return nullptr;
        for (const auto& candidate : spec->second)
            if (candidate.id == field_id) return &candidate;
        return nullptr;
    }

    static void put_u16(std::string& out, std::uint16_t v) {
        out.push_back(static_cast<char>(v >> 8));
        out.push_back(static_cast<char>(v));
    }
    static void put_u32(std::string& out, std::uint32_t v) {
        out.push_back(static_cast<char>(v >> 24));
        out.push_back(static_cast<char>(v >> 16));
        out.push_back(static_cast<char>(v >> 8));
        out.push_back(static_cast<char>(v));
    }
    static void put_u64(std::string& out, std::uint64_t v) {
        put_u32(out, static_cast<std::uint32_t>(v >> 32));
        put_u32(out, static_cast<std::uint32_t>(v));
    }
    static bool get_u16(std::string_view bytes, std::size_t& pos, std::uint16_t& v) {
        if (pos + 2 > bytes.size()) return false;
        v = static_cast<std::uint16_t>((static_cast<std::uint16_t>(static_cast<std::uint8_t>(bytes[pos])) << 8)
                                       | static_cast<std::uint16_t>(static_cast<std::uint8_t>(bytes[pos + 1])));
        pos += 2;
        return true;
    }
    static bool get_u32(std::string_view bytes, std::size_t& pos, std::uint32_t& v) {
        if (pos + 4 > bytes.size()) return false;
        v = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[pos])) << 24)
          | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[pos + 1])) << 16)
          | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[pos + 2])) << 8)
          | static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[pos + 3]));
        pos += 4;
        return true;
    }
    static bool get_u64(std::string_view bytes, std::size_t& pos, std::uint64_t& v) {
        std::uint32_t hi = 0, lo = 0;
        if (!get_u32(bytes, pos, hi) || !get_u32(bytes, pos, lo)) return false;
        v = (static_cast<std::uint64_t>(hi) << 32) | lo;
        return true;
    }

    bool put_value(std::string& out, const std::string& type, const std::string& of,
                   const EntityValue& value) const {
        if (type == "BOOL") {
            const auto* b = std::get_if<bool>(&value.data);
            if (!b) return false;
            out.push_back(*b ? 1 : 0);
            return true;
        }
        if (type == "INT32" || type == "UINT16" || type == "UINT32" || type == "INT64" || type == "EntityRef") {
            const auto* n = std::get_if<std::int64_t>(&value.data);
            if (!n) return false;
            put_u64(out, static_cast<std::uint64_t>(*n));
            return true;
        }
        if (type == "STRING" || type == "BYTES") {
            const auto* s = std::get_if<std::string>(&value.data);
            if (!s || s->size() > kMaxCount) return false;
            put_u32(out, static_cast<std::uint32_t>(s->size()));
            out += *s;
            return true;
        }
        if (type == "ARRAY") {
            const auto* items = std::get_if<EntityArray>(&value.data);
            if (!items || items->size() > kMaxCount) return false;
            put_u32(out, static_cast<std::uint32_t>(items->size()));
            for (const auto& item : *items)
                if (!put_value(out, of, "", item)) return false;
            return true;
        }
        const auto spec = contract_.structs().find(type);
        const auto* object = std::get_if<EntityObject>(&value.data);
        if (spec == contract_.structs().end() || !object || object->size() != spec->second.size()) return false;
        for (const auto& field : spec->second) {  // declaration order: ids ascend
            const auto value = object->find(field.name);
            if (value == object->end()) return false;
            put_u16(out, static_cast<std::uint16_t>(field.id));
            if (!put_value(out, field.type, field.of, value->second)) return false;
        }
        return true;
    }

    bool get_value(std::string_view bytes, std::size_t& pos, const std::string& type, const std::string& of,
                   EntityValue& value, std::string& reason) const {
        if (type == "BOOL") {
            if (pos + 1 > bytes.size()) { reason = "truncated bool"; return false; }
            value = EntityValue(bytes[pos++] != 0);
            return true;
        }
        if (type == "INT32" || type == "UINT16" || type == "UINT32" || type == "INT64" || type == "EntityRef") {
            std::uint64_t raw = 0;
            if (!get_u64(bytes, pos, raw)) { reason = "truncated integer"; return false; }
            value = EntityValue(static_cast<std::int64_t>(raw));
            return true;
        }
        if (type == "STRING" || type == "BYTES") {
            std::uint32_t len = 0;
            if (!get_u32(bytes, pos, len) || pos + len > bytes.size()) { reason = "truncated string"; return false; }
            value = EntityValue(std::string(bytes.substr(pos, len)));
            pos += len;
            return true;
        }
        if (type == "ARRAY") {
            std::uint32_t count = 0;
            if (!get_u32(bytes, pos, count) || count > kMaxCount) { reason = "bad array count"; return false; }
            EntityArray items;
            items.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                EntityValue item{std::int64_t{0}};
                if (!get_value(bytes, pos, of, "", item, reason)) return false;
                items.push_back(std::move(item));
            }
            value = EntityValue(std::move(items));
            return true;
        }
        const auto spec = contract_.structs().find(type);
        if (spec == contract_.structs().end()) { reason = "unknown struct type"; return false; }
        EntityObject object;
        for (std::size_t i = 0; i < spec->second.size(); ++i) {
            std::uint16_t field_id = 0;
            if (!get_u16(bytes, pos, field_id)) { reason = "truncated field id"; return false; }
            const auto* field = struct_field(contract_, type, field_id);
            if (!field) { reason = "unknown struct field id"; return false; }
            EntityValue member{std::int64_t{0}};
            if (!get_value(bytes, pos, field->type, field->of, member, reason)) return false;
            object.emplace(field->name, std::move(member));
        }
        if (object.size() != spec->second.size()) { reason = "struct field missing"; return false; }
        value = EntityValue(std::move(object));
        return true;
    }

    // Walk the delta path against the declared shapes, emitting each step, and
    // return the leaf shape the value must take at the end.
    ValueShape put_path(std::string& out, const ValueShape& shape, const SyncPath& path) const {
        ValueShape current = shape;
        for (const auto& step : path) {
            if (step.by_index) {
                if (current.type != "ARRAY") return ValueShape{};
                out.push_back(1);
                put_u32(out, static_cast<std::uint32_t>(step.index));
                current = ValueShape{true, current.of, ""};
            } else {
                if (current.type == "ARRAY" || current.type == "BOOL" || current.type == "INT32" ||
                    current.type == "UINT16" || current.type == "UINT32" || current.type == "INT64" ||
                    current.type == "STRING" || current.type == "BYTES" || current.type == "EntityRef")
                    return ValueShape{};
                const auto* field = struct_field(contract_, current.type, step.field_id);
                if (!field) return ValueShape{};
                out.push_back(0);
                out.push_back(static_cast<char>(step.field_id >> 8));
                out.push_back(static_cast<char>(step.field_id));
                current = ValueShape{true, field->type, field->of};
            }
        }
        return current;
    }

    ValueShape get_path(std::string_view bytes, std::size_t& pos, const ValueShape& shape,
                        std::uint8_t path_len, SyncPath& path, std::string& reason) const {
        ValueShape current = shape;
        for (std::uint8_t i = 0; i < path_len; ++i) {
            if (pos >= bytes.size()) { reason = "truncated path step tag"; return ValueShape{}; }
            const auto tag = static_cast<std::uint8_t>(bytes[pos++]);
            if (tag == 1) {
                if (current.type != "ARRAY") { reason = "index step into non-array"; return ValueShape{}; }
                std::uint32_t index = 0;
                if (!get_u32(bytes, pos, index)) { reason = "truncated index"; return ValueShape{}; }
                path.push_back(SyncStep{true, 0, index});
                current = ValueShape{true, current.of, ""};
            } else if (tag == 0) {
                if (pos + 2 > bytes.size()) { reason = "truncated field id"; return ValueShape{}; }
                const auto field_id = static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[pos])) << 8
                                    | static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[pos + 1]));
                pos += 2;
                if (current.type == "ARRAY" || current.type == "BOOL" || current.type == "INT32" ||
                    current.type == "UINT16" || current.type == "UINT32" || current.type == "INT64" ||
                    current.type == "STRING" || current.type == "BYTES" || current.type == "EntityRef") {
                    reason = "field step into non-struct";
                    return ValueShape{};
                }
                const auto* field = struct_field(contract_, current.type, field_id);
                if (!field) { reason = "unknown struct field id"; return ValueShape{}; }
                path.push_back(SyncStep{false, field_id, 0});
                current = ValueShape{true, field->type, field->of};
            } else {
                reason = "unknown path step tag";
                return ValueShape{};
            }
        }
        return current;
    }

    const EntityContract& contract_;
    const ContractEntity* entity_;
};

// One server-push property frame: MLP1 | scope | type | incarnation | entity |
// SyncWireCodec bytes. The marker keeps property traffic out of the MLE1
// invocation path (and out of plain script frames); the codec tail is the same
// self-describing bytes a receiver feeds to its property replica.
inline std::optional<std::string> property_push_frame(const EntityContract& contract,
                                                      const EntityTarget& target,
                                                      const SyncMessage& message,
                                                      std::string& reason) {
    const SyncWireCodec codec(contract, target.type_id);
    auto body = codec.encode(message);
    if (!body) { reason = "property message did not encode"; return std::nullopt; }
    const auto put = [](std::string& out, std::uint64_t v, unsigned bytes) {
        for (unsigned i = bytes; i > 0; --i)
            out.push_back(static_cast<char>((v >> ((i - 1) * 8)) & 0xFF));
    };
    std::string out = "MLP1";
    put(out, target.scope, 4);
    put(out, target.type_id, 4);
    put(out, target.incarnation, 4);
    put(out, static_cast<std::uint64_t>(target.entity_id), 8);
    out += *body;
    return out;
}

// Parse a pushed frame back into its target and message; false plus a reason on
// a non-property frame, a truncated header or a codec-level refusal.
inline bool decode_property_push(const EntityContract& contract, std::string_view frame,
                                 EntityTarget& target, SyncMessage& message, std::string& reason) {
    // The field header is scope | type | incarnation | entity (the MLP1 marker
    // precedes it); the codec tail starts right after the entity id.
    constexpr std::size_t header = 4 + 4 + 4 + 8;
    if (!frame.starts_with("MLP1")) { reason = "not a property push frame"; return false; }
    if (frame.size() < 4 + header) { reason = "property push frame is truncated"; return false; }
    const auto take = [&](std::size_t at, unsigned bytes) {
        std::uint64_t v = 0;
        for (unsigned i = 0; i < bytes; ++i) v = (v << 8) | static_cast<unsigned char>(frame[at + i]);
        return v;
    };
    target.scope = static_cast<std::uint32_t>(take(4, 4));
    target.type_id = static_cast<std::uint32_t>(take(8, 4));
    target.incarnation = static_cast<std::uint32_t>(take(12, 4));
    target.entity_id = static_cast<std::int64_t>(take(16, 8));
    const SyncWireCodec codec(contract, target.type_id);
    return codec.decode(frame.substr(4 + header), message, reason);
}
}
