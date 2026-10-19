// SPDX-License-Identifier: Apache-2.0
import { pathToFileURL } from 'node:url';

const modulePath = process.argv[2];
if (!modulePath) throw new Error('usage: node sproto_sdk_golden.mjs GENERATED.bindings.js');
const { encodeRecord, decodeRecord, sprotoFields } = await import(pathToFileURL(modulePath));
const hex = (bytes) => [...bytes].map((byte) => byte.toString(16).padStart(2, '0')).join('');
const unhex = (text) => Uint8Array.from(text.match(/../g), (pair) => parseInt(pair, 16));
const expectThrow = (label, fn) => {
  try { fn(); } catch (error) {
    if (!(error instanceof RangeError)) throw new Error(`${label}: unexpected error class ${error}`);
    return;
  }
  throw new Error(`${label}: malformed input decoded instead of throwing`);
};

// Standard sproto wire vectors, locked against the vendored upstream encoder:
// an in-band short, the 8-byte two's-complement INT64 form (negative and
// positive beyond the JS Number-safe range), a struct field whose inventory
// holds an array of structs, an array of 32-bit integers, and an array of
// 64-bit integers whose first element needs only 32 bits so the encoder
// sign-extends the padding of the earlier 8-byte slots.
const vectors = [
  ['level=1', { level: 1 }, '020001000400'],
  ['int64_negative', { exp: -9007199254740993n }, '02000700000008000000ffffffffffffdfff'],
  ['int64_positive', { level: 65535, exp: 9007199254740993n }, '0400010000000300000004000000ffff0000080000000100000000002000'],
  ['nested_struct', { inventory: { capacity: 1, items: [{ itemId: 7, count: 2 }, { itemId: 0, count: 1 }] } },
    '0200030000001e000000020004000000140000000600000002001000060006000000020002000400'],
  ['int_array32', { level: 1, tags: [0, 1, 2147483647] }, '040001000400050000000d000000040000000001000000ffffff7f'],
  ['int_array64', { totals: [-1n, 2147483648n, 4294967296n] }, '02000b0000001900000008ffffffffffffffff00000080000000000000000001000000'],
  ['empty_int_array', { tags: [] }, '02000900000000000000'],
  ['empty_struct_array', { inventory: { capacity: 0, items: [] } }, '0200030000000a00000002000200000000000000'],
];
for (const [label, value, expected] of vectors) {
  const encoded = encodeRecord(sprotoFields.Avatar, value);
  if (hex(encoded) !== expected) throw new Error(`JS sproto vector ${label} mismatch: ${hex(encoded)} != ${expected}`);
  console.log(`JS_SPROTO_SDK_GOLDEN ${label} hex=${hex(encoded)} [PASS]`);
}

// Every locked vector also round trips: decode returns the exact integers
// (including the negative wide value the vendored upstream decoder corrupts,
// its todword int arithmetic pins the high dword to all ones), and re-encode
// reproduces the same bytes.
const roundTrips = [
  ['int64_negative', { exp: -9007199254740993n }],
  ['int64_positive', { level: 65535, exp: 9007199254740993n }],
  ['nested_struct', { inventory: { capacity: 1, items: [{ itemId: 7, count: 2 }, { itemId: 0, count: 1 }] } }],
  ['int_array32', { level: 1, tags: [0n, 1n, 2147483647n] }],
  ['int_array64', { totals: [-1n, 2147483648n, 4294967296n] }],
  ['empty_int_array', { tags: [] }],
  ['empty_struct_array', { inventory: { capacity: 0, items: [] } }],
];
for (const [label, value] of roundTrips) {
  const encoded = encodeRecord(sprotoFields.Avatar, value);
  const decoded = decodeRecord(sprotoFields.Avatar, encoded);
  if (hex(encodeRecord(sprotoFields.Avatar, decoded)) !== hex(encoded)) throw new Error(`JS sproto roundtrip ${label} changed the bytes`);
}
const negative = decodeRecord(sprotoFields.Avatar, unhex('02000700000008000000ffffffffffffdfff'));
if (negative.exp !== -9007199254740993n) throw new Error(`JS int64_negative decode mismatch: ${negative.exp}`);
const nested = decodeRecord(sprotoFields.Avatar, unhex('0200030000001e000000020004000000140000000600000002001000060006000000020002000400'));
if (nested.inventory.capacity !== 1n
  || nested.inventory.items[0].itemId !== 7n || nested.inventory.items[0].count !== 2n
  || nested.inventory.items[1].itemId !== 0n || nested.inventory.items[1].count !== 1n) {
  throw new Error(`JS nested struct decode mismatch: ${JSON.stringify(nested)}`);
}
const totals = decodeRecord(sprotoFields.Avatar, unhex('02000b0000001900000008ffffffffffffffff00000080000000000000000001000000'));
if (totals.totals[0] !== -1n || totals.totals[1] !== 2147483648n || totals.totals[2] !== 4294967296n) {
  throw new Error(`JS int array64 decode mismatch: ${JSON.stringify(totals)}`);
}
console.log('JS_SPROTO_SDK_DECODE scalar+struct+arrays roundtrip exact [PASS]');

// Absent fields decode as absent: sproto has no wire defaults, so the
// contract's definition-time defaults stay an application concern.
const onlyLevel = decodeRecord(sprotoFields.Avatar, encodeRecord(sprotoFields.Avatar, { level: 1 }));
if (onlyLevel.level !== 1n || onlyLevel.exp !== undefined) throw new Error('JS absent field did not decode as missing');
console.log('JS_SPROTO_SDK_DECODE defaults: absent fields decode missing [PASS]');

// Unknown wire tags are skipped, never misdelivered: gap word to tag 7 (past
// totals id 6), then an in-band word that decodes into no known field.
const unknown = decodeRecord(sprotoFields.Avatar, unhex('02000d000200'));
if (unknown.level !== undefined || unknown.exp !== undefined || unknown.totals !== undefined) throw new Error('JS unknown tag was not skipped');
console.log('JS_SPROTO_SDK_DECODE unknown_tag: skipped, nothing decoded [PASS]');

// Malformed buffers fail closed: short header, tag block overrun, truncated
// length-prefixed value, trailing bytes, and a struct-array element whose
// length prefix overruns the array content. Out-of-range and non-integer
// inputs fail on encode.
expectThrow('short header', () => decodeRecord(sprotoFields.Avatar, unhex('02')));
expectThrow('short tag block', () => decodeRecord(sprotoFields.Avatar, unhex('03000100')));
expectThrow('truncated value', () => decodeRecord(sprotoFields.Avatar, unhex('02000700000008000000ff')));
expectThrow('trailing bytes', () => decodeRecord(sprotoFields.Avatar, unhex('02000100040000')));
expectThrow('bad array layout', () => decodeRecord(sprotoFields.Avatar, unhex('02000900000003000000')));
expectThrow('non-integer', () => encodeRecord(sprotoFields.Avatar, { level: 1.5 }));
expectThrow('int64 overflow', () => encodeRecord(sprotoFields.Avatar, { exp: 2n ** 63n }));
expectThrow('not an array', () => encodeRecord(sprotoFields.Avatar, { tags: 5 }));
expectThrow('bad array element', () => encodeRecord(sprotoFields.Avatar, { tags: [1.5] }));
console.log('JS_SPROTO_SDK_DECODE malformed: short/truncated/array-layout/overflow rejected [PASS]');
