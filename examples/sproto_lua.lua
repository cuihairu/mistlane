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

if MISTLANE_SPROTO_MODE == "parser" then
  print("UPSTREAM_SCHEMA_PARSE_OK Avatar sproto-bytes=" .. #packed_schema)
elseif MISTLANE_SPROTO_MODE == "golden" then
  local full_bytes = sp:encode("Avatar", full)
  local empty_bytes = sp:encode("Avatar", empty)
  print("STANDARD_SPROTO_GOLDEN full=" .. hex(full_bytes))
  print("STANDARD_SPROTO_GOLDEN empty=" .. hex(empty_bytes))
  assert(hex(full_bytes) == "0400010000000000000004000000ffff00002e00000002000000000004000000ffff00001c000000060000000200020002000e00000002000000040004000000ffffff7f04000000ffffff7f",
    "full Avatar standard sproto golden changed")
  assert(hex(empty_bytes) == "03000100020000000a00000002000200000000000000",
    "empty Avatar standard sproto golden changed")
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
  print("LUA_SPROTO_ROUNDTRIP_OK encode→decode→encode bytes=" .. #bytes)
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
