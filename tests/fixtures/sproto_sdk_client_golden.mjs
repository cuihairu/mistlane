// SPDX-License-Identifier: Apache-2.0
// Client-audience artifact golden for the MLV1 session negotiation surface
// (docs/entity-contract.md, entity.client.versions): the digest the generated
// client emits must agree with the published lock written beside it, the
// handshake frame bytes are pinned, every host reply verdict parses, and
// malformed replies refuse closed. A small codec probe confirms the client
// artifact kept the wire codec its server twin pins in sproto_sdk_golden.mjs.
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { pathToFileURL } from 'node:url';

const modulePath = process.argv[2];
if (!modulePath) throw new Error('usage: node tests/fixtures/sproto_sdk_client_golden.mjs GENERATED.client.js');
const mod = await import(pathToFileURL(modulePath));
const hex = (bytes) => [...bytes].map((byte) => byte.toString(16).padStart(2, '0')).join('');
const utf8 = (text) => new TextEncoder().encode(text);
const bytesEqual = (a, b) => a.length === b.length && [...a].every((byte, i) => byte === b[i]);

// The audience filter must not have touched the wire codec: the canonical
// envelope vector and one sproto vector are the same bytes the server
// artifact's golden pins, and the facade tables stay present.
const canonicalBytes = '0000000700000001000000000000002a00000003010002112233445566778800000005000000fa7061796c6f6164';
if (hex(mod.encodeEnvelope(
  { scope: 7, type_id: 1, entity_id: 42n, incarnation: 3, side: 1, method_id: 2,
    correlation_id: 0x1122334455667788n, hosting_generation: 5, deadline_ms: 250 },
  utf8('payload'))) !== canonicalBytes) throw new Error('client artifact envelope bytes drifted');
if (hex(mod.encodeRecord(mod.sprotoFields.Avatar, { level: 1 })) !== '020001000400')
  throw new Error('client artifact sproto bytes drifted');
if (mod.entityTypes.Avatar !== 1) throw new Error('client artifact entity table drifted');
console.log('JS_MLV1_CLIENT_CODEC envelope and sproto bytes byte-identical to the server artifact [PASS]');

// The digest is derived from the compiled contract, so it must equal the
// digest line the published lock pins — a client cannot claim a version its
// artifact was not built from.
if (typeof mod.CONTRACT_DIGEST !== 'string' || !/^[0-9a-f]{16}$/.test(mod.CONTRACT_DIGEST))
  throw new Error(`CONTRACT_DIGEST is not 16 lowercase hex: ${mod.CONTRACT_DIGEST}`);
const lock = readFileSync(join(dirname(modulePath), 'avatar.lock'), 'utf8');
const digestLine = lock.split('\n').find((lineText) => lineText.startsWith('# digest '));
if (!digestLine) throw new Error('avatar.lock carries no digest line');
const lockDigest = digestLine.slice('# digest '.length).trim();
if (lockDigest !== mod.CONTRACT_DIGEST)
  throw new Error(`CONTRACT_DIGEST ${mod.CONTRACT_DIGEST} disagrees with the lock digest ${lockDigest}`);
console.log(`JS_MLV1_CLIENT_DIGEST ${mod.CONTRACT_DIGEST} agrees with avatar.lock [PASS]`);

// The frame is exactly "MLV1 <digest>" — nothing padded, nothing terminated.
if (!bytesEqual(mod.handshakeFrame(), utf8('MLV1 ' + mod.CONTRACT_DIGEST)))
  throw new Error(`handshake frame drifted: ${hex(mod.handshakeFrame())}`);
console.log('JS_MLV1_CLIENT_FRAME bytes pinned [PASS]');

// Every host verdict parses: OK carries the host digest (which for a v2 host
// may differ from the client's own — the same bytes must parse either way),
// the three error codes carry no host.
const ok = mod.parseHandshakeReply(utf8('MLV1 OK ' + lockDigest));
if (!ok || ok.ok !== true || ok.host !== lockDigest) throw new Error('MLV1 OK verdict lost');
const okSelf = mod.parseHandshakeReply(utf8('MLV1 OK ' + mod.CONTRACT_DIGEST));
if (!okSelf || okSelf.ok !== true || okSelf.host !== mod.CONTRACT_DIGEST) throw new Error('self-digest OK verdict lost');
for (const code of ['INVALID', 'VERSION', 'LATE']) {
  const bad = mod.parseHandshakeReply(utf8('MLV1 ERR ' + code));
  if (!bad || bad.ok !== false || bad.error !== code || bad.host !== undefined)
    throw new Error(`MLV1 ERR ${code} verdict lost`);
}
console.log('JS_MLV1_CLIENT_REPLY OK and all three error verdicts exact [PASS]');

// Malformed replies refuse closed: wrong marker, bare prefix, short/long/
// uppercase/non-hex digests, trailing bytes, unknown error code, empty and
// invalid UTF-8 input, and a non-bytes argument.
const refusals = [
  utf8('MLV1'),
  utf8('MLV1 '),
  utf8('MLV1 ' + mod.CONTRACT_DIGEST.slice(0, 15)),
  utf8('MLV1 ' + mod.CONTRACT_DIGEST + '0'),
  utf8('MLV1 ' + '0123456789abcdeg'),
  utf8('MLV1 ' + mod.CONTRACT_DIGEST.toUpperCase()),
  utf8('MLV2 ' + mod.CONTRACT_DIGEST),
  utf8('MLV1 OK ' + lockDigest + ' '),
  utf8('MLV1 OK ' + lockDigest.toUpperCase()),
  utf8('MLV1 OK ' + lockDigest.slice(0, 15)),
  utf8('MLV1 ERR NOPE'),
  utf8('MLV1 ERR VERSION '),
  utf8(''),
  new Uint8Array([0xff, 0xfe]),
  'not bytes',
];
for (const bytes of refusals) {
  if (mod.parseHandshakeReply(bytes) !== null) throw new Error('malformed handshake reply accepted');
}
console.log('JS_MLV1_CLIENT_REFUSE malformed replies refuse closed [PASS]');
