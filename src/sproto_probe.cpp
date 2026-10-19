// SPDX-License-Identifier: Apache-2.0
#include <lua.hpp>
#include "mistlane/entity_dispatch.hpp"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

extern "C" int luaopen_sproto_core(lua_State*);
extern "C" int luaopen_lpeg(lua_State*);

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: mistlane-sproto-probe MODE SCHEMA.sproto\n";
        return 2;
    }
    lua_State* state = luaL_newstate();
    if (!state) return 2;
    luaL_openlibs(state);
    luaL_requiref(state, "lpeg", luaopen_lpeg, 1);
    lua_pop(state, 1);
    luaL_requiref(state, "sproto.core", luaopen_sproto_core, 1);
    lua_pop(state, 1);
    const std::string package_path =
        "package.path = '" MISTLANE_SPROTO_LUA_DIR "/?.lua;' "
        ".. package.path; "
        "package.cpath = '" MISTLANE_LPEG_LUA_DIR "/?.so;' .. package.cpath";
    if (luaL_dostring(state, package_path.c_str()) != LUA_OK) {
        std::cerr << lua_tostring(state, -1) << '\n';
        lua_close(state);
        return 1;
    }
    lua_pushstring(state, argv[1]); lua_setglobal(state, "MISTLANE_SPROTO_MODE");
    lua_pushstring(state, argv[2]); lua_setglobal(state, "MISTLANE_SPROTO_SCHEMA");
    const std::string script = std::string(MISTLANE_SPROTO_LUA_DIR) + "/../../examples/sproto_lua.lua";
    const int result = luaL_dofile(state, script.c_str());
    if (result != LUA_OK) std::cerr << lua_tostring(state, -1) << '\n';
    if (result == LUA_OK && std::string(argv[1]) == "sdk-vector") {
        // Independent C++ codec for standard sproto records: a count word,
        // gap-encoded tag words, then in-band or length-prefixed little-endian
        // values. Matches upstream cloudwu/sproto byte for byte for scalar
        // integers (including the 8-byte two's-complement INT64 form), struct
        // fields, integer arrays (intlen byte, 4- or 8-byte elements) and
        // arrays of structs (per-element [len][record]); booleans ride the
        // integer in-band word, STRING/BYTES are length-prefixed byte objects
        // (STRING validated as strict UTF-8), boolean arrays are one raw 0/1
        // byte per element with no intlen byte, and string/bytes arrays are
        // per-element [len][bytes]. All dword reads widen each byte through
        // uint32_t before shifting, so the upstream todword int-overflow
        // defect that corrupts negative wide integers cannot reproduce here.
        struct Value;
        using Fields = std::vector<std::pair<unsigned, Value>>;
        struct Value {
            enum class Kind { Int, Bool, String, Bytes, Struct,
                              IntArray, StructArray, BoolArray, StringArray, BytesArray } kind = Kind::Int;
            std::int64_t i = 0;
            Fields fields;                   // Struct
            std::string text;                // String / Bytes payload
            std::vector<std::int64_t> ints;  // IntArray (0/1 for BoolArray)
            std::vector<Fields> items;       // StructArray
            std::vector<std::string> strings; // StringArray / BytesArray elements
            static Value make_int(std::int64_t v) { Value x; x.kind = Kind::Int; x.i = v; return x; }
            static Value make_bool(bool flag) { Value x; x.kind = Kind::Bool; x.i = flag ? 1 : 0; return x; }
            static Value make_string(std::string text) { Value x; x.kind = Kind::String; x.text = std::move(text); return x; }
            static Value make_bytes(std::string bytes) { Value x; x.kind = Kind::Bytes; x.text = std::move(bytes); return x; }
            static Value make_struct(Fields f) { Value x; x.kind = Kind::Struct; x.fields = std::move(f); return x; }
            static Value make_int_array(std::vector<std::int64_t> v) { Value x; x.kind = Kind::IntArray; x.ints = std::move(v); return x; }
            static Value make_bool_array(std::vector<std::int64_t> v) { Value x; x.kind = Kind::BoolArray; x.ints = std::move(v); return x; }
            static Value make_struct_array(std::vector<Fields> v) { Value x; x.kind = Kind::StructArray; x.items = std::move(v); return x; }
            static Value make_string_array(std::vector<std::string> v) { Value x; x.kind = Kind::StringArray; x.strings = std::move(v); return x; }
            static Value make_bytes_array(std::vector<std::string> v) { Value x; x.kind = Kind::BytesArray; x.strings = std::move(v); return x; }
        };
        struct Encoder {
            static std::vector<unsigned char> encode(const Fields& fields) {
            std::vector<unsigned> tags;
            std::vector<unsigned char> data;
            auto push32 = [&data](std::uint32_t v) { for (int i = 0; i < 4; ++i) { data.push_back(v & 0xff); v >>= 8; } };
            int last = -1;
            for (const auto& [id, value] : fields) {
                const unsigned gap = id - last - 1;
                if (gap) tags.push_back((gap - 1) * 2 + 1);
                last = static_cast<int>(id);
                // Integers and booleans share the integer wire form: an in-band
                // short word (a boolean holds 0/1, so it always fits and never
                // takes the length-prefixed form), or a 4-/8-byte little-endian
                // value beyond that range.
                if (value.kind == Value::Kind::Int || value.kind == Value::Kind::Bool) {
                    const auto n = value.i;
                    if (n >= 0 && n < 0x7fff) {
                        tags.push_back(static_cast<unsigned>((n + 1) * 2));
                    } else {
                        tags.push_back(0);
                        if (n >= -2147483648LL && n <= 2147483647LL) {
                            push32(4); push32(static_cast<std::uint32_t>(n));
                        } else {
                            push32(8);
                            const auto wide = static_cast<std::uint64_t>(n);
                            push32(static_cast<std::uint32_t>(wide & 0xffffffffu));
                            push32(static_cast<std::uint32_t>(wide >> 32));
                        }
                    }
                } else {
                    tags.push_back(0); // length-prefixed field
                    std::vector<unsigned char> content;
                    auto c32 = [&content](std::uint32_t v) { for (int i = 0; i < 4; ++i) { content.push_back(v & 0xff); v >>= 8; } };
                    if (value.kind == Value::Kind::String || value.kind == Value::Kind::Bytes) {
                        // A string or bytes field is a length-prefixed byte object.
                        content.insert(content.end(), value.text.begin(), value.text.end());
                    } else if (value.kind == Value::Kind::Struct) {
                        const auto rec = encode(value.fields);
                        content.insert(content.end(), rec.begin(), rec.end());
                    } else if (value.kind == Value::Kind::IntArray) {
                        // intlen byte first; switch to 8-byte elements when any
                        // element needs 64 bits, sign-extending earlier slots.
                        // Zero elements emit no intlen byte at all: the content
                        // is empty, matching upstream encode_integer_array.
                        bool wide = false;
                        for (const auto e : value.ints) if (e < -2147483648LL || e > 2147483647LL) { wide = true; break; }
                        if (!value.ints.empty()) {
                            content.push_back(wide ? 8 : 4);
                            for (const auto e : value.ints) {
                                if (wide) for (int i = 0; i < 8; ++i) content.push_back((static_cast<std::uint64_t>(e) >> (8 * i)) & 0xff);
                                else for (int i = 0; i < 4; ++i) content.push_back((static_cast<std::uint32_t>(static_cast<std::int32_t>(e)) >> (8 * i)) & 0xff);
                            }
                        }
                    } else if (value.kind == Value::Kind::BoolArray) {
                        // One raw 0/1 byte per element, no intlen byte: upstream
                        // encode_array's SPROTO_TBOOLEAN path.
                        for (const auto flag : value.ints) content.push_back(static_cast<unsigned char>(flag));
                    } else if (value.kind == Value::Kind::StringArray || value.kind == Value::Kind::BytesArray) {
                        // Per-element [len][bytes], like an array of structs.
                        for (const auto& element : value.strings) {
                            c32(static_cast<std::uint32_t>(element.size()));
                            content.insert(content.end(), element.begin(), element.end());
                        }
                    } else { // StructArray
                        for (const auto& rec : value.items) {
                            const auto rec_bytes = encode(rec);
                            c32(static_cast<std::uint32_t>(rec_bytes.size()));
                            content.insert(content.end(), rec_bytes.begin(), rec_bytes.end());
                        }
                    }
                    push32(static_cast<std::uint32_t>(content.size()));
                    data.insert(data.end(), content.begin(), content.end());
                }
            }
            std::vector<unsigned char> bytes{static_cast<unsigned char>(tags.size() & 0xff), static_cast<unsigned char>(tags.size() >> 8)};
            for (const auto tag : tags) { bytes.push_back(tag & 0xff); bytes.push_back(tag >> 8); }
            bytes.insert(bytes.end(), data.begin(), data.end());
            return bytes;
            }
        };
        const auto encode = [](const Fields& fields) { return Encoder::encode(fields); };
        enum class Kind { Int, Bool, String, Bytes, Struct,
                          IntArray, StructArray, BoolArray, StringArray, BytesArray };
        struct Desc { Kind kind; const std::map<unsigned, Desc>* sub = nullptr; };
        const std::map<unsigned, Desc> item_schema = {{0, {Kind::Int}}, {1, {Kind::Int}}};
        const std::map<unsigned, Desc> inventory_schema = {{0, {Kind::Int}}, {1, {Kind::StructArray, &item_schema}}};
        const std::map<unsigned, Desc> avatar_schema = {
            {1, {Kind::Int}}, {2, {Kind::Struct, &inventory_schema}}, {3, {Kind::Int}},
            {4, {Kind::Int}}, {5, {Kind::IntArray}}, {6, {Kind::IntArray}},
            {7, {Kind::Bool}}, {8, {Kind::String}}, {9, {Kind::Bytes}},
            {10, {Kind::BoolArray}}, {11, {Kind::StringArray}}, {12, {Kind::BytesArray}},
        };
        // Strict UTF-8 well-formedness for STRING payloads (BYTES never
        // validate). Rejects overlong encodings, UTF-16 surrogate code points
        // and code points above U+10FFFF; upstream sproto passes raw bytes and
        // never validates, so this is the typed-codec contract, not wire law.
        const auto valid_utf8 = [](const std::string& payload) {
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
                if (code < minimum) return false;
                if (code >= 0xd800 && code <= 0xdfff) return false;
                p += expected + 1;
            }
            return true;
        };
        // Fail-closed recursive decode: known tags only (others are skipped,
        // mirroring upstream findtag), malformed or trailing bytes rejected.
        std::function<bool(const std::vector<unsigned char>&, unsigned, const std::map<unsigned, Desc>&, std::map<unsigned, Value>&, unsigned&)> decode_record;
        decode_record = [&](const std::vector<unsigned char>& bytes, unsigned at, const std::map<unsigned, Desc>& schema, std::map<unsigned, Value>& out, unsigned& end) -> bool {
            out.clear();
            const auto word16 = [&bytes](unsigned p) { return static_cast<unsigned>(bytes[p] | (bytes[p + 1] << 8)); };
            const auto read32 = [&bytes](unsigned p) { return static_cast<std::uint32_t>(bytes[p]) | (static_cast<std::uint32_t>(bytes[p + 1]) << 8) | (static_cast<std::uint32_t>(bytes[p + 2]) << 16) | (static_cast<std::uint32_t>(bytes[p + 3]) << 24); };
            if (bytes.size() - at < 2) return false;
            const unsigned count = word16(at);
            at += 2;
            if (bytes.size() - at < 2u * count) return false;
            unsigned tag_pos = at, data_pos = at + 2u * count, tag = 0xffffffff;
            for (unsigned i = 0; i < count; ++i) {
                const unsigned word = word16(tag_pos);
                tag_pos += 2; ++tag;
                if (word & 1) { tag += word / 2; continue; }
                const auto schema_it = schema.find(tag);
                if (schema_it == schema.end()) continue; // unknown tag skipped
                const Desc& desc = schema_it->second;
                if (word == 0) { // zero tag word: the value is length-prefixed
                    if (bytes.size() - data_pos < 4) return false;
                    const std::uint32_t len = read32(data_pos);
                    const unsigned start = data_pos + 4;
                    if (bytes.size() - start < len) return false;
                    Value value;
                    if (desc.kind == Kind::Int) {
                        if (len != 4 && len != 8) return false;
                        const std::uint64_t wide = len == 4 ? read32(start) : static_cast<std::uint64_t>(read32(start)) | (static_cast<std::uint64_t>(read32(start + 4)) << 32);
                        value = Value::make_int(static_cast<std::int64_t>(wide));
                    } else if (desc.kind == Kind::Bool) {
                        // Upstream decodes a length-prefixed boolean through the
                        // integer branch; the encoder never produces it, but a
                        // 4-/8-byte form with any set bit decodes true.
                        if (len != 4 && len != 8) return false;
                        const std::uint64_t wide = len == 4 ? read32(start) : static_cast<std::uint64_t>(read32(start)) | (static_cast<std::uint64_t>(read32(start + 4)) << 32);
                        value = Value::make_bool(wide != 0);
                    } else if (desc.kind == Kind::String || desc.kind == Kind::Bytes) {
                        const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(start);
                        const std::string payload(begin, begin + static_cast<std::ptrdiff_t>(len));
                        if (desc.kind == Kind::String && !valid_utf8(payload)) return false;
                        value = desc.kind == Kind::String ? Value::make_string(payload) : Value::make_bytes(payload);
                    } else if (desc.kind == Kind::Struct) {
                        std::map<unsigned, Value> sub;
                        unsigned sub_end;
                        if (!decode_record(bytes, start, *desc.sub, sub, sub_end) || sub_end != start + len) return false;
                        Fields fields;
                        for (const auto& [k, v] : sub) fields.emplace_back(k, v);
                        value = Value::make_struct(std::move(fields));
                    } else if (desc.kind == Kind::IntArray) {
                        // An empty array is a zero content dword (upstream
                        // encode_integer_array emits no intlen for zero
                        // elements); a lone intlen byte also decodes empty.
                        std::vector<std::int64_t> ints;
                        if (len > 0) {
                            const unsigned intlen = bytes[start];
                            if ((intlen != 4 && intlen != 8) || (len - 1) % intlen != 0) return false;
                            for (unsigned p = start + 1; p < start + len; p += intlen) {
                                const std::uint64_t e = intlen == 4 ? read32(p) : static_cast<std::uint64_t>(read32(p)) | (static_cast<std::uint64_t>(read32(p + 4)) << 32);
                                ints.push_back(static_cast<std::int64_t>(e));
                            }
                        }
                        value = Value::make_int_array(std::move(ints));
                    } else if (desc.kind == Kind::BoolArray) {
                        // One raw byte per element; only 0 and 1 are booleans.
                        std::vector<std::int64_t> flags;
                        for (unsigned p = start; p < start + len; ++p) {
                            if (bytes[p] > 1) return false;
                            flags.push_back(bytes[p]);
                        }
                        value = Value::make_bool_array(std::move(flags));
                    } else {
                        std::vector<Fields> items;
                        std::vector<std::string> strings;
                        unsigned p = start, remaining = len;
                        while (remaining > 0) {
                            if (remaining < 4) return false;
                            const std::uint32_t rlen = read32(p);
                            if (remaining - 4 < rlen) return false;
                            if (desc.kind == Kind::StringArray || desc.kind == Kind::BytesArray) {
                                const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(p + 4);
                                const std::string element(begin, begin + static_cast<std::ptrdiff_t>(rlen));
                                if (desc.kind == Kind::StringArray && !valid_utf8(element)) return false;
                                strings.push_back(std::move(element));
                            } else { // StructArray
                                std::map<unsigned, Value> sub;
                                unsigned sub_end;
                                if (!decode_record(bytes, p + 4, *desc.sub, sub, sub_end) || sub_end != p + 4 + rlen) return false;
                                Fields fields;
                                for (const auto& [k, v] : sub) fields.emplace_back(k, v);
                                items.push_back(std::move(fields));
                            }
                            p += 4 + rlen;
                            remaining -= 4 + rlen;
                        }
                        value = desc.kind == Kind::StructArray ? Value::make_struct_array(std::move(items))
                               : desc.kind == Kind::StringArray ? Value::make_string_array(std::move(strings))
                                                                  : Value::make_bytes_array(std::move(strings));
                    }
                    out[tag] = std::move(value);
                    data_pos = start + len;
                } else {
                    if (desc.kind == Kind::Int) {
                        out[tag] = Value::make_int(static_cast<std::int64_t>(word / 2 - 1));
                    } else if (desc.kind == Kind::Bool) {
                        // In-band boolean: the word is (v+1)*2, so only 2 and 4
                        // name the values false and true.
                        const auto flag = word / 2 - 1;
                        if (flag > 1) return false;
                        out[tag] = Value::make_bool(flag == 1);
                    } else return false; // other fields must be length-prefixed
                }
            }
            end = data_pos;
            return true;
        };
        const auto to_hex = [](const std::vector<unsigned char>& bytes) {
            std::ostringstream hex;
            for (const auto byte : bytes) hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
            return hex.str();
        };
        const auto one = encode({{1, Value::make_int(1)}});
        const auto negative = encode({{4, Value::make_int(-9007199254740993LL)}});
        const auto positive = encode({{1, Value::make_int(65535)}, {4, Value::make_int(9007199254740993LL)}});
        const auto nested = encode({{2, Value::make_struct({{0, Value::make_int(1)},
            {1, Value::make_struct_array({{{0, Value::make_int(7)}, {1, Value::make_int(2)}},
                                          {{0, Value::make_int(0)}, {1, Value::make_int(1)}}})}})}});
        const auto tags = encode({{1, Value::make_int(1)}, {5, Value::make_int_array({0, 1, 2147483647})}});
        const auto totals = encode({{6, Value::make_int_array({-1, 2147483648LL, 4294967296LL})}});
        const auto empty_tags = encode({{5, Value::make_int_array({})}});
        const auto empty_items = encode({{2, Value::make_struct({{0, Value::make_int(0)}, {1, Value::make_struct_array(std::vector<Fields>{})}})}});
        // BOOL/STRING/BYTES vectors, byte-locked against the vendored upstream
        // encoder (see examples/sproto_lua.lua golden mode): in-band boolean
        // words, a multi-byte UTF-8 string, binary-safe bytes, a boolean array
        // of raw 0/1 bytes, string/bytes arrays as per-element [len][bytes],
        // and the empty forms of every new shape.
        const auto text_set = encode({{7, Value::make_bool(true)},
            {8, Value::make_string("中文")},
            {9, Value::make_bytes(std::string("\0\xff", 2))},
            {10, Value::make_bool_array({1, 0, 1})},
            {11, Value::make_string_array({"latin", std::string()})},
            {12, Value::make_bytes_array({std::string("\x01\x02", 2), std::string()})}});
        const auto false_flag = encode({{7, Value::make_bool(false)}});
        const auto all_empty = encode({{8, Value::make_string(std::string())}, {9, Value::make_bytes(std::string())},
            {10, Value::make_bool_array({})}, {11, Value::make_string_array({})}, {12, Value::make_bytes_array({})}});
        if (to_hex(one) != "020001000400"
            || to_hex(negative) != "02000700000008000000ffffffffffffdfff"
            || to_hex(positive) != "0400010000000300000004000000ffff0000080000000100000000002000"
            || to_hex(nested) != "0200030000001e000000020004000000140000000600000002001000060006000000020002000400"
            || to_hex(tags) != "040001000400050000000d000000040000000001000000ffffff7f"
            || to_hex(totals) != "02000b0000001900000008ffffffffffffffff00000080000000000000000001000000"
            || to_hex(empty_tags) != "02000900000000000000"
            || to_hex(empty_items) != "0200030000000a00000002000200000000000000"
            || to_hex(text_set) != "07000d0004000000000000000000000006000000e4b8ade696870200000000ff030000000100010d000000050000006c6174696e000000000a00000002000000010200000000"
            || to_hex(false_flag) != "02000d000200"
            || to_hex(all_empty) != "06000f00000000000000000000000000000000000000000000000000000000000000") {
            std::cerr << "C++ sproto vector mismatch\n"; lua_close(state); return 1;
        }
        std::cout << "CPP_SPROTO_SDK_GOLDEN level=1 hex=" << to_hex(one) << " [PASS]\n";
        std::cout << "CPP_SPROTO_SDK_GOLDEN int64_negative=" << to_hex(negative) << " [PASS]\n";
        std::cout << "CPP_SPROTO_SDK_GOLDEN int64_positive=" << to_hex(positive) << " [PASS]\n";
        std::cout << "CPP_SPROTO_SDK_GOLDEN nested=" << to_hex(nested) << " [PASS]\n";
        std::cout << "CPP_SPROTO_SDK_GOLDEN int_array32=" << to_hex(tags) << " [PASS]\n";
        std::cout << "CPP_SPROTO_SDK_GOLDEN int_array64=" << to_hex(totals) << " [PASS]\n";
        std::cout << "CPP_SPROTO_SDK_GOLDEN empty_int_array=" << to_hex(empty_tags) << " [PASS]\n";
        std::cout << "CPP_SPROTO_SDK_GOLDEN empty_struct_array=" << to_hex(empty_items) << " [PASS]\n";
        std::cout << "CPP_SPROTO_SDK_GOLDEN bool_string_bytes=" << to_hex(text_set) << " [PASS]\n";
        std::cout << "CPP_SPROTO_SDK_GOLDEN bool_false=" << to_hex(false_flag) << " [PASS]\n";
        std::cout << "CPP_SPROTO_SDK_GOLDEN all_empty=" << to_hex(all_empty) << " [PASS]\n";
        std::map<unsigned, Value> decoded;
        const auto decode = [&](const std::vector<unsigned char>& bytes, const std::map<unsigned, Desc>& schema, std::map<unsigned, Value>& out) {
            unsigned end = 0;
            return decode_record(bytes, 0, schema, out, end) && end == bytes.size();
        };
        const bool ok =
            decode(negative, avatar_schema, decoded) && decoded.at(4).i == -9007199254740993LL
            && decode(positive, avatar_schema, decoded) && decoded.at(1).i == 65535 && decoded.at(4).i == 9007199254740993LL
            && decode(nested, avatar_schema, decoded) && decoded.at(2).kind == Value::Kind::Struct
            && decoded.at(2).fields.at(0).second.i == 1 && decoded.at(2).fields.at(1).second.kind == Value::Kind::StructArray
            && decoded.at(2).fields.at(1).second.items.size() == 2
            && decoded.at(2).fields.at(1).second.items[0].at(0).second.i == 7 && decoded.at(2).fields.at(1).second.items[0].at(1).second.i == 2
            && decoded.at(2).fields.at(1).second.items[1].at(0).second.i == 0 && decoded.at(2).fields.at(1).second.items[1].at(1).second.i == 1
            && encode({decoded.begin(), decoded.end()}) == nested
            && decode(tags, avatar_schema, decoded) && decoded.at(1).i == 1 && decoded.at(5).ints == (std::vector<std::int64_t>{0, 1, 2147483647})
            && encode({decoded.begin(), decoded.end()}) == tags
            && decode(totals, avatar_schema, decoded) && decoded.at(6).ints == (std::vector<std::int64_t>{-1, 2147483648LL, 4294967296LL})
            && encode({decoded.begin(), decoded.end()}) == totals
            && decode(text_set, avatar_schema, decoded)
            && decoded.at(7).i == 1
            && decoded.at(8).kind == Value::Kind::String && decoded.at(8).text == "中文"
            && decoded.at(9).kind == Value::Kind::Bytes && decoded.at(9).text == std::string("\0\xff", 2)
            && decoded.at(10).ints == (std::vector<std::int64_t>{1, 0, 1})
            && decoded.at(11).strings == (std::vector<std::string>{"latin", std::string()})
            && decoded.at(12).strings == (std::vector<std::string>{std::string("\x01\x02", 2), std::string()})
            && encode({decoded.begin(), decoded.end()}) == text_set
            && decode(false_flag, avatar_schema, decoded) && decoded.at(7).i == 0
            && decode(all_empty, avatar_schema, decoded) && decoded.size() == 5
            && decoded.at(8).text.empty() && decoded.at(9).text.empty()
            && decoded.at(10).ints.empty() && decoded.at(11).strings.empty() && decoded.at(12).strings.empty()
            && decode(std::vector<unsigned char>{0x02, 0x00, 0x19, 0x00, 0x02, 0x00}, avatar_schema, decoded) && decoded.empty()
            && decode(one, avatar_schema, decoded) && decoded.size() == 1 && decoded.at(1).i == 1
            && !decode(std::vector<unsigned char>{0x03, 0x00, 0x01, 0x00}, avatar_schema, decoded)
            && !decode(std::vector<unsigned char>{0x02, 0x00, 0x09, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00}, avatar_schema, decoded)
            && !decode(std::vector<unsigned char>{0x02, 0x00, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00}, avatar_schema, decoded)
            // New-shape malformations: an invalid UTF-8 STRING payload, a
            // boolean-array byte that is neither 0 nor 1, and a string-array
            // element whose length prefix overruns the array content.
            && !decode(std::vector<unsigned char>{0x02, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xff}, avatar_schema, decoded)
            && !decode(std::vector<unsigned char>{0x02, 0x00, 0x13, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02}, avatar_schema, decoded)
            && !decode(std::vector<unsigned char>{0x02, 0x00, 0x15, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x61}, avatar_schema, decoded);
        if (!ok) { std::cerr << "C++ sproto decode mismatch\n"; lua_close(state); return 1; }
        std::cout << "CPP_SPROTO_SDK_DECODE scalar+struct+arrays roundtrip unknown_tag=skipped absent=missing malformed=rejected [PASS]\n";
    }
    if (result == LUA_OK && std::string(argv[1]) == "dispatch") {
        lua_getglobal(state, "MISTLANE_DECODE_OK");
        const bool decoded_ok = lua_toboolean(state, -1) != 0;
        lua_pop(state, 1);
        lua_getglobal(state, "MISTLANE_DECODED_ACCOUNT");
        const auto account = lua_tointeger(state, -1);
        lua_pop(state, 1);
        if (!decoded_ok) { std::cerr << "upstream sproto decode marker missing\n"; lua_close(state); return 1; }
        const mistlane::EntityContract contract(R"(<Contracts><Entity name="Avatar" id="1"><ServerMethods><Method name="getProfile" id="2" mode="request" caller="server"><Arg name="account" id="0" type="UINT32"/><Returns><Field name="level" id="1" type="UINT16"/></Returns></Method></ServerMethods></Entity></Contracts>)");
        mistlane::EntityDispatcher dispatcher(contract);
        dispatcher.bind("Avatar", "server", 2, [](const auto&,const auto&,const auto&) { return mistlane::EntityValues{{"level",std::int64_t{7}}}; });
        auto response=dispatcher.dispatch({{1,1,42,3},{"server",9},"server",2,true,{{"account",std::int64_t{account}}}});
        if (!response.ok) { std::cerr << "sproto decode dispatch failed\n"; lua_close(state); return 1; }
        std::cout << "SPROTO_DECODE_DISPATCH_TYPED_RESPONSE level=" << std::get<std::int64_t>(response.response.at("level").data) << " [PASS]\n";
    }
    lua_close(state);
    return result == LUA_OK ? 0 : 1;
}
