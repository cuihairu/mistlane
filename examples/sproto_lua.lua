-- SPDX-License-Identifier: Apache-2.0
-- Exercise the vendored upstream cloudwu/sproto Lua API against generated schema.
-- Run after CMake build:
--   build/debug/mistlane-sproto-probe roundtrip build/debug/sproto-fixture/avatar.sproto
local parser = require "sprotoparser"
local sproto = require "sproto"
local packed_schema, sp
if MISTLANE_SPROTO_MODE ~= "regen" then
  local file = assert(io.open(MISTLANE_SPROTO_SCHEMA, "rb"))
  local schema_text = file:read("*a")
  file:close()
  packed_schema = parser.parse(schema_text)
  sp = sproto.new(packed_schema)
  assert(sp:exist_type("Avatar"), "generated Avatar type missing")
end

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
-- BOOL/STRING/BYTES vectors: a boolean rides the integer in-band word
-- ((v+1)*2, so true=4 and false=2 — never length-prefixed from the encoder),
-- a string or bytes field is a length-prefixed byte object, an array of
-- booleans is one raw 0/1 byte per element with no intlen byte, and
-- string/bytes arrays are per-element [len][bytes] like arrays of structs.
-- "中文" pins multi-byte UTF-8 content and the blob bytes pin binary safety
-- through NUL and 0xff.
local text_set = {
  active = true,
  name = "中文",
  blob = "\0\255",
  flags = { true, false, true },
  aliases = { "latin", "" },
  blobs = { "\1\2", "" },
}
local bool_false = { active = false }
-- Every new shape at its empty form: empty strings are zero-length objects,
-- empty bool/string/bytes arrays zero content dwords. false pins the in-band
-- word 2 so a present-false stays distinct from an absent field.
local all_empty = { name = "", blob = "", flags = {}, aliases = {}, blobs = {} }

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
  local text_bytes = sp:encode("Avatar", text_set)
  local false_bytes = sp:encode("Avatar", bool_false)
  local all_empty_bytes = sp:encode("Avatar", all_empty)
  print("STANDARD_SPROTO_GOLDEN bool_string_bytes=" .. hex(text_bytes))
  print("STANDARD_SPROTO_GOLDEN bool_false=" .. hex(false_bytes))
  print("STANDARD_SPROTO_GOLDEN all_empty=" .. hex(all_empty_bytes))
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
  -- BOOL/STRING/BYTES forms: in-band boolean words (true=4, false=2),
  -- length-prefixed string/bytes objects (binary-safe through NUL and 0xff),
  -- a boolean array of raw 0/1 bytes with no intlen byte, string/bytes arrays
  -- as per-element [len][bytes], and the zero-length/empty-array forms.
  assert(hex(text_bytes) == "07000d0004000000000000000000000006000000e4b8ade696870200000000ff030000000100010d000000050000006c6174696e000000000a00000002000000010200000000",
    "BOOL/STRING/BYTES standard sproto golden changed")
  assert(hex(false_bytes) == "02000d000200",
    "in-band boolean false standard sproto golden changed")
  assert(hex(all_empty_bytes) == "06000f00000000000000000000000000000000000000000000000000000000000000",
    "empty string/bool-array/string-array/bytes-array standard sproto golden changed")
  local text_decoded = sp:decode("Avatar", text_bytes)
  assert(text_decoded.active == true and text_decoded.name == "中文"
    and text_decoded.blob == "\0\255"
    and text_decoded.flags[1] == true and text_decoded.flags[2] == false and text_decoded.flags[3] == true
    and text_decoded.aliases[1] == "latin" and text_decoded.aliases[2] == ""
    and text_decoded.blobs[1] == "\1\2" and text_decoded.blobs[2] == "",
    "BOOL/STRING/BYTES values did not decode back")
  local empty_decoded = sp:decode("Avatar", all_empty_bytes)
  assert(empty_decoded.name == "" and empty_decoded.blob == ""
    and #empty_decoded.flags == 0 and #empty_decoded.aliases == 0 and #empty_decoded.blobs == 0,
    "empty STRING/arrays did not decode as present-empty")
  print("STANDARD_SPROTO_GOLDEN bool/string/bytes: wire forms + decode + empties [PASS]")
  -- Absent fields are simply not decoded: sproto has no wire defaults, so the
  -- contract's definition-time defaults stay an application concern.
  local without_exp = sp:decode("Avatar", sp:encode("Avatar", { level = 1 }))
  assert(without_exp.exp == nil and without_exp.level == 1, "absent INT64 did not decode as missing")
  print("STANDARD_SPROTO_GOLDEN defaults: absent fields decode missing [PASS]")
  -- A wire tag outside the schema is skipped, never misdelivered (upstream
  -- sproto.c findtag miss continues): count 2, a gap word landing on tag 13
  -- (past blobs id 12), then an in-band word that decodes into no field.
  local unknown = sp:decode("Avatar", "\x02\x00\x19\x00\x02\x00")
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
  for name, vec in pairs({ nested = nested, tags32 = tags32, empty_tags = empty_tags,
      text_set = text_set, bool_false = bool_false, all_empty = all_empty }) do
    local vec_bytes = sp:encode("Avatar", vec)
    local vec_decoded = sp:decode("Avatar", vec_bytes)
    assert(sp:encode("Avatar", vec_decoded) == vec_bytes, name .. " array/struct vector did not round trip")
  end
  local text_rt = sp:decode("Avatar", sp:encode("Avatar", text_set))
  assert(text_rt.active == true and text_rt.name == "中文" and text_rt.blob == "\0\255"
    and text_rt.flags[2] == false and text_rt.aliases[1] == "latin"
    and text_rt.blobs[1] == "\1\2" and text_rt.blobs[2] == "",
    "BOOL/STRING/BYTES roundtrip lost values")
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
elseif MISTLANE_SPROTO_MODE == "regen" then
  -- Recover a .sproto text source from a compiled .spb schema (the game-fogs
  -- business protocol contract, whose original text source is absent; see
  -- docs/protocol-contract-recovery.md). Walks the .spb wire form directly —
  -- schema.txt-style dumps lose the decimal extra and map shape — and proves
  -- fidelity by recompiling the emitted text with the vendored sprotoparser
  -- and requiring byte identity with the input. MISTLANE_REGEN_GOLDEN (if
  -- set) must equal the emitted text; MISTLANE_REGEN_OUT (if set) receives it.
  local file = assert(io.open(MISTLANE_SPROTO_SCHEMA, "rb"))
  local bin = file:read("*a")
  file:close()

  local function word16(b, p) return b:byte(p) + b:byte(p + 1) * 256 end
  local function u32(b, p)
    return b:byte(p) + b:byte(p + 1) * 256 + b:byte(p + 2) * 65536 + b:byte(p + 3) * 16777216
  end
  -- packbytes: u32 little-endian length + bytes (sprotoparser.lua <s4).
  local function blob(b, p)
    local n = u32(b, p)
    return b:sub(p + 4, p + 3 + n), p + 4 + n
  end
  -- Schema attribute word: odd = absent ("\1\0" skip), even v = value v/2-1
  -- (packvalue encodes (id+1)*2).
  local function attr(b, p)
    local v = word16(b, p)
    if v % 2 == 1 then return nil, p + 2 end
    return math.floor(v / 2) - 1, p + 2
  end

  -- Group: u16 count(=2), two u16 placeholder words, then packbytes types
  -- blob, packbytes protocols blob.
  assert(word16(bin, 1) == 2, "group header is not the two-member form")
  local types_blob, gpos = blob(bin, 7)
  local protos_blob, gend = blob(bin, gpos)
  assert(gend == #bin + 1, "trailing bytes after the schema group")

  -- Types: per-type block = u16 field-count-word (1 = name only, 2 = name +
  -- fields), that many u16 words (name ref placeholder), packbytes name, and
  -- for fielded types packbytes fields blob. Each field block = u16 attr word
  -- count (4 scalar / 5 array / 6 keyed map / 7 generic map), a name-ref
  -- placeholder, the attribute words, then packbytes field name. Attribute
  -- words in order: buildin (absent for struct refs), extra (buildin) or type
  -- id (struct), tag, array flag, key tag, map flag.
  local names = {}  -- spb array order = the serializer's sorted order; id = index-1
  local types = {}  -- name -> field list (filled after the walk, sorted by tag)
  local pos = 1
  while pos <= #types_blob do
    local block; block, pos = blob(types_blob, pos)
    local n = word16(block, 1)
    local off = 3 + 2 * n
    local name; name, off = blob(block, off)
    local fields = {}
    if n >= 2 then
      local fblob; fblob, off = blob(block, off)
      local fpos = 1
      while fpos <= #fblob do
        local fb; fb, fpos = blob(fblob, fpos)
        local na = word16(fb, 1)
        local o = 5
        local buildin; buildin, o = attr(fb, o)
        local ref; ref, o = attr(fb, o)      -- extra for buildin, type id for struct
        local tag; tag, o = attr(fb, o)
        local array, key, map
        if na >= 5 then array, o = attr(fb, o) end
        if na >= 6 then key, o = attr(fb, o) end
        if na >= 7 then map, o = attr(fb, o) end
        local fname; fname, o = blob(fb, o)
        fields[#fields + 1] = { name = fname, tag = tag, buildin = buildin,
          extra = ref, refid = ref, array = array, key = key, map = map }
      end
    end
    table.sort(fields, function(a, b) return a.tag < b.tag end)
    names[#names + 1] = name
    types[name] = fields
  end
  local name_of = {}
  for i, n in ipairs(names) do name_of[i - 1] = n end

  -- Protocols: block = u16 attr word count (2 tag only / 3 no response /
  -- 4 request + response / 5 confirm), name-ref placeholder, tag, request id,
  -- response id, confirm flag, then packbytes name.
  local protocols = {}
  pos = 1
  while pos <= #protos_blob do
    local block; block, pos = blob(protos_blob, pos)
    local na = word16(block, 1)
    local o = 5
    local tag; tag, o = attr(block, o)
    local request, response, confirm
    if na >= 3 then request, o = attr(block, o) end
    if na >= 4 then response, o = attr(block, o) end
    if na >= 5 then confirm, o = attr(block, o) end
    local pname; pname, o = blob(block, o)
    protocols[#protocols + 1] = { name = pname, tag = tag,
      request = request and name_of[request] or nil,
      response = response and name_of[response] or nil,
      confirm = confirm ~= nil }
  end

  -- Render. References must survive sprotoparser's scope walk (checktype:
  -- innermost dotted prefix first, bare name last), so the emitter tries the
  -- full dotted name and then progressively shorter suffixes until the
  -- simulation resolves to the exact intended type — the spb holds the
  -- already-resolved name, and a shorter form that shadow-resolves elsewhere
  -- would mistranscribe it.
  local function resolves_to(context, textname)
    local scope = context
    while scope do
      local full = scope .. "." .. textname
      if types[full] then return full end
      scope = scope:match "(.+)%..+$"
    end
    if types[textname] then return textname end
    return nil
  end
  local function emit_ref(context, intended)
    local comps = {}
    for c in intended:gmatch "[^.]+" do comps[#comps + 1] = c end
    for i = 1, #comps do
      local suffix = table.concat(comps, ".", i)
      if resolves_to(context, suffix) == intended then return suffix end
    end
    error("no expressible reference from " .. context .. " to " .. intended)
  end

  local buildin_text = { [0] = "integer", [1] = "boolean", [2] = "string", [3] = "double" }
  -- A type named <Proto>.request / <Proto>.response whose protocol exists is
  -- rendered inline under that protocol (the original parser's inline-struct
  -- naming); identical IR either way, inline is the canonical shape.
  local owned = {}
  for _, pr in ipairs(protocols) do
    if pr.request == pr.name .. ".request" then owned[pr.request] = true end
    if pr.response == pr.name .. ".response" then owned[pr.response] = true end
  end
  -- Type name -> nested child full names. A child's dotted parent must be a
  -- real type or a protocol-owned inline struct — the only ways the original
  -- parser could have produced the dotted name.
  local children = {}
  for _, n in ipairs(names) do
    local parent = n:match "(.+)%.[^.]+$"
    if parent then
      if not types[parent] then
        assert(owned[n], "type " .. n .. " has no expressible parent declaration")
      end
      children[parent] = children[parent] or {}
      table.insert(children[parent], n)
    end
  end

  local function key_field_name(reftype, keytag)
    for _, f in ipairs(types[reftype]) do
      if f.tag == keytag then return f.name end
    end
    error("map key tag " .. keytag .. " not found in " .. reftype)
  end
  local function field_line(context, f)
    local base
    if f.buildin ~= nil then
      base = buildin_text[f.buildin]
      if f.buildin == 0 and f.extra and f.extra > 0 then
        base = base .. "(" .. f.extra .. ")"       -- decimal: explicit tag set
      elseif f.buildin == 2 and f.extra == 1 then
        base = "binary"
      end
    else
      base = emit_ref(context, name_of[f.refid])
    end
    if f.array then base = "*" .. base end
    if f.key then base = base .. "(" .. key_field_name(name_of[f.refid], f.key) .. ")" end
    return f.name .. " " .. f.tag .. " : " .. base
  end
  local function render_body(name, indent, lines)
    for _, f in ipairs(types[name]) do
      lines[#lines + 1] = indent .. field_line(name, f)
    end
    for _, child in ipairs(children[name] or {}) do
      lines[#lines + 1] = indent .. "." .. child:match "[^.]+$" .. " {"
      render_body(child, indent .. "\t", lines)
      lines[#lines + 1] = indent .. "}"
    end
  end

  local lines = {
    "# Recovered .sproto text for the game-fogs business protocol schema.",
    "# Regenerated from the committed binary schema (sproto.spb, game-fogs",
    "# commit 37b640a) by the vendored sproto toolchain; byte-identity of",
    "# recompile verified by the regen probe mode. The original text source,",
    "# generation script and official client bindings remain absent (see",
    "# docs/protocol-contract-recovery.md): string/binary naming, inline vs",
    "# shared type placement and comments are canonicalized, not original.",
  }
  for _, n in ipairs(names) do
    if not owned[n] then
      local parent = n:match "(.+)%.[^.]+$"
      if not parent then
        lines[#lines + 1] = "." .. n .. " {"
        render_body(n, "\t", lines)
        lines[#lines + 1] = "}"
        lines[#lines + 1] = ""
      end
    end
  end
  for _, pr in ipairs(protocols) do
    lines[#lines + 1] = pr.name .. " " .. pr.tag .. " {"
    if pr.request then
      if owned[pr.request] then
        lines[#lines + 1] = "\trequest {"
        render_body(pr.request, "\t\t", lines)
        lines[#lines + 1] = "\t}"
      else
        lines[#lines + 1] = "\trequest " .. pr.request
      end
    end
    if pr.response then
      if owned[pr.response] then
        lines[#lines + 1] = "\tresponse {"
        render_body(pr.response, "\t\t", lines)
        lines[#lines + 1] = "\t}"
      else
        lines[#lines + 1] = "\tresponse " .. pr.response
      end
    elseif pr.confirm then
      lines[#lines + 1] = "\tresponse nil"
    end
    lines[#lines + 1] = "}"
    lines[#lines + 1] = ""
  end
  local text = table.concat(lines, "\n")

  -- Fidelity proof: the emitted text must recompile to the exact input bytes.
  local repacked = parser.parse(text)
  if repacked ~= bin then
    local at = 1
    while at <= math.max(#repacked, #bin) and repacked:byte(at) == bin:byte(at) do at = at + 1 end
    error("regenerated schema diverges from the compiled input at byte " .. at
      .. string.format(" (text %d bytes, input %d bytes)", #repacked, #bin))
  end
  local fields_n, maps_n = 0, 0
  for _, t in pairs(types) do
    for _, f in ipairs(t) do
      fields_n = fields_n + 1
      if f.key then maps_n = maps_n + 1 end
    end
  end
  print("FOGS_SPROTO_REGEN types=" .. #names .. " protocols=" .. #protocols
    .. " fields=" .. fields_n .. " maps=" .. maps_n
    .. " bytes=" .. #bin .. " compile=IDENTICAL")
  local golden = os.getenv("MISTLANE_REGEN_GOLDEN")
  if golden then
    local g = assert(io.open(golden, "rb"))
    local golden_text = g:read("*a")
    g:close()
    assert(golden_text == text, "committed regeneration golden differs from the emitted text")
    print("FOGS_SPROTO_REGEN golden=" .. golden .. " [MATCH]")
  end
  local out = os.getenv("MISTLANE_REGEN_OUT")
  if out then
    local f = assert(io.open(out, "wb"))
    f:write(text)
    f:close()
    print("FOGS_SPROTO_REGEN wrote " .. out .. " (" .. #text .. " bytes)")
  end
  print("FOGS_SPROTO_REGEN spb→sproto→spb byte identity [PASS]")
else
  error("unknown probe mode: " .. tostring(MISTLANE_SPROTO_MODE))
end
