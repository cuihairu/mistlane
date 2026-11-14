// Process-level CPU/RSS sampling from /proc/<pid>/{status,stat}, the same
// surfaces the kernel exposes for any process. The node already exposes its
// own RSS gauge (mistlane_process_resident_memory_bytes, refreshed by
// Diagnostics::refresh_process_memory) but not its CPU time, and an IDE
// attaching to a node started without listeners needs the process face
// anyway. Sampling is read-only and works on any node build.

import { EventEmitter } from "node:events";
import * as fs from "node:fs";

export interface ProcStatus {
  rssBytes: number | null;
  vmSizeBytes: number | null;
  threads: number | null;
}

export interface ProcStat {
  /** utime + stime in clock ticks since process start. */
  cpuTicks: number;
  /** Ticks after the process started, for uptime-aware rates. */
  starttimeTicks: number;
}

export interface ProcSample {
  at: number;
  pid: number;
  rssBytes: number | null;
  vmSizeBytes: number | null;
  threads: number | null;
  /** Fraction of one core used since the previous sample (can exceed 1). */
  cpuFraction: number | null;
  cpuPercent: number | null;
}

export interface ProcSamplerOptions {
  pid: number;
  intervalMs?: number;
  /** Kernel clock ticks per second (USER_HZ). Default 100. */
  clockTicks?: number;
  /** Injectable reader for tests. Defaults to real /proc reads. */
  readFile?: (path: string, encoding: BufferEncoding) => Promise<string>;
  now?: () => number;
}

export const CLOCK_TICKS_DEFAULT = 100;

/**
 * Parses /proc/<pid>/status fields of interest. Values are "N kB" strings;
 * threads is a bare integer.
 */
export function parseProcStatus(text: string): ProcStatus {
  const kB = (name: string): number | null => {
    const match = new RegExp(`^${name}:\\s+(\\d+) kB$`, "m").exec(text);
    return match ? Number(match[1]) * 1024 : null;
  };
  const threads = /^Threads:\s+(\d+)$/m.exec(text);
  return {
    rssBytes: kB("VmRSS"),
    vmSizeBytes: kB("VmSize"),
    threads: threads ? Number(threads[1]) : null,
  };
}

/**
 * Parses /proc/<pid>/stat. Field 2 (comm) may contain spaces and parentheses,
 * so parsing starts after the last ')'; fields after it are 3-based in procfs
 * numbering (utime is field 14, stime 15, starttime 22).
 */
export function parseProcStat(text: string): ProcStat | null {
  const close = text.lastIndexOf(")");
  if (close < 0) return null;
  const fields = text.slice(close + 2).split(" ");
  // fields[i] holds procfs field i+3.
  const utime = fields[11];
  const stime = fields[12];
  const starttime = fields[19];
  if (!utime || !stime || !starttime) return null;
  return { cpuTicks: Number(utime) + Number(stime), starttimeTicks: Number(starttime) };
}

/** PID from a pidfile (the scaffold writes one plain integer, src/scaffold.cpp:345). */
export async function readPidFile(path: string): Promise<number> {
  const text = await fs.promises.readFile(path, "utf8");
  const pid = Number.parseInt(text.trim(), 10);
  if (!Number.isInteger(pid) || pid <= 0) throw new Error(`pidfile ${path} holds no usable pid`);
  return pid;
}

export class ProcSampler extends EventEmitter {
  private readonly pid: number;
  private readonly intervalMs: number;
  private readonly clockTicks: number;
  private readonly readFile: (path: string, encoding: BufferEncoding) => Promise<string>;
  private readonly now: () => number;
  private timer: NodeJS.Timeout | null = null;
  private running = false;
  private previous: { cpuTicks: number; at: number } | null = null;
  private reading = false;

  constructor(options: ProcSamplerOptions) {
    super();
    if (!Number.isInteger(options.pid) || options.pid <= 0) throw new Error(`invalid pid: ${options.pid}`);
    this.pid = options.pid;
    this.intervalMs = options.intervalMs ?? 1000;
    this.clockTicks = options.clockTicks ?? CLOCK_TICKS_DEFAULT;
    this.readFile = options.readFile ?? ((path, encoding) => fs.promises.readFile(path, encoding));
    this.now = options.now ?? Date.now;
  }

  get isRunning(): boolean {
    return this.running;
  }

  start(): void {
    if (this.running) return;
    this.running = true;
    const tick = (): void => {
      if (!this.running) return;
      void this.sampleOnce().finally(() => {
        if (this.running) this.timer = setTimeout(tick, this.intervalMs);
      });
    };
    void tick();
  }

  async sampleOnce(): Promise<ProcSample | null> {
    if (this.reading) return null;
    this.reading = true;
    try {
      const base = `/proc/${this.pid}`;
      const [statusText, statText] = await Promise.all([
        this.readFile(`${base}/status`, "utf8"),
        this.readFile(`${base}/stat`, "utf8"),
      ]);
      const status = parseProcStatus(statusText);
      const stat = parseProcStat(statText);
      const at = this.now();
      let cpuFraction: number | null = null;
      if (stat) {
        if (this.previous) {
          const ticks = stat.cpuTicks - this.previous.cpuTicks;
          const wallMs = Math.max(at - this.previous.at, 1);
          cpuFraction = (ticks / this.clockTicks) / (wallMs / 1000);
        }
        this.previous = { cpuTicks: stat.cpuTicks, at };
      }
      const sample: ProcSample = {
        at,
        pid: this.pid,
        rssBytes: status.rssBytes,
        vmSizeBytes: status.vmSizeBytes,
        threads: status.threads,
        cpuFraction,
        cpuPercent: cpuFraction === null ? null : cpuFraction * 100,
      };
      this.emit("sample", sample);
      return sample;
    } catch (error) {
      this.previous = null;
      this.emit("error", error instanceof Error ? error : new Error(String(error)));
      return null;
    } finally {
      this.reading = false;
    }
  }

  stop(): void {
    this.running = false;
    if (this.timer) {
      clearTimeout(this.timer);
      this.timer = null;
    }
  }
}
