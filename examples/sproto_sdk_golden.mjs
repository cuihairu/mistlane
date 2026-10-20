// SPDX-License-Identifier: Apache-2.0
import { pathToFileURL } from 'node:url';

const modulePath = process.argv[2];
if (!modulePath) throw new Error('usage: node sproto_sdk_golden.mjs GENERATED.bindings.js');
const { encodeRecord, decodeRecord, sprotoFields, encodeEnvelope, decodeEnvelope, encodeReply, decodeReply,
  entityTypes, entityMethods, EntityRpcQueue, EntityClient } = await import(pathToFileURL(modulePath));
const hex = (bytes) => [...bytes].map((byte) => byte.toString(16).padStart(2, '0')).join('');
const utf8 = (text) => new TextEncoder().encode(text);
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
// sign-extends the padding of the earlier 8-byte slots. BOOL/STRING/BYTES:
// booleans ride the integer in-band word (true=4, false=2), strings and bytes
// are length-prefixed byte objects, boolean arrays are one raw 0/1 byte per
// element with no intlen byte, and string/bytes arrays are per-element
// [len][bytes]; "中文" pins multi-byte UTF-8 content.
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
  ['bool_string_bytes', {
    active: true, name: '中文', blob: Uint8Array.of(0, 255),
    flags: [true, false, true], aliases: ['latin', ''], blobs: [Uint8Array.of(1, 2), Uint8Array.of()],
  }, '07000d0004000000000000000000000006000000e4b8ade696870200000000ff030000000100010d000000050000006c6174696e000000000a00000002000000010200000000'],
  ['bool_false', { active: false }, '02000d000200'],
  ['all_empty', { name: '', blob: Uint8Array.of(), flags: [], aliases: [], blobs: [] }, '06000f00000000000000000000000000000000000000000000000000000000000000'],
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
  ['bool_string_bytes', {
    active: true, name: '中文', blob: Uint8Array.of(0, 255),
    flags: [true, false, true], aliases: ['latin', ''], blobs: [Uint8Array.of(1, 2), Uint8Array.of()],
  }],
  ['bool_false', { active: false }],
  ['all_empty', { name: '', blob: Uint8Array.of(), flags: [], aliases: [], blobs: [] }],
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
const bytesEqual = (a, b) => a.length === b.length && [...a].every((byte, i) => byte === b[i]);
const text = decodeRecord(sprotoFields.Avatar, unhex('07000d0004000000000000000000000006000000e4b8ade696870200000000ff030000000100010d000000050000006c6174696e000000000a00000002000000010200000000'));
if (text.active !== true || text.name !== '中文' || !bytesEqual(text.blob, [0, 255])
  || !(text.flags.length === 3 && text.flags[0] === true && text.flags[1] === false && text.flags[2] === true)
  || text.aliases[0] !== 'latin' || text.aliases[1] !== ''
  || !bytesEqual(text.blobs[0], [1, 2]) || !bytesEqual(text.blobs[1], [])) {
  throw new Error(`JS bool/string/bytes decode mismatch: ${JSON.stringify(text)}`);
}
const allEmpty = decodeRecord(sprotoFields.Avatar, unhex('06000f00000000000000000000000000000000000000000000000000000000000000'));
if (allEmpty.name !== '' || !bytesEqual(allEmpty.blob, [])
  || !(allEmpty.flags.length === 0 && allEmpty.aliases.length === 0 && allEmpty.blobs.length === 0)) {
  throw new Error(`JS empty string/arrays decode mismatch: ${JSON.stringify(allEmpty)}`);
}
console.log('JS_SPROTO_SDK_DECODE scalar+struct+arrays+bool/string/bytes roundtrip exact [PASS]');

// Absent fields decode as absent: sproto has no wire defaults, so the
// contract's definition-time defaults stay an application concern.
const onlyLevel = decodeRecord(sprotoFields.Avatar, encodeRecord(sprotoFields.Avatar, { level: 1 }));
if (onlyLevel.level !== 1n || onlyLevel.exp !== undefined) throw new Error('JS absent field did not decode as missing');
console.log('JS_SPROTO_SDK_DECODE defaults: absent fields decode missing [PASS]');

// Unknown wire tags are skipped, never misdelivered: gap word to tag 13 (past
// blobs id 12), then an in-band word that decodes into no known field.
const unknown = decodeRecord(sprotoFields.Avatar, unhex('020019000200'));
if (unknown.level !== undefined || unknown.exp !== undefined || unknown.totals !== undefined) throw new Error('JS unknown tag was not skipped');
console.log('JS_SPROTO_SDK_DECODE unknown_tag: skipped, nothing decoded [PASS]');

// Malformed buffers fail closed: short header, tag block overrun, truncated
// length-prefixed value, trailing bytes, a struct-array element whose
// length prefix overruns the array content, an invalid UTF-8 STRING payload,
// a boolean-array byte that is neither 0 nor 1, and a string-array element
// whose length prefix overruns the content. Out-of-range and wrongly typed
// inputs fail on encode.
expectThrow('short header', () => decodeRecord(sprotoFields.Avatar, unhex('02')));
expectThrow('short tag block', () => decodeRecord(sprotoFields.Avatar, unhex('03000100')));
expectThrow('truncated value', () => decodeRecord(sprotoFields.Avatar, unhex('02000700000008000000ff')));
expectThrow('trailing bytes', () => decodeRecord(sprotoFields.Avatar, unhex('02000100040000')));
expectThrow('bad array layout', () => decodeRecord(sprotoFields.Avatar, unhex('02000900000003000000')));
expectThrow('invalid utf8 string', () => decodeRecord(sprotoFields.Avatar, unhex('02000f00000001000000ff')));
expectThrow('bad boolean array', () => decodeRecord(sprotoFields.Avatar, unhex('020013000000010000000002')));
expectThrow('bad string-array element', () => decodeRecord(sprotoFields.Avatar, unhex('020015000000050000000500000061')));
expectThrow('non-integer', () => encodeRecord(sprotoFields.Avatar, { level: 1.5 }));
expectThrow('int64 overflow', () => encodeRecord(sprotoFields.Avatar, { exp: 2n ** 63n }));
expectThrow('not an array', () => encodeRecord(sprotoFields.Avatar, { tags: 5 }));
expectThrow('bad array element', () => encodeRecord(sprotoFields.Avatar, { tags: [1.5] }));
expectThrow('not a boolean', () => encodeRecord(sprotoFields.Avatar, { active: 'yes' }));
expectThrow('not a string', () => encodeRecord(sprotoFields.Avatar, { name: 5 }));
expectThrow('not bytes', () => encodeRecord(sprotoFields.Avatar, { blob: 'x' }));
expectThrow('not a boolean array', () => encodeRecord(sprotoFields.Avatar, { flags: ['yes'] }));
console.log('JS_SPROTO_SDK_DECODE malformed: short/truncated/array-layout/utf-8/bool rejected [PASS]');

// A kind outside the def grammar still cannot slip through the codec: the
// generated tables only carry known kinds, but an injected unknown kind is
// refused on both directions. id 0 so the wire tags below reach the field.
const unsupported = [{ id: 0, name: 'zz', kind: 'wat' }];
expectThrow('unsupported kind', () => encodeRecord(unsupported, { zz: true }));
expectThrow('unsupported in-band decode', () => decodeRecord(unsupported, unhex('01000200')));
expectThrow('unsupported length-prefixed decode', () => decodeRecord(unsupported, unhex('0100000000000000')));
console.log('JS_SPROTO_SDK_CODEC unsupported kinds refused [PASS]');

// Invocation envelope and typed reply (docs/entity-contract.md): the C++
// server bytes are the reference (the entity.envelope case pins them). The
// generated client codec must encode the same fixed big-endian header byte
// for byte — the canonical vector and the extremes with a negative entity id
// and the maximum correlation — decode them exactly, and fail closed on
// truncation, unknown side bytes, malformed reply kinds/codes, reason lengths
// that do not match, reasons outside UTF-8 and trailing bytes.
const canonicalBytes = '0000000700000001000000000000002a00000003010002112233445566778800000005000000fa7061796c6f6164';
const canonical = encodeEnvelope(
  { scope: 7, type_id: 1, entity_id: 42n, incarnation: 3, side: 1, method_id: 2,
    correlation_id: 0x1122334455667788n, hosting_generation: 5, deadline_ms: 250 },
  utf8('payload'));
if (hex(canonical) !== canonicalBytes) throw new Error('envelope canonical bytes drifted');
const decodedEnvelope = decodeEnvelope(unhex(canonicalBytes));
if (!decodedEnvelope || decodedEnvelope.entity_id !== 42n || decodedEnvelope.correlation_id !== 0x1122334455667788n
  || decodedEnvelope.side !== 1 || decodedEnvelope.deadline_ms !== 250
  || hex(decodedEnvelope.payload) !== '7061796c6f6164')
  throw new Error('envelope decode lost fields');
const extremeBytes = 'ffffffff0000003ffffffffffffffffdffffffff0000fefffffffffffffffffffffffeffffffff';
const extreme = decodeEnvelope(unhex(extremeBytes));
if (extreme.entity_id !== -3n || extreme.correlation_id !== 0xffffffffffffffffn)
  throw new Error('envelope signed entity id or unsigned correlation lost');
const extremeRoundTrip = encodeEnvelope(extreme, extreme.payload);
if (hex(extremeRoundTrip) !== extremeBytes) throw new Error('envelope extreme re-encode drifted');
if (decodeEnvelope(unhex('0000000700000001000000000000002a00000003')) !== null)
  throw new Error('truncated envelope decoded');
const badSide = unhex(canonicalBytes);
badSide[20] = 2;
if (decodeEnvelope(badSide) !== null) throw new Error('unknown side byte decoded');
expectThrow('envelope side range', () => encodeEnvelope({ side: 2 }, new Uint8Array(0)));
expectThrow('envelope negative correlation', () => encodeEnvelope({ correlation_id: -1n }, new Uint8Array(0)));
if (hex(encodeReply({ payload: utf8('returns') })) !== '0072657475726e73')
  throw new Error('ok reply drifted');
if (hex(encodeReply({ error: 'INVALID', reason: 'bad shape' })) !== '01000009626164207368617065')
  throw new Error('INVALID reply drifted');
if (hex(encodeReply({ error: 'STALE' })) !== '0102') throw new Error('STALE reply drifted');
const decodedOk = decodeReply(unhex('0072657475726e73'));
if (!decodedOk || decodedOk.error !== null || hex(decodedOk.payload) !== '72657475726e73')
  throw new Error('ok reply decode failed');
const decodedInvalid = decodeReply(unhex('01000009626164207368617065'));
if (!decodedInvalid || decodedInvalid.error !== 'INVALID' || decodedInvalid.reason !== 'bad shape')
  throw new Error('INVALID reply reason lost');
const decodedStale = decodeReply(unhex('0102'));
if (!decodedStale || decodedStale.error !== 'STALE') throw new Error('STALE reply decode failed');
if (decodeReply(new Uint8Array(0)) !== null) throw new Error('empty reply decoded');
if (decodeReply(unhex('0578')) !== null) throw new Error('unknown reply kind decoded');
if (decodeReply(unhex('01c8')) !== null) throw new Error('unknown error code decoded');
if (decodeReply(unhex('01')) !== null) throw new Error('truncated reply decoded');
if (decodeReply(unhex('010000056162')) !== null) throw new Error('reason length mismatch decoded');
if (decodeReply(unhex('010200')) !== null) throw new Error('trailing bytes after STALE decoded');
if (decodeReply(unhex('01000002ffff')) !== null) throw new Error('invalid utf-8 reason decoded');
expectThrow('reply reason on non-INVALID', () => encodeReply({ error: 'STALE', reason: 'x' }));
expectThrow('reply unknown error', () => encodeReply({ error: 'NOPE' }));
console.log('JS_ENTITY_ENVELOPE canonical/extremes/replies and malformed refusals [PASS]');

// Client-side facade and bounded queue (docs/entity-contract.md): typed
// admissions, deadlines on an injected clock, surfaced timeout tombstones,
// and a retry that only re-arms idempotent calls with a fresh correlation
// id. The facade sends real envelope bytes straight off its own Avatar —
// method names, sides and return tables come from the generated tables, so
// there is no .server/.client intermediate layer.
if (entityTypes.Avatar !== 1) throw new Error('entity type table wrong');
if (entityMethods.Avatar.outbound.attack.mode !== 'notify'
  || entityMethods.Avatar.outbound.getProfile.returns !== 'Avatar_getProfile_Returns'
  || entityMethods.Avatar.inbound.onReward.args !== 'Avatar_onReward_Args')
  throw new Error('method tables wrong');
{
  const q = new EntityRpcQueue(2);
  if (q.call(100, false, null, 7) !== 7 || q.call(100, false, null, 8) !== 8) throw new Error('queue admission');
  if (q.call(100, false, null, 7) !== null || q.error !== 'INVALID') throw new Error('duplicate not INVALID');
  if (q.call(100) !== null || q.error !== 'CAPACITY') throw new Error('overflow not CAPACITY');
  if (q.call(0, false, null, 9) !== null || q.error !== 'INVALID') throw new Error('zero deadline not INVALID');
  if (q.advance(-1) !== false) throw new Error('negative clock accepted');
  q.advance(100);
  if (q.state(7) !== 'TIMED_OUT' || q.state(8) !== 'TIMED_OUT') throw new Error('deadlines not burned');
  if (q.complete(7) !== 'TIMEOUT') throw new Error('late reply not TIMEOUT');
  if (q.complete(99) !== 'STALE') throw new Error('unknown reply not STALE');
  const tomb = q.take(7);
  if (!tomb || tomb.state !== 'TIMED_OUT' || tomb.idempotent !== false) throw new Error('tombstone not surfaced');
  if (q.size !== 1) throw new Error('take did not release the slot');
  const c = q.call(50);
  if (!q.cancel(c) || q.cancel(c) !== false || q.error !== 'INVALID') throw new Error('cancel discipline');
  if (q.complete(c) !== 'CANCELLED') throw new Error('reply after cancel');
  q.take(c);
  const d = q.call(100);
  if (q.complete(d) !== 'OK' || q.complete(d) !== 'CONFLICT') throw new Error('double reply not CONFLICT');
  q.take(d);
  q.call(100, false, null, 21);
  q.advance(200);
  if (q.retry(21, 300) !== null) throw new Error('non-idempotent timeout retried');
  if (q.state(21) !== 'TIMED_OUT') throw new Error('tombstone vanished after refused retry');
  q.take(8);  // free one slot; the table below stays at capacity through the retry
  if (q.call(100, true, null, 31) !== 31) throw new Error('idempotent admission refused');
  q.advance(400);
  const fresh = q.retry(31, 300);  // full table: the tombstone slot is reused
  if (fresh === null || fresh === 31 || q.state(31) !== null
    || q.state(fresh) !== 'PENDING' || q.get(fresh).idempotent !== true) throw new Error('idempotent re-arm');
}
{
  const sent = [];
  const client = new EntityClient({ send: (bytes) => sent.push(bytes), type: 'Avatar', entity_id: 42n, incarnation: 3, capacity: 2 });
  if (client.call('nope').ok !== false || client.call('attack').ok !== false
    || client.notify('getProfile').ok !== false) throw new Error('facade mode misuse not INVALID');
  const call = client.call('getProfile', { account: 9 }, { deadline_ms: 100, idempotent: true });
  if (!call.ok) throw new Error('call refused');
  const env = decodeEnvelope(sent[0]);
  if (env.side !== 0 || env.method_id !== 2 || env.correlation_id !== BigInt(call.id)
    || env.hosting_generation !== 0 || env.deadline_ms !== 100 || env.entity_id !== 42n)
    throw new Error('facade envelope wrong');
  if (hex(env.payload) !== hex(encodeRecord(sprotoFields.Avatar_getProfile_Args, { account: 9 })))
    throw new Error('args payload wrong');
  const c2 = client.call('getProfile', { account: 10 }, { deadline_ms: 100 });
  if (!c2.ok) throw new Error('second call refused');
  const c3 = client.call('getProfile', { account: 11 });
  if (c3.ok || c3.error !== 'CAPACITY') throw new Error('facade overflow not typed');
  const returns = encodeReply({ payload: encodeRecord(sprotoFields.Avatar_getProfile_Returns, { level: 7 }) });
  if (client.on_reply(call.id, returns) !== 'OK') throw new Error('reply verdict');
  const done = client.take(call.id);
  if (!done || done.state !== 'OK' || done.error !== null || done.response.level !== 7n) throw new Error('typed return lost');
  // a typed error settles the call: the verdict is OK, the entry carries STALE
  if (client.on_reply(c2.id, encodeReply({ error: 'STALE' })) !== 'OK') throw new Error('error reply verdict');
  const done2 = client.take(c2.id);
  if (!done2 || done2.error !== 'STALE' || done2.response !== null) throw new Error('typed error lost');
  const r1 = client.call('getProfile', { account: 12 }, { deadline_ms: 50, idempotent: true });
  client.advance(1000);
  const first = decodeEnvelope(sent[sent.length - 1]);
  const fresh = client.retry(r1.id, 200);
  if (fresh === null || fresh === r1.id) throw new Error('retry not fresh');
  const resent = decodeEnvelope(sent[sent.length - 1]);
  if (resent.correlation_id !== BigInt(fresh) || resent.method_id !== 2
    || hex(resent.payload) !== hex(first.payload)) throw new Error('retry envelope wrong');
  client.advance(1200);
  if (client.on_reply(fresh, returns) !== 'TIMEOUT') throw new Error('late reply after deadline');
  const m1 = client.call('getProfile', { account: 13 });
  if (client.on_reply(m1.id, unhex('0578')) !== 'MALFORMED' || client.queue.state(m1.id) !== 'PENDING')
    throw new Error('malformed reply consumed the slot');
  let noticed = null;
  const notifyBytes = encodeEnvelope(
    { scope: 0, type_id: 1, entity_id: -1n, incarnation: 0, side: 1, method_id: 1,
      correlation_id: 0n, hosting_generation: 0, deadline_ms: 0 },
    encodeRecord(sprotoFields.Avatar_onReward_Args, { itemId: 77 }));
  client.on_notify(notifyBytes, (name, args) => { noticed = [name, args]; });
  if (!noticed || noticed[0] !== 'onReward' || noticed[1].itemId !== 77n) throw new Error('inbound notify lost');
  const clientSide = encodeEnvelope(
    { scope: 0, type_id: 1, entity_id: 42n, incarnation: 0, side: 0, method_id: 1,
      correlation_id: 0n, hosting_generation: 0, deadline_ms: 0 }, new Uint8Array(0));
  if (client.on_notify(clientSide, () => { throw new Error('client-side frame delivered as notify'); }) !== null)
    throw new Error('client-side frame accepted as notify');
  if (client.on_notify(unhex('0578'), () => { throw new Error('malformed frame delivered'); }) !== null)
    throw new Error('malformed frame accepted as notify');
}
console.log('JS_ENTITY_CLIENT bounded queue, facade calls/notify/retry and malformed refusals [PASS]');
