-- SPDX-License-Identifier: Apache-2.0
-- Exercise the vendored upstream cloudwu/sproto Lua API against generated schema.
-- Run after CMake build:
--   build/debug/mistlane-sproto-probe roundtrip build/debug/sproto-fixture/avatar.sproto
local file = assert(io.open(MISTLANE_SPROTO_SCHEMA, "rb"))
local schema_text = file:read("*a")
file:close()

local parser = require "sprotoparser"
local sproto = require "sproto"
local packed_schema = parser.parse(schema_text)
local sp = sproto.new(packed_schema)
assert(sp:exist_type("Avatar"), "generated Avatar type missing")

local function hex(bytes)
  return (bytes:gsub(".", function(c) return string.format("%02x", c:byte()) end))
end

local full = {
  level = 65535,
  score = 2147483647,
  inventory = {
    capacity = 65535,
    items = {
      { itemId = 0, count = 0 },
      { itemId = 2147483647, count = 1 },
    },
  },
}
local empty = { level = 0, inventory = { capacity = 0, items = {} } }
-- INT64 vectors: a negative beyond 2^53 (unrepresentable as a JS Number) and a
-- positive 2^53+1, both exercising the 64-bit integer wire form.
local wide_negative = { exp = -9007199254740993 }
local wide_positive = { level = 65535, exp = 9007199254740993 }
-- Struct + array vectors for the nested-structure slice. `nested` carries only a
-- struct field (whose items array is an array of structs), `tags32` an array of
-- 32-bit integers, `totals64` an array of 64-bit integers whose first element
-- needs only 32 bits so the encoder must sign-extend the padding of the
-- 64-bit element slots.
local nested = { inventory = { capacity = 1, items = { { itemId = 7, count = 2 }, { itemId = 0, count = 1 } } } }
local tags32 = { level = 1, tags = { 0, 1, 2147483647 } }
local totals64 = { totals = { -1, 2147483648, 4294967296 } }
-- Empty-array edge: upstream encode_integer_array/encode_array_object emit a
-- zero content dword for zero elements (no intlen byte at all), and the Lua
-- binding reports an empty table as a present-empty array, not as absent.
local empty_tags = { tags = {} }
local empty_items = { inventory = { capacity = 0, items = {} } }

if MISTLANE_SPROTO_MODE == "parser" then
  print("UPSTREAM_SCHEMA_PARSE_OK Avatar sproto-bytes=" .. #packed_schema)
elseif MISTLANE_SPROTO_MODE == "golden" then
  local full_bytes = sp:encode("Avatar", full)
  local empty_bytes = sp:encode("Avatar", empty)
  local negative_bytes = sp:encode("Avatar", wide_negative)
  local positive_bytes = sp:encode("Avatar", wide_positive)
  print("STANDARD_SPROTO_GOLDEN full=" .. hex(full_bytes))
  print("STANDARD_SPROTO_GOLDEN empty=" .. hex(empty_bytes))
  print("STANDARD_SPROTO_GOLDEN int64_negative=" .. hex(negative_bytes))
  print("STANDARD_SPROTO_GOLDEN int64_positive=" .. hex(positive_bytes))
  local nested_bytes = sp:encode("Avatar", nested)
  local tags_bytes = sp:encode("Avatar", tags32)
  local totals_bytes = sp:encode("Avatar", totals64)
  print("STANDARD_SPROTO_GOLDEN nested=" .. hex(nested_bytes))
  print("STANDARD_SPROTO_GOLDEN int_array32=" .. hex(tags_bytes))
  print("STANDARD_SPROTO_GOLDEN int_array64=" .. hex(totals_bytes))
  local empty_tags_bytes = sp:encode("Avatar", empty_tags)
  local empty_items_bytes = sp:encode("Avatar", empty_items)
  print("STANDARD_SPROTO_GOLDEN empty_int_array=" .. hex(empty_tags_bytes))
  print("STANDARD_SPROTO_GOLDEN empty_struct_array=" .. hex(empty_items_bytes))
  assert(hex(full_bytes) == "0400010000000000000004000000ffff00002e00000002000000000004000000ffff00001c000000060000000200020002000e00000002000000040004000000ffffff7f04000000ffffff7f",
    "full Avatar standard sproto golden changed")
  assert(hex(empty_bytes) == "03000100020000000a00000002000200000000000000",
    "empty Avatar standard sproto golden changed")
  assert(hex(negative_bytes) == "02000700000008000000ffffffffffffdfff",
    "negative INT64 standard sproto golden changed")
  assert(hex(positive_bytes) == "0400010000000300000004000000ffff0000080000000100000000002000",
    "positive INT64 standard sproto golden changed")
  -- Nested struct + array vectors: a struct field whose items is an array of
  -- structs, an array of 32-bit integers, and an array of 64-bit integers whose
  -- first element needs only 32 bits so the encoder sign-extends the padding of
  -- the earlier 8-byte-element slots (intlen byte 0x08).
  assert(hex(nested_bytes) == "0200030000001e000000020004000000140000000600000002001000060006000000020002000400",
    "nested struct standard sproto golden changed")
  assert(hex(tags_bytes) == "040001000400050000000d000000040000000001000000ffffff7f",
    "32-bit integer array standard sproto golden changed")
  assert(hex(totals_bytes) == "02000b0000001900000008ffffffffffffffff00000080000000000000000001000000",
    "64-bit integer array standard sproto golden changed")
  assert(hex(empty_tags_bytes) == "02000900000000000000",
    "empty integer array standard sproto golden changed")
  assert(hex(empty_items_bytes) == "0200030000000a00000002000200000000000000",
    "empty struct array standard sproto golden changed")
  -- Absent fields are simply not decoded: sproto has no wire defaults, so the
  -- contract's definition-time defaults stay an application concern.
  local without_exp = sp:decode("Avatar", sp:encode("Avatar", { level = 1 }))
  assert(without_exp.exp == nil and without_exp.level == 1, "absent INT64 did not decode as missing")
  print("STANDARD_SPROTO_GOLDEN defaults: absent fields decode missing [PASS]")
  -- A wire tag outside the schema is skipped, never misdelivered (upstream
  -- sproto.c findtag miss continues): count 2, a gap word landing on tag 6
  -- (past totals id 6), then an in-band word that decodes into no field.
  local unknown = sp:decode("Avatar", "\x02\x00\x0d\x00\x02\x00")
  assert(unknown ~= nil and next(unknown) == nil, "unknown wire tag was not skipped")
  print("STANDARD_SPROTO_GOLDEN unknown_tag: skipped, nothing decoded [PASS]")
  -- Locked upstream decoder defect, reproduced not hidden: todword() builds a
  -- dword with int arithmetic, so a low dword whose top byte has the sign bit
  -- set turns negative and sign-extends to size_t. ORing that with hi<<32 pins
  -- bits 32-63 to all ones, so any INT64 with bit 31 of its low 32 bits set
  -- decodes wrong. The encoder is honest; only the decoder is broken, and the
  -- bug is live on upstream master as of 2026-10-20 (same todword).
  -- The assert pins the return so a future vendored fix flips it loudly.
  assert(sp:decode("Avatar", negative_bytes).exp == -1,
    "upstream negative INT64 decode behaviour changed")
  print("STANDARD_SPROTO_GOLDEN int64_negative decode=-1 (upstream todword defect, locked) [PASS]")
  -- Malformed inputs fail closed: short header, short tag block, truncated
  -- length-prefixed value.
  for index, broken in ipairs({ "", full_bytes:sub(1, 3), full_bytes:sub(1, #full_bytes - 1), "\x03\x00\x01\x00" }) do
    local ok, result = pcall(sp.decode, sp, "Avatar", broken)
    assert(not ok or result == nil, "malformed buffer " .. index .. " decoded")
  end
  print("STANDARD_SPROTO_GOLDEN malformed: short/truncated buffers decode nil [PASS]")
elseif MISTLANE_SPROTO_MODE == "sdk-vector" then
  local bytes = sp:encode("Avatar", { level = 1 })
  print("CPP_SPROTO_SDK_GOLDEN level=1 hex=" .. hex(bytes))
  assert(hex(bytes) == "020001000400", "SDK scalar vector changed")
elseif MISTLANE_SPROTO_MODE == "roundtrip" then
  local bytes = sp:encode("Avatar", full)
  local decoded = sp:decode("Avatar", bytes)
  assert(decoded.level == full.level)
  assert(decoded.score == full.score)
  assert(decoded.inventory.capacity == full.inventory.capacity)
  assert(#decoded.inventory.items == 2)
  assert(decoded.inventory.items[1].itemId == 0 and decoded.inventory.items[1].count == 0)
  assert(decoded.inventory.items[2].itemId == 2147483647 and decoded.inventory.items[2].count == 1)
  assert(sp:encode("Avatar", decoded) == bytes)
  -- Only the positive vector round trips here: the upstream decoder defect
  -- (todword int arithmetic, see the golden mode) breaks decode of INT64
  -- values whose low 32 bits have bit 31 set, so wide_negative round trips
  -- only through mistlane's independent codecs, not through vendored sproto.
  for _, wide in ipairs({ wide_positive }) do
    local wide_bytes = sp:encode("Avatar", wide)
    local wide_decoded = sp:decode("Avatar", wide_bytes)
    assert(wide_decoded.exp == wide.exp, "INT64 value did not round trip exactly")
    assert(sp:encode("Avatar", wide_decoded) == wide_bytes, "INT64 re-encode changed the wire bytes")
  end
  -- Struct + integer-array vectors round trip through vendored sproto too: every
  -- value here is small enough that the decoder's int-arithmetic todword()
  -- (see golden mode) never sign-extends. totals64 stays out of this list on
  -- purpose: its 2147483648 element has the 32nd low-dword bit set, so the
  -- upstream decoder misreads it exactly like wide_negative.
  for name, vec in pairs({ nested = nested, tags32 = tags32, empty_tags = empty_tags }) do
    local vec_bytes = sp:encode("Avatar", vec)
    local vec_decoded = sp:decode("Avatar", vec_bytes)
    assert(sp:encode("Avatar", vec_decoded) == vec_bytes, name .. " array/struct vector did not round trip")
  end
  assert(sp:decode("Avatar", sp:encode("Avatar", nested)).inventory.items[1].itemId == 7
    and sp:decode("Avatar", sp:encode("Avatar", nested)).inventory.items[2].count == 1
    and sp:decode("Avatar", sp:encode("Avatar", tags32)).tags[3] == 2147483647,
    "array/struct values did not decode to the right fields")
  print("LUA_SPROTO_ROUNDTRIP_OK encode→decode→encode bytes=" .. #bytes .. " int64=" .. #sp:encode("Avatar", wide_negative) .. "/" .. #sp:encode("Avatar", wide_positive) .. " struct=" .. #sp:encode("Avatar", nested) .. " arrays=" .. #sp:encode("Avatar", tags32) .. "/" .. #sp:encode("Avatar", totals64))
elseif MISTLANE_SPROTO_MODE == "dispatch" then
  local bytes = sp:request_encode("E1_Server_2", { account = 1234 })
  local decoded = sp:request_decode("E1_Server_2", bytes)
  assert(decoded and decoded.account == 1234, "upstream request decode lost account field")
  _G.MISTLANE_DECODED_ACCOUNT = decoded.account
  _G.MISTLANE_DECODE_OK = true
  print("UPSTREAM_SPROTO_REQUEST_DECODE_OK E1_Server_2 account=" .. decoded.account .. " bytes=" .. #bytes)
else
  error("unknown probe mode: " .. tostring(MISTLANE_SPROTO_MODE))
end
