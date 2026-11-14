import { afterAll, describe, expect, it } from "vitest";
import * as net from "node:net";
import { encodeFrame, FrameAccumulator } from "../src/frame.js";
import { MistlaneTransport, TransportError } from "../src/transport.js";
import type { Socket } from "node:net";

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

async function waitFor(predicate: () => boolean, timeoutMs = 5000): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (predicate()) return;
    await sleep(20);
  }
  throw new Error("condition not reached in time");
}

interface NodeHandle {
  port: number;
  connections: Socket[];
  refusedHandshakes: number;
  close: () => void;
}

// An in-process stand-in that speaks the verified server side of the session
// plane (src/session_plane.hpp): u32 BE frames, MLS1/MLC2 handshakes with
// unprefixed handshake replies, correlated replies echoing the u32 request id
// and entity replies echoing the u64 envelope correlation.
function startFakeNode(port: number, options: { token?: string } = {}): Promise<NodeHandle> {
  const handle: NodeHandle = { port, connections: [], refusedHandshakes: 0, close: () => {} };
  const server = net.createServer((socket) => {
    handle.connections.push(socket);
    const frames = new FrameAccumulator();
    let correlated = false;
    socket.on("data", (chunk) => {
      let decoded;
      try {
        decoded = frames.push(chunk);
      } catch {
        socket.destroy();
        return;
      }
      for (const frame of decoded) handleFrame(socket, frame.payload);
    });

    function handleFrame(socket: Socket, payload: Buffer): void {
      const text = payload.toString("utf8");
      if (text.startsWith("MLS1")) {
        const words = text.split(/\s+/);
        if (options.token === undefined) {
          handle.refusedHandshakes += 1;
          socket.write(encodeFrame("MLS1 UNEXPECTED"));
          return;
        }
        if (words.length !== 4 || words[3] !== options.token) {
          handle.refusedHandshakes += 1;
          socket.write(encodeFrame("MLS1 TOKEN"));
          return;
        }
        socket.write(encodeFrame("MLS1 OK"));
        return;
      }
      if (text === "MLC2") {
        correlated = true;
        socket.write(encodeFrame("MLC2 OK")); // unprefixed while correlation is still off
        return;
      }
      // Entity traffic: MLE1 marker at offset 0 (uncorrelated) or 4 (MLC2).
      const offset = text.startsWith("MLE1")
        ? 0
        : payload.length >= 8 && payload.subarray(4, 8).toString("utf8") === "MLE1" ? 4 : -1;
      if (offset < 0) {
        socket.write(encodeFrame("unknown"));
        return;
      }
      const envelopeBody = payload.subarray(offset + 4);
      const correlation = envelopeBody.readBigUInt64BE(23);
      const echoed = Buffer.alloc(8);
      echoed.writeBigUInt64BE(correlation, 0);
      const reply = Buffer.concat([Buffer.from("MLE1"), echoed, Buffer.from([0x00, 0x07])]);
      const framed = correlated
        ? Buffer.concat([Buffer.from(payload.subarray(0, 4)), reply]) // echo the u32 request id
        : reply;
      socket.write(encodeFrame(framed));
    }
  });
  return new Promise((resolve) => {
    server.listen(port, "127.0.0.1", () => {
      handle.close = () => {
        for (const socket of handle.connections) socket.destroy();
        handle.connections.length = 0;
        server.close();
      };
      resolve(handle);
    });
  });
}

// A node whose entity replies always echo a stale correlation id, to prove
// the client detects the mismatch (the engine never does this; a relay bug
// would).
function startWrongEchoNode(port: number): Promise<net.Server> {
  const server = net.createServer((socket) => {
    const frames = new FrameAccumulator();
    socket.on("data", (chunk) => {
      for (const frame of frames.push(chunk)) {
        const stale = Buffer.alloc(8);
        stale.writeBigUInt64BE(999n, 0);
        socket.write(encodeFrame(Buffer.concat([
          Buffer.from("MLE1"), stale, Buffer.from([0x00]),
        ])));
      }
    });
  });
  return new Promise((resolve) => server.listen(port, "127.0.0.1", () => resolve(server)));
}

const PORT = 19901;
const WRONG_ECHO_PORT = 19955;
const closers: Array<() => void> = [];

afterAll(() => {
  for (const close of closers) close();
});

describe("MistlaneTransport against the session plane", () => {
  it("rejects a bad port up front", () => {
    expect(() => new MistlaneTransport({ host: "127.0.0.1", port: 0 })).toThrow(TransportError);
  });

  it("presents MLS1 and completes entity requests uncorrelated", async () => {
    const node = await startFakeNode(PORT, { token: "sekret" });
    closers.push(node.close);
    const transport = new MistlaneTransport({
      host: "127.0.0.1",
      port: PORT,
      identity: { role: "devtools", instance: "ide-1", token: "sekret" },
      autoReconnect: false,
    });
    closers.push(() => transport.close());
    const states: string[] = [];
    transport.on("state", (state) => states.push(state));
    await transport.connect();
    expect(transport.state).toBe("ready");
    expect(node.refusedHandshakes).toBe(0);

    const reply = await transport.callMethod(
      { scope: 1, typeId: 2, entityId: 7n, incarnation: 3, hostingGeneration: 0 },
      4,
      Buffer.from([0xaa]),
    );
    expect(reply.isError).toBe(false);
    expect(Buffer.from(reply.payload).toString("hex")).toBe("07");
    expect(states).toContain("ready");

    transport.close();
    expect(transport.state).toBe("closed");
  });

  it("fails fast when the node refuses the identity", async () => {
    await startFakeNode(PORT + 1, { token: "right-token" });
    const transport = new MistlaneTransport({
      host: "127.0.0.1",
      port: PORT + 1,
      identity: { role: "devtools", instance: "ide-1", token: "wrong-token" },
      autoReconnect: false,
    });
    closers.push(() => transport.close());
    await expect(transport.connect()).rejects.toThrow(/handshake rejected: MLS1 TOKEN|connection closed/);
    transport.close();
  });

  it("surfaces a tokenless node refusing MLS1 as UNEXPECTED", async () => {
    await startFakeNode(PORT + 2);
    const transport = new MistlaneTransport({
      host: "127.0.0.1",
      port: PORT + 2,
      identity: { role: "devtools", instance: "ide-1", token: "t" },
      autoReconnect: false,
    });
    closers.push(() => transport.close());
    await expect(transport.connect()).rejects.toThrow(/UNEXPECTED|connection closed/);
    transport.close();
  });

  it("correlates concurrent MLC2 requests by transport id", async () => {
    await startFakeNode(PORT + 3, { token: "t" });
    const transport = new MistlaneTransport({
      host: "127.0.0.1",
      port: PORT + 3,
      identity: { role: "devtools", instance: "ide-1", token: "t" },
      correlated: true,
      autoReconnect: false,
    });
    closers.push(() => transport.close());
    await transport.connect();
    const slow = transport.callMethod(
      { scope: 1, typeId: 2, entityId: 1n, incarnation: 1, hostingGeneration: 0 }, 1);
    const fast = transport.callMethod(
      { scope: 1, typeId: 2, entityId: 2n, incarnation: 1, hostingGeneration: 0 }, 2);
    const [a, b] = await Promise.all([slow, fast]);
    expect(a.isError).toBe(false);
    expect(b.isError).toBe(false);
    transport.close();
  });

  it("emits correlated pushes (transport id 0) as events", async () => {
    const node = await startFakeNode(PORT + 4, { token: "t" });
    closers.push(node.close);
    const transport = new MistlaneTransport({
      host: "127.0.0.1",
      port: PORT + 4,
      identity: { role: "devtools", instance: "ide-1", token: "t" },
      correlated: true,
      autoReconnect: false,
    });
    closers.push(() => transport.close());
    await transport.connect();
    const pushes: Buffer[] = [];
    transport.on("push", (payload: Buffer) => pushes.push(payload));
    const socket = node.connections[node.connections.length - 1]!;
    socket.write(encodeFrame((() => {
      const body = Buffer.from("MLP1 push");
      const out = Buffer.alloc(4 + body.length);
      out.writeUInt32BE(0, 0);
      body.copy(out, 4);
      return out;
    })()));
    await waitFor(() => pushes.length > 0);
    expect(pushes[0]!.toString("utf8")).toBe("MLP1 push");
    transport.close();
  });

  it("detects a reply whose correlation does not echo the request", async () => {
    const server = await startWrongEchoNode(WRONG_ECHO_PORT);
    closers.push(() => server.close());
    const transport = new MistlaneTransport({
      host: "127.0.0.1",
      port: WRONG_ECHO_PORT,
      autoReconnect: false,
    });
    closers.push(() => transport.close());
    await transport.connect();
    await expect(
      transport.entityRequest({
        scope: 1, typeId: 2, entityId: 7n, incarnation: 3, side: 1, methodId: 4,
        correlationId: 42n, hostingGeneration: 0, deadlineMs: 0, payload: Buffer.alloc(0),
      }),
    ).rejects.toThrow(/does not echo request 42/);
    transport.close();
  });

  it("reconnects with backoff after the node restarts", async () => {
    const node = await startFakeNode(PORT + 5, { token: "t" });
    node.close();
    const transport = new MistlaneTransport({
      host: "127.0.0.1",
      port: PORT + 5,
      identity: { role: "devtools", instance: "ide-1", token: "t" },
      backoff: { baseMs: 60, factor: 1, maxMs: 60 },
    });
    closers.push(() => transport.close());
    const backoffs: number[] = [];
    transport.on("backoff", (delay) => backoffs.push(delay));

    transport.connect().catch(() => {});
    // The node is gone: connect fails, the transport parks in backoff and
    // retries until the node returns.
    await waitFor(() => backoffs.length >= 1);

    const revived = await startFakeNode(PORT + 5, { token: "t" });
    closers.push(revived.close);
    await waitFor(() => transport.state === "ready", 8000);
    const reply = await transport.callMethod(
      { scope: 1, typeId: 2, entityId: 9n, incarnation: 1, hostingGeneration: 0 }, 1);
    expect(reply.isError).toBe(false);
    transport.close();
  });

  it("times a request out when the node never answers", async () => {
    const silent = net.createServer((socket) => { socket.on("data", () => {}); });
    await new Promise<void>((resolve) => silent.listen(PORT + 6, "127.0.0.1", resolve));
    closers.push(() => silent.close());
    const transport = new MistlaneTransport({
      host: "127.0.0.1",
      port: PORT + 6,
      requestTimeoutMs: 120,
      autoReconnect: false,
    });
    closers.push(() => transport.close());
    await transport.connect();
    await expect(transport.callMethod(
      { scope: 1, typeId: 2, entityId: 1n, incarnation: 1, hostingGeneration: 0 }, 1,
    )).rejects.toThrow(/timed out/);
    transport.close();
  });
});
