// SPDX-License-Identifier: Apache-2.0
import { pathToFileURL } from 'node:url';

const modulePath = process.argv[2];
if (!modulePath) throw new Error('usage: node sproto_sdk_golden.mjs GENERATED.bindings.js');
const { encodeScalar, sprotoFields } = await import(pathToFileURL(modulePath));
const bytes = encodeScalar(sprotoFields.Avatar, { level: 1 });
const hex = [...bytes].map((byte) => byte.toString(16).padStart(2, '0')).join('');
const expected = '020001000400';
if (hex !== expected) throw new Error(`JS sproto vector mismatch: ${hex} != ${expected}`);
console.log(`JS_SPROTO_SDK_GOLDEN level=1 hex=${hex} [PASS]`);
