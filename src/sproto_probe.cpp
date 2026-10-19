// SPDX-License-Identifier: Apache-2.0
#include <lua.hpp>
#include "mistlane/entity_dispatch.hpp"
#include <algorithm>
#include <cstdint>
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
        // Independent C++ codec for standard sproto scalar records: a count
        // word, gap-encoded tag words, then in-band or length-prefixed
        // little-endian values. Encoding matches upstream cloudwu/sproto byte
        // for byte (including the 8-byte two's-complement INT64 form); decode
        // keeps the dword math unsigned, so the upstream todword int-overflow
        // defect that corrupts negative wide integers cannot reproduce here.
        const auto encode = [](const std::vector<std::pair<unsigned, std::int64_t>>& fields) {
            std::vector<unsigned> tags;
            std::vector<unsigned char> data;
            auto push32 = [&data](std::uint32_t v) { for (int i = 0; i < 4; ++i) { data.push_back(v & 0xff); v >>= 8; } };
            int last = -1;
            for (const auto& [id, n] : fields) {
                const unsigned gap = id - last - 1;
                if (gap) tags.push_back((gap - 1) * 2 + 1);
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
                last = static_cast<int>(id);
            }
            std::vector<unsigned char> bytes{static_cast<unsigned char>(tags.size() & 0xff), static_cast<unsigned char>(tags.size() >> 8)};
            for (const auto tag : tags) { bytes.push_back(tag & 0xff); bytes.push_back(tag >> 8); }
            bytes.insert(bytes.end(), data.begin(), data.end());
            return bytes;
        };
        // Fail-closed decode: known tags only (others are skipped, mirroring
        // upstream findtag), malformed buffers rejected. Dword reads widen
        // each byte through uint32_t before shifting: the low dword of a wide
        // integer may carry its sign bit, and int arithmetic here would
        // re-create the upstream todword sign-extension defect.
        const auto decode = [](const std::vector<unsigned char>& bytes, const std::vector<unsigned>& known, std::map<unsigned, std::int64_t>& out) {
            out.clear();
            if (bytes.size() < 2) return false;
            const auto word16 = [&bytes](unsigned at) { return static_cast<unsigned>(bytes[at] | (bytes[at + 1] << 8)); };
            const auto read32 = [&bytes](unsigned at) { return static_cast<std::uint32_t>(bytes[at]) | (static_cast<std::uint32_t>(bytes[at+1]) << 8) | (static_cast<std::uint32_t>(bytes[at+2]) << 16) | (static_cast<std::uint32_t>(bytes[at+3]) << 24); };
            const unsigned count = word16(0);
            if (bytes.size() < 2 + 2u * count) return false;
            // Tag words and the data section advance on separate cursors: only
            // length-prefixed values consume data bytes, so an in-band word
            // never moves the data cursor (upstream sproto_decode layout).
            unsigned tag_pos = 2, data_pos = 2 + 2u * count, tag = 0xffffffff;
            for (unsigned i = 0; i < count; ++i) {
                const unsigned word = word16(tag_pos);
                tag_pos += 2; ++tag;
                if (word & 1) { tag += word / 2; continue; }
                std::uint64_t v = word / 2 - 1;
                if (word == 0) { // zero tag word: the value is length-prefixed
                    if (bytes.size() - data_pos < 4) return false;
                    const std::uint32_t len = read32(data_pos);
                    if (bytes.size() - data_pos - 4 < len || (len != 4 && len != 8)) return false;
                    v = len == 4 ? read32(data_pos + 4) : static_cast<std::uint64_t>(read32(data_pos + 4)) | (static_cast<std::uint64_t>(read32(data_pos + 8)) << 32);
                    data_pos += 4 + len;
                }
                if (std::find(known.begin(), known.end(), tag) != known.end()) out[tag] = static_cast<std::int64_t>(v);
            }
            return data_pos == bytes.size();
        };
        const auto to_hex = [](const std::vector<unsigned char>& bytes) {
            std::ostringstream hex;
            for (const auto byte : bytes) hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
            return hex.str();
        };
        const std::vector<unsigned> avatar{1, 2, 3, 4};
        const auto one = encode({{1, 1}});
        const auto negative = encode({{4, -9007199254740993LL}});
        const auto positive = encode({{1, 65535}, {4, 9007199254740993LL}});
        if (to_hex(one) != "020001000400"
            || to_hex(negative) != "02000700000008000000ffffffffffffdfff"
            || to_hex(positive) != "0400010000000300000004000000ffff0000080000000100000000002000") {
            std::cerr << "C++ sproto vector mismatch\n"; lua_close(state); return 1;
        }
        std::cout << "CPP_SPROTO_SDK_GOLDEN level=1 hex=" << to_hex(one) << " [PASS]\n";
        std::cout << "CPP_SPROTO_SDK_GOLDEN int64_negative=" << to_hex(negative) << " [PASS]\n";
        std::cout << "CPP_SPROTO_SDK_GOLDEN int64_positive=" << to_hex(positive) << " [PASS]\n";
        std::map<unsigned, std::int64_t> decoded;
        if (!decode(negative, avatar, decoded) || decoded.at(4) != -9007199254740993LL
            || !decode(positive, avatar, decoded) || decoded.at(1) != 65535 || decoded.at(4) != 9007199254740993LL
            || !decode(std::vector<unsigned char>{0x02, 0x00, 0x0b, 0x00, 0x02, 0x00}, avatar, decoded) || !decoded.empty()
            || decode(std::vector<unsigned char>{0x03, 0x00, 0x01, 0x00}, avatar, decoded)) {
            std::cerr << "C++ sproto decode mismatch\n"; lua_close(state); return 1;
        }
        std::cout << "CPP_SPROTO_SDK_DECODE int64 negative=-9007199254740993 positive=9007199254740993 unknown_tag=skipped malformed=rejected [PASS]\n";
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
