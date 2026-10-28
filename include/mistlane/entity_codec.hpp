// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "mistlane/entity_endpoint.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mistlane {
// Schema-driven sproto wire codec for entity invocations (docs/entity-contract.md,
// "Invocation envelope and error protocol"): encodes the typed arguments and
// return payloads against the contract's declared shapes and decodes the wire
// payload back to typed values. Its wire form matches the goldens locked in
// src/sproto_probe.cpp and the generated JS/C# SDK codecs. Decode intentionally
// departs from the vendored upstream sproto: INT32/INT64 4-byte elements are
// sign-extended like the generated JS/C# codecs (upstream sproto does not), and
// the typed codec enforces STRING UTF-8 well-formedness (BYTES never validates)
// — a typed-binding contract, not wire law, since upstream sproto passes raw
// bytes.
class SprotoCodec : public EntityCodec {
public:
    explicit SprotoCodec(const EntityContract& contract) : contract_(contract) {}
    bool decode_args(const ContractArgs& args, std::string_view payload, EntityValues& out, std::string& reason) const override {
        std::size_t end = 0;
        if (!decode_record(payload, 0, args, out, end, reason)) return false;
        if (end != payload.size()) { reason = "trailing bytes after record"; return false; }
        return true;
    }
    std::optional<std::string> encode_returns(const ContractArgs& returns, const EntityValues& values) const override {
        std::vector<unsigned char> bytes;
        if (!encode_record(returns, values, bytes, 0)) return std::nullopt;
        return std::string(bytes.begin(), bytes.end());
    }
private:
    enum class Kind { Int, Bool, String, Bytes, Struct,
                      IntArray, StructArray, BoolArray, StringArray, BytesArray };
    // The wire field kind plus, for struct-typed fields, the referenced
    // structure's field list (resolved once; the contract rejects recursion).
    struct Shape { Kind kind = Kind::Int; const ContractArgs* sub = nullptr; };
    Shape shape_of(const ContractField& field, const std::string& struct_name) const {
        Shape shape;
        const auto struct_args = [&]() -> const ContractArgs* {
            const auto it = contract_.structs().find(struct_name);
            return it == contract_.structs().end() ? nullptr : &it->second;
        };
        if (field.type == "ARRAY") {
            if (field.of == "BOOL") shape.kind = Kind::BoolArray;
            else if (field.of == "STRING") shape.kind = Kind::StringArray;
            else if (field.of == "BYTES") shape.kind = Kind::BytesArray;
            else if (contract_.structs().contains(field.of)) { shape.kind = Kind::StructArray; shape.sub = struct_args(); }
            else shape.kind = Kind::IntArray;
            return shape;
        }
        if (field.type == "BOOL") shape.kind = Kind::Bool;
        else if (field.type == "STRING") shape.kind = Kind::String;
        else if (field.type == "BYTES") shape.kind = Kind::Bytes;
        else if (contract_.structs().contains(field.type)) { shape.kind = Kind::Struct; shape.sub = struct_args(); }
        else shape.kind = Kind::Int;
        return shape;
    }
    // BOOL values ride the integer wire form, but the runtime's EntityValue
    // stores them as a distinct bool alternative; read either as an integer.
    static std::optional<std::int64_t> as_int(const EntityValue& value) {
        if (const auto* flag = std::get_if<bool>(&value.data)) return *flag ? 1 : 0;
        if (const auto* number = std::get_if<std::int64_t>(&value.data)) return *number;
        return std::nullopt;
    }
    static const ContractField* field_by_id(const ContractArgs& fields, unsigned id) {
        for (const auto& field : fields) if (field.id == id) return &field;
        return nullptr;
    }
    static bool valid_utf8(std::string_view payload) {
        std::size_t p = 0;
        while (p < payload.size()) {
            const auto lead = static_cast<unsigned char>(payload[p]);
            if (lead < 0x80) { ++p; continue; }
            unsigned expected = 0, minimum = 0;
            if ((lead & 0xe0) == 0xc0) { expected = 1; minimum = 0x80; }
            else if ((lead & 0xf0) == 0xe0) { expected = 2; minimum = 0x800; }
            else if ((lead & 0xf8) == 0xf0) { expected = 3; minimum = 0x10000; }
            else return false;
            if (payload.size() - p < expected + 1) return false;
            unsigned code = lead & (0x7f >> (expected + 1));
            for (unsigned k = 1; k <= expected; ++k) {
                const auto part = static_cast<unsigned char>(payload[p + k]);
                if ((part & 0xc0) != 0x80) return false;
                code = (code << 6) | (part & 0x3f);
            }
            if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
            p += expected + 1;
        }
        return true;
    }

    // ---- encode ----
    static void push_le32(std::vector<unsigned char>& out, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) { out.push_back(static_cast<unsigned char>(v & 0xff)); v >>= 8; }
    }
    bool encode_record(const ContractArgs& fields, const EntityValues& values,
                       std::vector<unsigned char>& out, unsigned depth) const {
        if (depth > 8) return false; // the contract rejects recursion; this bounds hostile inputs
        std::vector<std::pair<unsigned, const ContractField*>> ordered;
        for (const auto& field : fields) {
            if (values.find(field.name) == values.end()) return false; // every declared field must be present
            ordered.emplace_back(field.id, &field);
        }
        std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<unsigned> tags;
        std::vector<unsigned char> data;
        int last = -1;
        for (const auto& [id, field] : ordered) {
            const unsigned gap = id - static_cast<unsigned>(last) - 1;
            if (gap) tags.push_back((gap - 1) * 2 + 1);
            last = static_cast<int>(id);
            const auto& value = values.at(field->name);
            const Shape shape = shape_of(*field, field->of.empty() ? field->type : field->of);
            if (shape.kind == Kind::Int || shape.kind == Kind::Bool) {
                const auto number = as_int(value);
                if (!number) return false;
                if (shape.kind == Kind::Bool && *number != 0 && *number != 1) return false;
                if (*number >= 0 && *number < 0x7fff) {
                    tags.push_back(static_cast<unsigned>((*number + 1) * 2));
                } else {
                    tags.push_back(0);
                    if (*number >= -2147483648LL && *number <= 2147483647LL) {
                        push_le32(data, 4);
                        push_le32(data, static_cast<std::uint32_t>(static_cast<std::int32_t>(*number)));
                    } else {
                        push_le32(data, 8);
                        const auto wide = static_cast<std::uint64_t>(*number);
                        push_le32(data, static_cast<std::uint32_t>(wide & 0xffffffffu));
                        push_le32(data, static_cast<std::uint32_t>(wide >> 32));
                    }
                }
            } else {
                tags.push_back(0);
                std::vector<unsigned char> content;
                if (!encode_content(shape, value, content, depth)) return false;
                push_le32(data, static_cast<std::uint32_t>(content.size()));
                data.insert(data.end(), content.begin(), content.end());
            }
        }
        out.push_back(static_cast<unsigned char>(tags.size() & 0xff));
        out.push_back(static_cast<unsigned char>(tags.size() >> 8));
        for (const auto tag : tags) { out.push_back(static_cast<unsigned char>(tag & 0xff)); out.push_back(static_cast<unsigned char>(tag >> 8)); }
        out.insert(out.end(), data.begin(), data.end());
        return true;
    }
    bool encode_content(const Shape& shape, const EntityValue& value, std::vector<unsigned char>& content, unsigned depth) const {
        if (shape.kind == Kind::String || shape.kind == Kind::Bytes) {
            const auto* text = std::get_if<std::string>(&value.data);
            if (!text) return false;
            if (shape.kind == Kind::String && !valid_utf8(*text)) return false;
            content.insert(content.end(), text->begin(), text->end());
            return true;
        }
        if (shape.kind == Kind::Struct) {
            const auto* object = std::get_if<EntityObject>(&value.data);
            if (!object || !shape.sub) return false;
            std::vector<unsigned char> record;
            if (!encode_record(*shape.sub, *object, record, depth + 1)) return false;
            content.insert(content.end(), record.begin(), record.end());
            return true;
        }
        if (shape.kind == Kind::IntArray || shape.kind == Kind::BoolArray) {
            const auto* array = std::get_if<EntityArray>(&value.data);
            if (!array) return false;
            if (shape.kind == Kind::BoolArray) {
                for (const auto& item : *array) {
                    const auto flag = as_int(item);
                    if (!flag || (*flag != 0 && *flag != 1)) return false;
                    content.push_back(static_cast<unsigned char>(*flag));
                }
                return true;
            }
            bool wide = false;
            for (const auto& item : *array) {
                const auto number = as_int(item);
                if (!number) return false;
                if (*number < -2147483648LL || *number > 2147483647LL) wide = true;
            }
            if (!array->empty()) {
                content.push_back(static_cast<unsigned char>(wide ? 8 : 4));
                for (const auto& item : *array) {
                    const auto e = *as_int(item);
                    if (wide) for (int i = 0; i < 8; ++i) content.push_back(static_cast<unsigned char>((static_cast<std::uint64_t>(e) >> (8 * i)) & 0xff));
                    else for (int i = 0; i < 4; ++i) content.push_back(static_cast<unsigned char>((static_cast<std::uint32_t>(static_cast<std::int32_t>(e)) >> (8 * i)) & 0xff));
                }
            }
            return true;
        }
        if (shape.kind == Kind::StringArray || shape.kind == Kind::BytesArray || shape.kind == Kind::StructArray) {
            const auto* array = std::get_if<EntityArray>(&value.data);
            if (!array) return false;
            if (shape.kind == Kind::StructArray) {
                if (!shape.sub) return false;
                for (const auto& item : *array) {
                    const auto* object = std::get_if<EntityObject>(&item.data);
                    if (!object) return false;
                    std::vector<unsigned char> record;
                    if (!encode_record(*shape.sub, *object, record, depth + 1)) return false;
                    push_le32(content, static_cast<std::uint32_t>(record.size()));
                    content.insert(content.end(), record.begin(), record.end());
                }
                return true;
            }
            for (const auto& item : *array) {
                const auto* text = std::get_if<std::string>(&item.data);
                if (!text) return false;
                if (shape.kind == Kind::StringArray && !valid_utf8(*text)) return false;
                push_le32(content, static_cast<std::uint32_t>(text->size()));
                content.insert(content.end(), text->begin(), text->end());
            }
            return true;
        }
        return false;
    }

    // ---- decode ----
    static std::uint16_t read_le16(std::string_view b, std::size_t p) {
        return static_cast<std::uint16_t>(static_cast<unsigned char>(b[p]) | (static_cast<unsigned char>(b[p + 1]) << 8));
    }
    static std::uint32_t read_le32(std::string_view b, std::size_t p) {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(b[p]))
             | (static_cast<std::uint32_t>(static_cast<unsigned char>(b[p + 1])) << 8)
             | (static_cast<std::uint32_t>(static_cast<unsigned char>(b[p + 2])) << 16)
             | (static_cast<std::uint32_t>(static_cast<unsigned char>(b[p + 3])) << 24);
    }
    bool decode_record(std::string_view b, std::size_t at, const ContractArgs& fields,
                       EntityValues& out, std::size_t& end, std::string& reason) const {
        out.clear();
        if (b.size() - at < 2) { reason = "truncated tag count"; return false; }
        const unsigned count = read_le16(b, at); at += 2;
        if (b.size() - at < 2u * count) { reason = "truncated tags"; return false; }
        std::size_t tag_pos = at, data_pos = at + 2u * count;
        unsigned tag = 0xffffffff;
        for (unsigned i = 0; i < count; ++i) {
            const unsigned word = read_le16(b, tag_pos); tag_pos += 2; ++tag;
            if (word & 1) { tag += word / 2; continue; } // gap marker
            const auto* field = field_by_id(fields, tag);
            if (!field) {
                // Unknown tag skipped, mirroring upstream findtag; but the data
                // cursor must still advance past a length-prefixed unknown field
                // or every later field decodes misaligned.
                if (word == 0) {
                    if (b.size() - data_pos < 4) { reason = "truncated field length"; return false; }
                    const std::uint32_t len = read_le32(b, data_pos);
                    if (b.size() - (data_pos + 4) < len) { reason = "truncated field"; return false; }
                    data_pos += 4 + len;
                }
                continue;
            }
            const Shape shape = shape_of(*field, field->of.empty() ? field->type : field->of);
            if (word == 0) {
                if (b.size() - data_pos < 4) { reason = "truncated field length"; return false; }
                const std::uint32_t len = read_le32(b, data_pos);
                const std::size_t start = data_pos + 4;
                if (b.size() - start < len) { reason = "truncated field"; return false; }
                EntityValue value = EntityValue(static_cast<std::int64_t>(0));
                if (!decode_field(shape, b, start, len, value, reason)) return false;
                out.emplace(field->name, std::move(value));
                data_pos = start + len;
            } else {
                if (shape.kind == Kind::Bool) {
                    const auto flag = word / 2 - 1;
                    if (flag > 1) { reason = "in-band boolean out of range"; return false; }
                    out.emplace(field->name, EntityValue(flag == 1));
                } else if (shape.kind == Kind::Int) {
                    out.emplace(field->name, EntityValue(static_cast<std::int64_t>(word / 2 - 1)));
                } else { reason = "in-band non-integer field"; return false; }
            }
        }
        end = data_pos;
        return true;
    }
    bool decode_field(const Shape& shape, std::string_view b, std::size_t start, std::uint32_t len,
                      EntityValue& out, std::string& reason) const {
        if (shape.kind == Kind::Int) {
            if (len != 4 && len != 8) { reason = "integer width"; return false; }
            const std::uint32_t low = read_le32(b, start);
            std::int64_t value;
            if (len == 4) {
                value = static_cast<std::int64_t>(low);
                if (value & 0x80000000) value -= 0x100000000LL; // sign-extend 32-bit like upstream expand64
            } else {
                value = static_cast<std::int64_t>(static_cast<std::uint64_t>(low)
                    | (static_cast<std::uint64_t>(read_le32(b, start + 4)) << 32));
            }
            out = EntityValue(value);
            return true;
        }
        if (shape.kind == Kind::Bool) {
            if (len != 4 && len != 8) { reason = "boolean width"; return false; }
            const std::uint64_t wide = len == 4 ? read_le32(b, start)
                : static_cast<std::uint64_t>(read_le32(b, start)) | (static_cast<std::uint64_t>(read_le32(b, start + 4)) << 32);
            out = EntityValue(wide != 0);
            return true;
        }
        if (shape.kind == Kind::String || shape.kind == Kind::Bytes) {
            const std::string text(b.substr(start, len));
            if (shape.kind == Kind::String && !valid_utf8(text)) { reason = "invalid utf-8 string"; return false; }
            out = EntityValue(text);
            return true;
        }
        if (shape.kind == Kind::Struct) {
            if (!shape.sub) { reason = "missing struct shape"; return false; }
            EntityObject object;
            std::size_t sub_end = 0;
            if (!decode_record(b, start, *shape.sub, object, sub_end, reason) || sub_end != start + len) {
                if (reason.empty()) reason = "struct bounds";
                return false;
            }
            out = EntityValue(std::move(object));
            return true;
        }
        if (shape.kind == Kind::IntArray) {
            EntityArray array;
            if (len > 0) {
                const unsigned intlen = static_cast<unsigned char>(b[start]);
                if ((intlen != 4 && intlen != 8) || (len - 1) % intlen != 0) { reason = "int array width"; return false; }
                for (std::size_t p = start + 1; p < start + len; p += intlen) {
                    if (intlen == 4) {
                        const std::uint32_t e = read_le32(b, p);
                        std::int64_t value = static_cast<std::int64_t>(e);
                        if (value & 0x80000000) value -= 0x100000000LL; // sign-extend 32-bit like upstream expand64
                        array.emplace_back(value);
                    } else {
                        const std::uint64_t e = static_cast<std::uint64_t>(read_le32(b, p))
                            | (static_cast<std::uint64_t>(read_le32(b, p + 4)) << 32);
                        array.emplace_back(static_cast<std::int64_t>(e));
                    }
                }
            }
            out = EntityValue(std::move(array));
            return true;
        }
        if (shape.kind == Kind::BoolArray) {
            EntityArray array;
            for (std::size_t p = start; p < start + len; ++p) {
                const auto flag = static_cast<unsigned char>(b[p]);
                if (flag > 1) { reason = "bool array element"; return false; }
                array.emplace_back(flag == 1);
            }
            out = EntityValue(std::move(array));
            return true;
        }
        if (shape.kind == Kind::StringArray || shape.kind == Kind::BytesArray || shape.kind == Kind::StructArray) {
            EntityArray array;
            std::size_t p = start;
            std::uint32_t remaining = len;
            while (remaining > 0) {
                if (remaining < 4) { reason = "truncated array element length"; return false; }
                const std::uint32_t rlen = read_le32(b, p);
                if (remaining - 4 < rlen) { reason = "truncated array element"; return false; }
                if (shape.kind == Kind::StructArray) {
                    if (!shape.sub) { reason = "missing struct array shape"; return false; }
                    EntityObject object;
                    std::size_t sub_end = 0;
                    if (!decode_record(b, p + 4, *shape.sub, object, sub_end, reason) || sub_end != p + 4 + rlen) {
                        if (reason.empty()) reason = "struct array element bounds";
                        return false;
                    }
                    array.emplace_back(std::move(object));
                } else {
                    const std::string element(b.substr(p + 4, rlen));
                    if (shape.kind == Kind::StringArray && !valid_utf8(element)) { reason = "invalid utf-8 element"; return false; }
                    array.emplace_back(element);
                }
                p += 4 + rlen;
                remaining -= 4 + rlen;
            }
            out = EntityValue(std::move(array));
            return true;
        }
        reason = "unsupported field kind";
        return false;
    }
    const EntityContract& contract_;
};
}
