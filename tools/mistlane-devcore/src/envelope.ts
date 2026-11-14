// Entity runtime protocol decode, mirrored from the engine
// (include/mistlane/entity_envelope.hpp and include/mistlane/entity_dispatch.hpp):
//
//   request: "MLE1" | envelope (39 bytes, big-endian) | payload
//   reply:   "MLE1" | correlation u64 BE | EntityReply
//
// The wire carries entity_id before incarnation even though the C++ struct
// declares incarnation first (docs/business-bot.md "never positionally
// initialize the struct"); the encode/decode order below is the wire order.
// A correlated (MLC2) session prefixes every frame payload with a u32 BE
// request id (src/session_plane.hpp correlated_frame), replies echo the id and
// pushes carry id 0, so entity traffic through such a session has its MLE1
// marker at offset 4 (entity_frame_offset).

export const MLE1 = "MLE1";
export const ENVELOPE_HEADER = 39;
export const MLC2_PREFIX = 4;
export const ENTITY_REQUEST_CORRELATION_AT = 4 + 23;
export const ENTITY_REPLY_CORRELATION_AT = 4;

export const ENTITY_SIDE_CLIENT = 0;
export const ENTITY_SIDE_SERVER = 1;

export type EntitySide = typeof ENTITY_SIDE_CLIENT | typeof ENTITY_SIDE_SERVER;

/** Error codes in declaration order (include/mistlane/entity_dispatch.hpp:13). */
export enum EntityError {
  INVALID = 0,
  UNROUTED = 1,
  STALE = 2,
  FORBIDDEN = 3,
  TIMEOUT = 4,
  CANCELLED = 5,
  NO_IMPLEMENTATION = 6,
  NOT_READY = 7,
  CAPACITY = 8,
  CONFLICT = 9,
}

export function entityErrorName(code: number): string | undefined {
  return EntityError[code] as string | undefined;
}

export interface EntityEnvelope {
  scope: number;
  typeId: number;
  entityId: bigint | number;
  incarnation: number;
  side: EntitySide;
  methodId: number;
  correlationId: bigint | number;
  hostingGeneration: number;
  deadlineMs: number;
  payload: Uint8Array;
}

class BeWriter {
  private readonly bytes: number[] = [];
  push(value: bigint | number, width: number): void {
    const v = typeof value === "bigint" ? value : BigInt(value);
    for (let shift = width * 8; shift > 0; ) {
      shift -= 8;
      this.bytes.push(Number((v >> BigInt(shift)) & 0xffn));
    }
  }
  raw(data: Uint8Array): void {
    for (const byte of data) this.bytes.push(byte);
  }
  toBuffer(): Buffer {
    return Buffer.from(this.bytes);
  }
}

class BeReader {
  offset = 0;
  constructor(private readonly bytes: Uint8Array) {}
  get remaining(): number {
    return this.bytes.length - this.offset;
  }
  read(width: number): bigint | null {
    if (this.remaining < width) return null;
    let value = 0n;
    for (let i = 0; i < width; i += 1) {
      value = (value << 8n) | BigInt(this.bytes[this.offset + i]!);
    }
    this.offset += width;
    return value;
  }
}

/**
 * Encodes the 39-byte envelope (no marker, no frame). Byte layout, all
 * big-endian: scope u32 | type_id u32 | entity_id i64 | incarnation u32 |
 * side u8 | method_id u16 | correlation_id u64 | hosting_generation u32 |
 * deadline_ms u32.
 */
export function encodeEnvelope(envelope: EntityEnvelope): Buffer {
  const out = new BeWriter();
  out.push(envelope.scope, 4);
  out.push(envelope.typeId, 4);
  out.push(envelope.entityId, 8);
  out.push(envelope.incarnation, 4);
  out.push(envelope.side, 1);
  out.push(envelope.methodId, 2);
  out.push(envelope.correlationId, 8);
  out.push(envelope.hostingGeneration, 4);
  out.push(envelope.deadlineMs, 4);
  out.raw(envelope.payload);
  return out.toBuffer();
}

/** Decodes the 39-byte envelope plus payload; null when malformed or short. */
export function decodeEnvelope(bytes: Uint8Array): EntityEnvelope | null {
  if (bytes.length < ENVELOPE_HEADER) return null;
  const reader = new BeReader(bytes);
  const scope = reader.read(4)!;
  const typeId = reader.read(4)!;
  const entityId = reader.read(8)!;
  const incarnation = reader.read(4)!;
  const side = reader.read(1)!;
  const methodId = reader.read(2)!;
  const correlationId = reader.read(8)!;
  const hostingGeneration = reader.read(4)!;
  const deadlineMs = reader.read(4)!;
  if (side > 1n) return null;
  return {
    scope: Number(scope),
    typeId: Number(typeId),
    entityId: entityId,
    incarnation: Number(incarnation),
    side: Number(side) as EntitySide,
    methodId: Number(methodId),
    correlationId: correlationId,
    hostingGeneration: Number(hostingGeneration),
    deadlineMs: Number(deadlineMs),
    payload: bytes.subarray(ENVELOPE_HEADER),
  };
}

/** One request frame: "MLE1" | envelope | payload (uncorrelated session). */
export function encodeEntityRequest(envelope: EntityEnvelope): Buffer {
  const body = encodeEnvelope(envelope);
  return Buffer.concat([Buffer.from(MLE1, "utf8"), body]);
}

export interface EntityReply {
  isError: boolean;
  error?: EntityError;
  reason?: string;
  payload: Buffer;
}

/**
 * EntityReply decode (entity_envelope.hpp decode_entity_reply): kind 0 is
 * `0x00 | returns payload`; kind 1 is `0x01 | u8 error code` and only INVALID
 * (code 0) may append `u16 BE length | reason`. Unknown kinds or codes are
 * malformed (null), not mapped.
 */
export function decodeEntityReply(bytes: Uint8Array): EntityReply | null {
  if (bytes.length < 1) return null;
  const kind = bytes[0]!;
  if (kind === 0x00) return { isError: false, payload: Buffer.from(bytes.subarray(1)) };
  if (kind !== 0x01 || bytes.length < 2) return null;
  const code = bytes[1]!;
  if (code > 9) return null;
  if (code === EntityError.INVALID) {
    const reader = new BeReader(bytes.subarray(2));
    const width = reader.read(2);
    if (width === null) return null;
    if (Number(width) !== reader.remaining) return null;
    return {
      isError: true,
      error: EntityError.INVALID,
      reason: Buffer.from(bytes.subarray(4)).toString("utf8"),
      payload: Buffer.alloc(0),
    };
  }
  if (bytes.length !== 2) return null;
  return { isError: true, error: code as EntityError, payload: Buffer.alloc(0) };
}

export interface EntityResponse {
  correlationId: bigint;
  reply: EntityReply;
}

/**
 * Decodes an uncorrelated MLE1 reply frame payload:
 * "MLE1" | correlation u64 BE (bytes [4,12)) | EntityReply. This is the shape
 * the scenario bot validates (src/business_plan.hpp business_step).
 */
export function decodeEntityResponse(frame: Uint8Array): EntityResponse | null {
  if (frame.length < 12 || Buffer.from(frame.subarray(0, 4)).toString("utf8") !== MLE1) return null;
  let correlation = 0n;
  for (let i = 4; i < 12; i += 1) correlation = (correlation << 8n) | BigInt(frame[i]!);
  const reply = decodeEntityReply(frame.subarray(12));
  if (!reply) return null;
  return { correlationId: correlation, reply };
}

/**
 * Correlated (MLC2) session framing (src/session_plane.hpp correlated_frame /
 * frame_id): every frame payload is `u32 BE request id | payload`; pushes
 * carry id 0.
 */
export function encodeCorrelatedFrame(requestId: number, payload: Uint8Array): Buffer {
  const out = Buffer.alloc(MLC2_PREFIX + payload.length);
  out.writeUInt32BE(requestId >>> 0, 0);
  Buffer.from(payload).copy(out, MLC2_PREFIX);
  return out;
}

export function decodeCorrelatedFrame(payload: Uint8Array): { requestId: number; body: Buffer } | null {
  if (payload.length < MLC2_PREFIX) return null;
  return {
    requestId: Buffer.from(payload.subarray(0, MLC2_PREFIX)).readUInt32BE(0),
    body: Buffer.from(payload.subarray(MLC2_PREFIX)),
  };
}

/** MLS1 service identity hello: exactly four words (src/node_internal.hpp:28). */
export function encodeServiceHello(role: string, instance: string, token: string): string {
  for (const word of [role, instance, token]) {
    if (/\s/.test(word)) throw new Error(`MLS1 identity words cannot contain whitespace: ${JSON.stringify(word)}`);
  }
  return `MLS1 ${role} ${instance} ${token}`;
}
