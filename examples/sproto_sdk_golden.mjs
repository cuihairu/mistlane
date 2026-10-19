// SPDX-License-Identifier: Apache-2.0
import { pathToFileURL } from 'node:url';

const modulePath = process.argv[2];
if (!modulePath) throw new Error('usage: node sproto_sdk_golden.mjs GENERATED.bindings.js');
const { encodeScalar, decodeScalar, sprotoFields } = await import(pathToFileURL(modulePath));
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
// positive beyond the JS Number-safe range), and mixed 4/8-byte records.
const vectors = [
  ['level=1', { level: 1 }, '020001000400'],
  ['int64_negative', { exp: -9007199254740993n }, '02000700000008000000ffffffffffffdfff'],
  ['int64_positive', { level: 65535, exp: 9007199254740993n }, '0400010000000300000004000000ffff0000080000000100000000002000'],
];
for (const [label, value, expected] of vectors) {
  const encoded = encodeScalar(sprotoFields.Avatar, value);
  if (hex(encoded) !== expected) throw new Error(`JS sproto vector ${label} mismatch: ${hex(encoded)} != ${expected}`);
  console.log(`JS_SPROTO_SDK_GOLDEN ${label} hex=${hex(encoded)} [PASS]`);
}

// Decode returns the exact integers, including the negative wide value the
// vendored upstream decoder corrupts (its todword int arithmetic pins the
// high dword to all ones); mistlane codecs must read it back exactly.
const negative = decodeScalar(sprotoFields.Avatar, unhex('02000700000008000000ffffffffffffdfff'));
if (negative.exp !== -9007199254740993n) throw new Error(`JS int64_negative decode mismatch: ${negative.exp}`);
const positive = decodeScalar(sprotoFields.Avatar, unhex('0400010000000300000004000000ffff0000080000000100000000002000'));
if (positive.level !== 65535n || positive.exp !== 9007199254740993n) throw new Error(`JS int64_positive decode mismatch: ${JSON.stringify(positive)}`);
console.log('JS_SPROTO_SDK_DECODE int64 negative=-9007199254740993 positive=9007199254740993 [PASS]');

// Absent fields decode as absent: sproto has no wire defaults, so the
// contract's definition-time defaults stay an application concern.
const onlyLevel = decodeScalar(sprotoFields.Avatar, encodeScalar(sprotoFields.Avatar, { level: 1 }));
if (onlyLevel.level !== 1n || onlyLevel.exp !== undefined) throw new Error('JS absent field did not decode as missing');
console.log('JS_SPROTO_SDK_DECODE defaults: absent fields decode missing [PASS]');

// Unknown wire tags are skipped, never misdelivered: gap word to tag 5 (past
// exp id 4), then an in-band word that decodes into no known field.
const unknown = decodeScalar(sprotoFields.Avatar, unhex('02000b000200'));
if (unknown.level !== undefined || unknown.exp !== undefined) throw new Error('JS unknown tag was not skipped');
console.log('JS_SPROTO_SDK_DECODE unknown_tag: skipped, nothing decoded [PASS]');

// Malformed buffers fail closed: short header, tag block overrun, truncated
// length-prefixed value, trailing bytes. Out-of-range and non-integer inputs
// fail on encode.
expectThrow('short header', () => decodeScalar(sprotoFields.Avatar, unhex('02')));
expectThrow('short tag block', () => decodeScalar(sprotoFields.Avatar, unhex('03000100')));
expectThrow('truncated value', () => decodeScalar(sprotoFields.Avatar, unhex('02000700000008000000ff')));
expectThrow('trailing bytes', () => decodeScalar(sprotoFields.Avatar, unhex('02000100040000')));
expectThrow('non-integer', () => encodeScalar(sprotoFields.Avatar, { level: 1.5 }));
expectThrow('int64 overflow', () => encodeScalar(sprotoFields.Avatar, { exp: 2n ** 63n }));
console.log('JS_SPROTO_SDK_DECODE malformed: short/truncated/overflow rejected [PASS]');
