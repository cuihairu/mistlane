// Log source, phase one: a file-tail poller. The engine ships no log-file
// sink of its own — node output lands on stdout/stderr (src/main.cpp,
// src/node.cpp), and the scaffolded ops scripts turn that into files:
//
//   "$root/ops/run-instance.sh" "$name" >>"$root/var/log/$name.log" 2>&1
//   (src/scaffold.cpp:347; the script's own `logs` action is tail -n 100 -f)
//
// so a plain-text append-only file at a fixed path IS the established log
// surface. This poller reads it the way the ops script's tail does: fixed
// interval, incremental offsets, partial-line buffering, and tail -F style
// recovery across truncation and rotation. A push socket (devlog tee inside
// the engine) is the phase-two upgrade and needs no client format change.

import { EventEmitter } from "node:events";
import * as fs from "node:fs";
import { StringDecoder } from "node:string_decoder";

export interface FileTailOptions {
  path: string;
  intervalMs?: number;
  /** Start from EOF instead of replaying history. Default true. */
  fromEnd?: boolean;
  encoding?: BufferEncoding;
  /** Injectable stat for tests. */
  statFn?: (path: fs.PathLike) => Promise<fs.Stats>;
}

export interface LogLinesEvent {
  at: number;
  lines: string[];
}

export type TailResetReason = "truncated" | "rotated" | "appeared";

export class FileTailPoller extends EventEmitter {
  private readonly path: string;
  private readonly intervalMs: number;
  private readonly fromEnd: boolean;
  private readonly encoding: BufferEncoding;
  private readonly statFn: (path: fs.PathLike) => Promise<fs.Stats>;
  private timer: NodeJS.Timeout | null = null;
  private handle: fs.promises.FileHandle | null = null;
  private offset = 0;
  private signature: { dev: number; ino: number } | null = null;
  private pendingText = "";
  private readonly decoder = new StringDecoder("utf8");
  private reading = false;
  private running = false;
  private openedOnce = false;
  /** True when the path existed on the very first poll; null before it. */
  private existedAtStart: boolean | null = null;

  constructor(options: FileTailOptions) {
    super();
    this.path = options.path;
    this.intervalMs = options.intervalMs ?? 500;
    this.fromEnd = options.fromEnd !== false;
    this.encoding = options.encoding ?? "utf8";
    this.statFn = options.statFn ?? ((path) => fs.promises.stat(path));
  }

  get isRunning(): boolean {
    return this.running;
  }

  start(): void {
    if (this.running) return;
    this.running = true;
    const tick = (): void => {
      if (!this.running) return;
      void this.poll().finally(() => {
        if (this.running) this.timer = setTimeout(tick, this.intervalMs);
      });
    };
    void tick();
  }

  stop(): void {
    this.running = false;
    if (this.timer) {
      clearTimeout(this.timer);
      this.timer = null;
    }
    void this.handle?.close().catch(() => {});
    this.handle = null;
    this.signature = null;
    this.offset = 0;
    this.pendingText = "";
    this.openedOnce = false;
    this.existedAtStart = null;
    this.decoder.end();
  }

  private async poll(): Promise<void> {
    if (this.reading) return;
    this.reading = true;
    try {
      let stat: fs.Stats;
      try {
        stat = await this.statFn(this.path);
      } catch {
        // File absent (yet): keep waiting, and re-arm from scratch when the
        // path reappears — a fresh deployment must not replay stale offsets.
        if (this.handle) {
          await this.handle.close().catch(() => {});
          this.handle = null;
          this.signature = null;
        }
        this.offset = 0;
        if (this.existedAtStart === null) this.existedAtStart = false;
        this.emit("waiting", { path: this.path });
        return;
      }
      if (this.existedAtStart === null) this.existedAtStart = true;

      const signature = { dev: stat.dev, ino: stat.ino };
      if (this.handle && this.signature &&
          (signature.ino !== this.signature.ino || signature.dev !== this.signature.dev)) {
        await this.drain(); // rotation: flush the tail of the old file first
        await this.handle.close().catch(() => {});
        this.handle = null;
        this.signature = null;
        this.offset = 0;
        this.emit("reset", { reason: "rotated" as TailResetReason, path: this.path });
      }

      if (!this.handle) {
        this.handle = await fs.promises.open(this.path, "r");
        this.signature = signature;
        const size = (await this.handle.stat()).size;
        // fromEnd means "do not replay history that predates the attach", so
        // it applies only when the file already existed when polling began. A
        // file that appears later — rotation, or a deployment starting after
        // the attach — is fresh content and is read from the start, exactly
        // like `tail -F` reopening after a rename.
        const seekToEnd = this.fromEnd && !this.openedOnce && this.existedAtStart === true;
        this.offset = seekToEnd ? size : 0;
        if (!seekToEnd && size > 0) this.emit("reset", { reason: "appeared" as TailResetReason, path: this.path });
        this.openedOnce = true;
        // Fired once the poller actually holds the file: hosts flip their
        // "log" face to up here, and tests can order "attach, then traffic".
        this.emit("open", { path: this.path, offset: this.offset });
      } else if (stat.size < this.offset) {
        this.offset = 0; // truncation (logrotate copytruncate, manual :>, restart append)
        this.emit("reset", { reason: "truncated" as TailResetReason, path: this.path });
      }

      await this.drain();
    } catch (error) {
      this.emit("error", error instanceof Error ? error : new Error(String(error)));
    } finally {
      this.reading = false;
    }
  }

  private async drain(): Promise<void> {
    const handle = this.handle;
    if (!handle) return;
    const size = (await handle.stat()).size;
    if (size <= this.offset) return;
    const length = size - this.offset;
    const buffer = Buffer.alloc(length);
    const { bytesRead } = await handle.read(buffer, 0, length, this.offset);
    this.offset += bytesRead;
    if (bytesRead === 0) return;

    this.pendingText += this.decoder.write(buffer.subarray(0, bytesRead));
    const text = this.pendingText;
    if (!text.includes("\n")) return; // hold a partial line until it terminates
    const parts = text.split("\n");
    this.pendingText = parts.pop() ?? "";
    const lines = parts.map((line) => (line.endsWith("\r") ? line.slice(0, -1) : line));
    this.emit("lines", { at: Date.now(), lines } satisfies LogLinesEvent);
  }
}
