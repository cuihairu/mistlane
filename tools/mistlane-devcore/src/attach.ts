// The attach session: one facade that owns the three data sources (metrics
// poller, log tail poller, process sampler) and re-emits their events under
// typed names. IDE shells (the VS Code extension, any future VS host) stay
// thin: attach, subscribe, detach. Nothing here mutates the node — every
// source is a read-only surface.

import { EventEmitter } from "node:events";
import { MetricsPoller, MetricsPollerOptions, MetricsSampleEvent } from "./metrics.js";
import { FileTailPoller, FileTailOptions, LogLinesEvent } from "./logtail.js";
import { ProcSample, ProcSampler, ProcSamplerOptions } from "./proc.js";

export interface AttachOptions {
  metrics?: MetricsPollerOptions;
  log?: FileTailOptions;
  proc?: ProcSamplerOptions;
}

export type AttachSourceState = "up" | "down" | "waiting" | "off";

export interface AttachStatus {
  metrics: AttachSourceState;
  log: AttachSourceState;
  proc: AttachSourceState;
}

export type AttachEvents = {
  metrics: (event: MetricsSampleEvent) => void;
  "metrics-error": (error: Error) => void;
  "metrics-state": (state: AttachSourceState) => void;
  log: (event: LogLinesEvent) => void;
  "log-error": (error: Error) => void;
  "log-state": (state: AttachSourceState) => void;
  "log-open": (event: { path: string; offset: number }) => void;
  "log-reset": (event: { reason: string; path: string }) => void;
  "log-waiting": (event: { path: string }) => void;
  proc: (sample: ProcSample) => void;
  "proc-error": (error: Error) => void;
  "proc-state": (state: AttachSourceState) => void;
};

/**
 * One attached node view. All sources are optional; pass only what the node
 * actually exposes. The engine needs no C++ changes for any of them:
 * --dashboard-listen (or --metrics-listen) serves /metrics over plain HTTP,
 * the ops scripts already land logs in var/log/<instance>.log, and /proc is
 * the kernel's own surface.
 */
export class AttachSession extends EventEmitter {
  private metricsPoller: MetricsPoller | null = null;
  private logPoller: FileTailPoller | null = null;
  private procSampler: ProcSampler | null = null;
  private statusValue: AttachStatus = { metrics: "off", log: "off", proc: "off" };

  constructor(private readonly options: AttachOptions) {
    super();
    if (options.metrics) {
      this.metricsPoller = new MetricsPoller(options.metrics);
      this.metricsPoller.on("sample", (event: MetricsSampleEvent) => {
        this.setStatus("metrics", "up");
        this.emit("metrics", event);
      });
      this.metricsPoller.on("error", (error: Error) => {
        this.setStatus("metrics", "down");
        this.emit("metrics-error", error);
      });
    }
    if (options.log) {
      this.logPoller = new FileTailPoller(options.log);
      this.logPoller.on("lines", (event: LogLinesEvent) => this.emit("log", event));
      this.logPoller.on("open", (event) => {
        this.setStatus("log", "up");
        this.emit("log-open", event);
      });
      this.logPoller.on("error", (error: Error) => this.emit("log-error", error));
      this.logPoller.on("reset", (event) => this.emit("log-reset", event));
      this.logPoller.on("waiting", (event) => {
        this.setStatus("log", "waiting");
        this.emit("log-waiting", event);
      });
    }
    if (options.proc) {
      this.procSampler = new ProcSampler(options.proc);
      this.procSampler.on("sample", (sample: ProcSample) => {
        this.setStatus("proc", "up");
        this.emit("proc", sample);
      });
      this.procSampler.on("error", (error: Error) => {
        this.setStatus("proc", "down");
        this.emit("proc-error", error);
      });
    }
  }

  get status(): AttachStatus {
    return { ...this.statusValue };
  }

  private setStatus(source: keyof AttachStatus, state: AttachSourceState): void {
    if (this.statusValue[source] === state) return;
    this.statusValue[source] = state;
    this.emit(`${source}-state`, state);
  }

  attach(): void {
    if (this.metricsPoller) {
      this.setStatus("metrics", "up");
      this.metricsPoller.start();
    }
    if (this.logPoller) {
      this.logPoller.start();
      this.setStatus("log", "waiting");
    }
    void this.procSampler?.start();
    this.setStatus("proc", this.procSampler ? "up" : "off");
  }

  detach(): void {
    this.metricsPoller?.stop();
    this.logPoller?.stop();
    this.procSampler?.stop();
    this.statusValue = { metrics: "off", log: "off", proc: "off" };
  }
}

// Typed emit helpers: EventEmitter's on() is stringly typed; narrow it here so
// IDE hosts get completion without a dependency on a typed-emitter library.
export interface AttachSession {
  on<Event extends keyof AttachEvents>(event: Event, listener: AttachEvents[Event]): this;
  emit<Event extends keyof AttachEvents>(event: Event, ...args: Parameters<AttachEvents[Event]>): boolean;
}
