import { describe, expect, it } from "vitest";
import {
  ENVELOPE_HEADER,
  EntityError,
  decodeCorrelatedFrame,
  decodeEntityReply,
  decodeEntityResponse,
  decodeEnvelope,
  encodeCorrelatedFrame,
  encodeEntityRequest,
  encodeEnvelope,
  encodeServiceHello,
} from "../src/envelope.js";

const hex = (bytes: Uint8Array): string => Buffer.from(bytes).toString("hex");

describe("MLE1 envelope codec (include/mistlane/entity_envelope.hpp)", () => {
  it("encodes the 39-byte header in wire order (entity_id before incarnation)", () => {
    const encoded = encodeEnvelope({
      scope: 1,
      typeId: 2,
      entityId: 7n,
      incarnation: 3,
      side: 1,
      methodId: 4,
      correlationId: 0x0102030405060708n,
      hostingGeneration: 5,
      deadlineMs: 250,
      payload: Buffer.from("ab"),
    });
    expect(encoded.length).toBe(ENVELOPE_HEADER + 2);
    expect(hex(encoded)).toBe(
      "00000001" + // scope u32
        "00000002" + // type_id u32
        "0000000000000007" + // entity_id i64
        "00000003" + // incarnation u32
        "01" + // side u8 (server)
        "0004" + // method_id u16
        "0102030405060708" + // correlation_id u64
        "00000005" + // hosting_generation u32
        "000000fa" + // deadline_ms u32
        "6162", // payload
    );
  });

  it("round-trips every field including full-width i64 addresses", () => {
    const envelope = {
      scope: 0xffffffff,
      typeId: 65535,
      entityId: 0x7fffffffffffffffn,
      incarnation: 1,
      side: 0 as const,
      methodId: 65535,
      correlationId: 0xffffffffffffffffn,
      hostingGeneration: 42,
      deadlineMs: 60000,
      payload: Buffer.from([0, 1, 2, 254, 255]),
    };
    const decoded = decodeEnvelope(encodeEnvelope(envelope))!;
    expect(decoded.scope).toBe(envelope.scope);
    expect(decoded.typeId).toBe(envelope.typeId);
    expect(decoded.entityId).toBe(envelope.entityId);
    expect(decoded.incarnation).toBe(envelope.incarnation);
    expect(decoded.side).toBe(envelope.side);
    expect(decoded.methodId).toBe(envelope.methodId);
    expect(decoded.correlationId).toBe(envelope.correlationId);
    expect(decoded.hostingGeneration).toBe(envelope.hostingGeneration);
    expect(decoded.deadlineMs).toBe(envelope.deadlineMs);
    expect(Buffer.from(decoded.payload).equals(Buffer.from(envelope.payload))).toBe(true);
  });

  it("rejects short input and side values above 1", () => {
    expect(decodeEnvelope(Buffer.alloc(ENVELOPE_HEADER - 1))).toBeNull();
    const badSide = encodeEnvelope({ ...baseEnvelope(), side: 1 });
    badSide[20] = 2;
    expect(decodeEnvelope(badSide)).toBeNull();
  });

  it("builds a request with the MLE1 marker in front", () => {
    const frame = encodeEntityRequest(baseEnvelope());
    expect(frame.subarray(0, 4).toString("utf8")).toBe("MLE1");
    expect(frame.length).toBe(4 + ENVELOPE_HEADER + 2);
  });
});

describe("EntityReply codec", () => {
  it("decodes an OK reply as kind 0 plus returns payload", () => {
    const reply = decodeEntityReply(Buffer.from([0x00, 0xaa, 0xbb]))!;
    expect(reply.isError).toBe(false);
    expect(hex(reply.payload)).toBe("aabb");
  });

  it("decodes every declared error code 0..9 and rejects 10", () => {
    for (let code = 1; code <= 9; code += 1) {
      const reply = decodeEntityReply(Buffer.from([0x01, code]))!;
      expect(reply.isError).toBe(true);
      expect(reply.error).toBe(code);
    }
    // INVALID (0) carries the reason width; an empty reason is width zero.
    const invalid = decodeEntityReply(Buffer.from([0x01, 0x00, 0x00, 0x00]))!;
    expect(invalid.error).toBe(EntityError.INVALID);
    expect(invalid.reason).toBe("");
    expect(decodeEntityReply(Buffer.from([0x01, 0x00]))).toBeNull(); // width missing
    expect(decodeEntityReply(Buffer.from([0x01, 10]))).toBeNull();
    expect(decodeEntityReply(Buffer.from([0x02]))).toBeNull();
    expect(decodeEntityReply(Buffer.alloc(0))).toBeNull();
  });

  it("round-trips an INVALID reason with a u16 BE width", () => {
    const reason = "no such entity";
    const width = Buffer.alloc(2);
    width.writeUInt16BE(reason.length, 0);
    const bytes = Buffer.concat([Buffer.from([0x01, 0x00]), width, Buffer.from(reason)]);
    const reply = decodeEntityReply(bytes)!;
    expect(reply.error).toBe(EntityError.INVALID);
    expect(reply.reason).toBe(reason);
  });

  it("rejects INVALID replies whose width disagrees with the bytes present", () => {
    const width = Buffer.alloc(2);
    width.writeUInt16BE(3, 0);
    const short = Buffer.concat([Buffer.from([0x01, 0x00]), width, Buffer.from("ab")]);
    expect(decodeEntityReply(short)).toBeNull();
  });

  it("rejects non-INVALID errors that carry extra bytes", () => {
    expect(decodeEntityReply(Buffer.from([0x01, 0x02, 0x00]))).toBeNull();
  });
});

describe("MLE1 reply decode (src/business_plan.hpp business_step)", () => {
  it("extracts the echoed correlation from bytes [4,12)", () => {
    const correlation = Buffer.alloc(8);
    correlation.writeBigUInt64BE(77n, 0);
    const frame = Buffer.concat([Buffer.from("MLE1"), correlation, Buffer.from([0x00, 0x01, 0x02])]);
    const response = decodeEntityResponse(frame)!;
    expect(response.correlationId).toBe(77n);
    expect(response.reply.isError).toBe(false);
    expect(hex(response.reply.payload)).toBe("0102");
  });

  it("refuses frames without the marker or shorter than 12 bytes", () => {
    expect(decodeEntityResponse(Buffer.from("MLX1whatever"))).toBeNull();
    expect(decodeEntityResponse(Buffer.alloc(11))).toBeNull();
  });
});

describe("MLC2 correlated framing (src/session_plane.hpp correlated_frame)", () => {
  it("prefixes and unprefixedes a u32 BE request id", () => {
    const frame = encodeCorrelatedFrame(0x0102a0b0, Buffer.from("MLE1x"));
    expect(frame.readUInt32BE(0)).toBe(0x0102a0b0);
    const decoded = decodeCorrelatedFrame(frame)!;
    expect(decoded.requestId).toBe(0x0102a0b0);
    expect(decoded.body.toString("utf8")).toBe("MLE1x");
  });

  it("treats id 0 as the push channel", () => {
    const decoded = decodeCorrelatedFrame(encodeCorrelatedFrame(0, Buffer.from("push")))!;
    expect(decoded.requestId).toBe(0);
  });

  it("returns null for bodies shorter than the id prefix", () => {
    expect(decodeCorrelatedFrame(Buffer.from([0, 0]))).toBeNull();
  });
});

describe("MLS1 service hello (src/node_internal.hpp service_fields)", () => {
  it("is exactly four words", () => {
    expect(encodeServiceHello("bot", "bot-1", "secret")).toBe("MLS1 bot bot-1 secret");
  });

  it("refuses whitespace inside identity words", () => {
    expect(() => encodeServiceHello("a b", "i", "t")).toThrow();
    expect(() => encodeServiceHello("a", "i", "t t")).toThrow();
  });
});

function baseEnvelope() {
  return {
    scope: 1,
    typeId: 2,
    entityId: 7n,
    incarnation: 3,
    side: 1 as const,
    methodId: 4,
    correlationId: 0x0102030405060708n,
    hostingGeneration: 5,
    deadlineMs: 250,
    payload: Buffer.from("ab"),
  };
}
