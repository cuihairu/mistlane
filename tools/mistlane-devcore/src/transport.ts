// A client-side transport for the mistlane session plane, mirroring the
// client precedents in the engine tree (src/cli_client.cpp TcpClient,
// src/bot.cpp business scenario, src/session_plane.hpp server side):
//
//   frame:      u32 BE length | payload (1 MiB bound)
//   handshake:  "MLS1 <role> <instance> <token>" -> "MLS1 OK"   (only when the
//               node started with --service-token; the accepted peer's frames
//               are attributed caller = server)
//   correlate:  "MLC2" -> "MLC2 OK"; every later frame payload then carries a
//               u32 BE request id, replies echo the id and pushes carry id 0
//   entity:     request "MLE1" | envelope(39B BE) | payload
//               reply   "MLE1" | correlation u64 BE | EntityReply
//               (through a correlated session both sides additionally carry
//               the u32 transport id; the MLE1 marker sits at offset 4)
//
// This core never speaks for production traffic; it exists so IDE tooling can
// probe a node's session plane with the same bytes the shipped CLI uses.

import { EventEmitter } from "node:events";
import * as net from "node:net";
import { FrameAccumulator, MAX_FRAME, encodeFrame, OversizeFrameError } from "./frame.js";
import {
  EntityEnvelope,
  EntityReply,
  MLE1,
  decodeCorrelatedFrame,
  decodeEntityResponse,
  encodeCorrelatedFrame,
  encodeEntityRequest,
  encodeServiceHello,
} from "./envelope.js";
import { BackoffOptions, ReconnectBackoff } from "./backoff.js";

export type TransportState = "idle" | "connecting" | "ready" | "backoff" | "closed";

export interface TransportIdentity {
  role: string;
  instance: string;
  token: string;
}

export interface TransportOptions {
  host: string;
  port: number;
  /** Present MLS1 first; required when the node runs with --service-token. */
  identity?: TransportIdentity;
  /** Perform the MLC2 handshake and prefix frames with request ids. Default false. */
  correlated?: boolean;
  connectTimeoutMs?: number;
  requestTimeoutMs?: number;
  backoff?: BackoffOptions;
  /** Reconnect with backoff after an unexpected close. Default true. */
  autoReconnect?: boolean;
}

export interface PendingRequest {
  resolve: (payload: Buffer) => void;
  reject: (error: Error) => void;
  timer: NodeJS.Timeout;
}

interface HandshakeStep {
  payload: string;
  expectedReply: string;
  waiter: { resolve: () => void; reject: (error: Error) => void; timer: NodeJS.Timeout };
}

export class TransportError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "TransportError";
  }
}

export class MistlaneTransport extends EventEmitter {
  private readonly host: string;
  private readonly port: number;
  private readonly identity?: TransportIdentity;
  private readonly correlated: boolean;
  private readonly connectTimeoutMs: number;
  private readonly requestTimeoutMs: number;
  private readonly autoReconnect: boolean;
  private readonly backoff: ReconnectBackoff;
  private readonly frames = new FrameAccumulator();
  private socket: net.Socket | null = null;
  private stateValue: TransportState = "idle";
  /** Outstanding correlated requests by transport id (MLC2 mode). */
  private readonly pending = new Map<number, PendingRequest>();
  /** The single outstanding request on an uncorrelated (serial) connection. */
  private uncorrelated: PendingRequest | null = null;
  private nextRequestId = 1;
  private nextCorrelationId = 1n;
  private reconnectTimer: NodeJS.Timeout | null = null;
  private handshakeDone = false;
  private closing = false;
  /** Handshake words in flight, answered in order; cursor marks the next one. */
  private handshakeSteps: HandshakeStep[] = [];
  private handshakeCursor = 0;
  private handshakeWait: { resolve: () => void; reject: (error: Error) => void } | null = null;

  constructor(options: TransportOptions) {
    super();
    if (!(options.port > 0 && options.port <= 65535)) throw new TransportError("invalid port");
    this.host = options.host;
    this.port = options.port;
    this.identity = options.identity;
    this.correlated = options.correlated === true;
    this.connectTimeoutMs = options.connectTimeoutMs ?? 5000;
    this.requestTimeoutMs = options.requestTimeoutMs ?? 5000;
    this.autoReconnect = options.autoReconnect !== false;
    this.backoff = new ReconnectBackoff(options.backoff);
  }

  get state(): TransportState {
    return this.stateValue;
  }

  private setState(state: TransportState): void {
    if (this.stateValue === state) return;
    this.stateValue = state;
    this.emit("state", state);
  }

  /** Opens the connection and completes the MLS1/MLC2 handshakes. */
  connect(): Promise<void> {
    if (this.socket) return Promise.resolve();
    this.closing = false;
    this.setState("connecting");
    return new Promise<void>((resolve, reject) => {
      this.handshakeWait = { resolve: () => { this.handshakeWait = null; resolve(); },
                             reject: (error) => { this.handshakeWait = null; reject(error); } };
      this.openSocket();
    });
  }

  private openSocket(): void {
    const socket = net.createConnection({ host: this.host, port: this.port });
    socket.setNoDelay(true);
    this.socket = socket;
    const connectTimer = setTimeout(() => {
      socket.destroy(new TransportError(`connect timed out after ${this.connectTimeoutMs}ms`));
    }, this.connectTimeoutMs);

    socket.once("connect", () => {
      clearTimeout(connectTimer);
      this.handshakeDone = false;
      this.runHandshake();
    });
    socket.on("data", (chunk: Buffer) => this.onData(chunk));
    socket.once("error", (error: Error) => this.onLinkFailure(error));
    socket.once("close", () => this.onLinkFailure(new TransportError("connection closed")));
  }

  private handshakePlan(): string[] {
    const steps: string[] = [];
    if (this.identity) steps.push(encodeServiceHello(this.identity.role, this.identity.instance, this.identity.token));
    if (this.correlated) steps.push("MLC2");
    return steps;
  }

  private runHandshake(): void {
    const plan = this.handshakePlan();
    if (plan.length === 0) return this.finishHandshake();
    this.handshakeSteps = plan.map((payload) => ({
      payload,
      expectedReply: `${payload === "MLC2" ? "MLC2" : "MLS1"} OK`,
      waiter: { resolve: () => {}, reject: () => {}, timer: null as unknown as NodeJS.Timeout },
    }));
    this.handshakeCursor = 0;
    this.sendHandshakeStep();
  }

  private finishHandshake(): void {
    this.handshakeSteps = []; // later frames are business traffic, not handshakes
    this.handshakeCursor = 0;
    this.handshakeDone = true;
    this.backoff.reset();
    this.setState("ready");
    if (this.handshakeWait) this.handshakeWait.resolve();
  }

  private sendHandshakeStep(): void {
    const step = this.handshakeSteps[this.handshakeCursor]!;
    const timer = setTimeout(() => {
      step.waiter.reject(new TransportError(`handshake timed out after ${this.requestTimeoutMs}ms`));
    }, this.requestTimeoutMs);
    step.waiter.timer = timer;
    // onFrame advances the cursor on a matching reply, then resolves.
    step.waiter.resolve = () => {
      clearTimeout(timer);
      if (this.handshakeCursor >= this.handshakeSteps.length) return this.finishHandshake();
      this.sendHandshakeStep();
    };
    step.waiter.reject = (error) => {
      clearTimeout(timer);
      this.socket?.destroy(new TransportError(`handshake rejected: ${error.message}`));
    };
    this.socket!.write(encodeFrame(step.payload));
  }

  /** Sends one frame and awaits the reply payload (correlation handled). */
  request(payload: Uint8Array | string, timeoutMs?: number): Promise<Buffer> {
    if (this.stateValue !== "ready") return Promise.reject(new TransportError("transport is not ready"));
    const body = typeof payload === "string" ? Buffer.from(payload, "utf8") : Buffer.from(payload);
    if (body.length > MAX_FRAME) return Promise.reject(new OversizeFrameError(body.length));
    const timeout = timeoutMs ?? this.requestTimeoutMs;

    if (this.correlated && this.handshakeDone) {
      const id = this.nextRequestId;
      this.nextRequestId = (this.nextRequestId + 1) >>> 0 || 1; // id 0 belongs to pushes
      return new Promise<Buffer>((resolve, reject) => {
        const timer = setTimeout(() => {
          this.pending.delete(id);
          reject(new TransportError(`request timed out after ${timeout}ms`));
        }, timeout);
        this.pending.set(id, { resolve, reject, timer });
        this.socket!.write(encodeFrame(encodeCorrelatedFrame(id, body)));
      });
    }

    if (this.uncorrelated) return Promise.reject(new TransportError("a request is already in flight"));
    return new Promise<Buffer>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.uncorrelated = null;
        reject(new TransportError(`request timed out after ${timeout}ms`));
      }, timeout);
      this.uncorrelated = { resolve, reject, timer };
      this.socket!.write(encodeFrame(body));
    });
  }

  /**
   * One MLE1 entity round trip: assigns the envelope correlation, sends
   * "MLE1"|envelope|payload, and validates that the reply echoes the
   * correlation (payload bytes [4,12)), the shape the scenario bot validates
   * (src/business_plan.hpp business_step). On a correlated session request()
   * has already consumed the u32 transport id prefix, so the answer here is
   * the raw "MLE1" reply frame.
   */
  entityRequest(envelope: EntityEnvelope, timeoutMs?: number): Promise<EntityReply> {
    if (typeof envelope.correlationId !== "bigint") {
      envelope = { ...envelope, correlationId: BigInt(envelope.correlationId) };
    }
    if (envelope.correlationId === 0n) {
      return Promise.reject(new TransportError("probe requests need a nonzero correlation id"));
    }
    const correlation = envelope.correlationId;
    return this.request(encodeEntityRequest(envelope), timeoutMs).then((payload) => {
      if (payload.length < 4 || payload.subarray(0, 4).toString("utf8") !== MLE1) {
        throw new TransportError("reply is not an MLE1 frame");
      }
      const response = decodeEntityResponse(payload);
      if (!response) throw new TransportError("malformed MLE1 reply");
      if (response.correlationId !== correlation) {
        throw new TransportError(
          `reply correlation ${response.correlationId} does not echo request ${correlation}`,
        );
      }
      return response.reply;
    });
  }

  /** Convenience over entityRequest: fills addressing fields for one call. */
  callMethod(
    target: Pick<EntityEnvelope, "scope" | "typeId" | "entityId" | "incarnation" | "hostingGeneration"> & {
      side?: EntityEnvelope["side"];
    },
    methodId: number,
    payload: Uint8Array = Buffer.alloc(0),
    deadlineMs = 0,
    timeoutMs?: number,
  ): Promise<EntityReply> {
    return this.entityRequest(
      {
        ...target,
        side: target.side ?? 1,
        methodId,
        correlationId: this.nextCorrelationId++,
        deadlineMs,
        payload,
      },
      timeoutMs,
    );
  }

  private onData(chunk: Buffer): void {
    let frames: ReturnType<FrameAccumulator["push"]>;
    try {
      frames = this.frames.push(chunk);
    } catch (error) {
      this.onLinkFailure(error as Error);
      return;
    }
    for (const frame of frames) this.onFrame(frame.payload);
  }

  private onFrame(payload: Buffer): void {
    // Handshake replies stay uncorrelated even on an MLC2 connection ("queued
    // unprefixed while correlation is still off", src/session_plane.hpp:528).
    if (this.handshakeCursor < this.handshakeSteps.length) {
      const step = this.handshakeSteps[this.handshakeCursor]!;
      const answer = payload.toString("utf8");
      if (answer !== step.expectedReply) {
        this.handshakeSteps = [];
        this.handshakeCursor = 0;
        step.waiter.reject(new TransportError(answer));
        return;
      }
      this.handshakeCursor += 1;
      step.waiter.resolve();
      return;
    }

    if (this.correlated && this.handshakeDone) {
      const decoded = decodeCorrelatedFrame(payload);
      if (!decoded) return this.failPending(new TransportError("short correlated frame"));
      const pending = this.pending.get(decoded.requestId);
      if (pending) {
        this.pending.delete(decoded.requestId);
        clearTimeout(pending.timer);
        pending.resolve(decoded.body);
        return;
      }
      if (decoded.requestId === 0) {
        this.emit("push", decoded.body);
        return;
      }
      return; // stale reply for an already-timed-out request
    }

    const pending = this.uncorrelated;
    if (pending) {
      this.uncorrelated = null;
      clearTimeout(pending.timer);
      pending.resolve(payload);
      return;
    }
    this.emit("push", payload);
  }

  private failPending(error: Error): void {
    for (const [id, pending] of this.pending) {
      clearTimeout(pending.timer);
      pending.reject(error);
      this.pending.delete(id);
    }
    if (this.uncorrelated) {
      clearTimeout(this.uncorrelated.timer);
      const pending = this.uncorrelated;
      this.uncorrelated = null;
      pending.reject(error);
    }
  }

  private onLinkFailure(error: Error): void {
    if (this.closing) return;
    this.teardownSocket();
    this.failPending(error);
    this.rejectHandshakeSteps(error);
    if (this.handshakeWait) {
      const waiter = this.handshakeWait;
      this.handshakeWait = null;
      waiter.reject(error);
    }
    if (!this.autoReconnect || this.closing) {
      this.setState("closed");
      // Node throws on an 'error' emission without listeners; connect() and
      // the state/backoff events already carry the failure, so only forward
      // it when someone subscribed.
      if (this.listenerCount("error") > 0) this.emit("error", error);
      return;
    }
    const delay = this.backoff.next();
    this.setState("backoff");
    this.emit("backoff", delay, error);
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      if (this.closing) return;
      this.setState("connecting");
      this.openSocket();
    }, delay);
  }

  private teardownSocket(): void {
    this.handshakeDone = false;
    if (this.socket) {
      const socket = this.socket;
      this.socket = null;
      socket.removeAllListeners();
      socket.destroy();
    }
  }

  /** Rejects only the handshake steps still in flight, then clears state. */
  private rejectHandshakeSteps(error: Error): void {
    for (let i = this.handshakeCursor; i < this.handshakeSteps.length; i += 1) {
      this.handshakeSteps[i]!.waiter.reject(error);
    }
    this.handshakeSteps = [];
    this.handshakeCursor = 0;
  }

  /** Closes the transport and stops any scheduled reconnect. */
  close(): void {
    this.closing = true;
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    this.teardownSocket();
    this.failPending(new TransportError("transport closed"));
    this.rejectHandshakeSteps(new TransportError("transport closed"));
    if (this.handshakeWait) {
      const waiter = this.handshakeWait;
      this.handshakeWait = null;
      waiter.reject(new TransportError("transport closed"));
    }
    this.setState("closed");
  }
}
