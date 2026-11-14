import { describe, expect, it } from "vitest";
import { FrameAccumulator, MAX_FRAME, OversizeFrameError, encodeFrame } from "../src/frame.js";

describe("frame codec", () => {
  it("round-trips a payload", () => {
    const frame = encodeFrame("MLS1 OK");
    expect(frame.subarray(0, 4).readUInt32BE(0)).toBe(7);
    expect(frame.subarray(4).toString("utf8")).toBe("MLS1 OK");
    const decoded = new FrameAccumulator().push(frame);
    expect(decoded).toHaveLength(1);
    expect(decoded[0]!.payload.toString("utf8")).toBe("MLS1 OK");
    expect(decoded[0]!.length).toBe(7);
  });

  it("encodes an empty payload", () => {
    const decoded = new FrameAccumulator().push(encodeFrame(""));
    expect(decoded[0]!.length).toBe(0);
    expect(decoded[0]!.payload.length).toBe(0);
  });

  it("encodes binary payloads unchanged", () => {
    const payload = Buffer.from([0x00, 0xff, 0x10, 0x00, 0x01]);
    const decoded = new FrameAccumulator().push(encodeFrame(payload));
    expect(decoded[0]!.payload.equals(payload)).toBe(true);
  });

  it("re-assembles a frame split at every byte boundary", () => {
    const frame = encodeFrame("MLE1" + "x".repeat(64));
    for (const sizes of [[1], [4, 4], [2, 40], [65, 5]]) {
      const accumulator = new FrameAccumulator();
      const all: ReturnType<FrameAccumulator["push"]> = [];
      let offset = 0;
      for (const size of sizes!) {
        all.push(...accumulator.push(frame.subarray(offset, offset + size)));
        offset += size;
      }
      all.push(...accumulator.push(frame.subarray(offset)));
      expect(all).toHaveLength(1);
      expect(all[0]!.payload.toString("utf8")).toBe("MLE1" + "x".repeat(64));
    }
  });

  it("decodes several frames delivered in one chunk", () => {
    const chunk = Buffer.concat([encodeFrame("a"), encodeFrame("bb"), encodeFrame("ccc")]);
    const decoded = new FrameAccumulator().push(chunk);
    expect(decoded.map((frame) => frame.payload.toString("utf8"))).toEqual(["a", "bb", "ccc"]);
  });

  it("rejects an oversize length like the engine rejects the connection", () => {
    const header = Buffer.alloc(4);
    header.writeUInt32BE(MAX_FRAME + 1, 0);
    expect(() => new FrameAccumulator().push(header)).toThrow(OversizeFrameError);
  });

  it("refuses to encode beyond the 1 MiB bound", () => {
    expect(() => encodeFrame(Buffer.alloc(MAX_FRAME + 1))).toThrow(OversizeFrameError);
    expect(() => encodeFrame(Buffer.alloc(MAX_FRAME))).not.toThrow();
  });

  it("keeps buffering a partial header", () => {
    const accumulator = new FrameAccumulator();
    expect(accumulator.push(encodeFrame("hello").subarray(0, 2))).toHaveLength(0);
    expect(accumulator.buffered).toBe(2);
    const decoded = accumulator.push(encodeFrame("hello").subarray(2));
    expect(decoded).toHaveLength(1);
    expect(decoded[0]!.payload.toString("utf8")).toBe("hello");
    expect(accumulator.buffered).toBe(0);
  });
});
