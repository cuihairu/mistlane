// Metrics source: polls the node's read-only metrics surface over plain HTTP
// and emits parsed samples. Verified surface (src/observability.hpp
// DashboardServer, wired for --dashboard-listen and mirrored by the
// --metrics-listen exposer):
//
//   GET /metrics -> 200, text/plain; version=0.0.4, Prometheus registry text
//                   (Diagnostics::metrics_text()), Connection: close
//   GET / or /dashboard -> the single-page dashboard HTML
//   anything else -> 404
//
// No handshake and no token: the dashboard listener is a read-only bounded
// surface (64 peers, 2 KiB request line, 5 s expiry), so a plain poll is the
// intended consumption path — same as the page itself fetches /metrics.

import { EventEmitter } from "node:events";
import { PrometheusText, parsePrometheusText } from "./promtext.js";

export interface MetricsPollerOptions {
  /** Origin only, e.g. http://127.0.0.1:9801 (no trailing slash). */
  baseUrl: string;
  path?: string;
  intervalMs?: number;
  requestTimeoutMs?: number;
  /** Injectable for tests; defaults to global fetch. */
  fetchImpl?: typeof fetch;
  now?: () => number;
}

export interface MetricsSampleEvent {
  at: number;
  metrics: PrometheusText;
  raw: string;
}

export class MetricsPoller extends EventEmitter {
  private readonly baseUrl: string;
  private readonly path: string;
  private readonly intervalMs: number;
  private readonly requestTimeoutMs: number;
  private readonly fetchImpl: typeof fetch;
  private readonly now: () => number;
  private timer: NodeJS.Timeout | null = null;
  private inFlight = false;
  private running = false;
  private lastOkAt: number | null = null;

  constructor(options: MetricsPollerOptions) {
    super();
    const base = options.baseUrl.replace(/\/+$/, "");
    if (!/^https?:\/\//.test(base)) throw new Error(`metrics baseUrl must be http(s): ${options.baseUrl}`);
    this.baseUrl = base;
    this.path = options.path ?? "/metrics";
    this.intervalMs = options.intervalMs ?? 1000;
    this.requestTimeoutMs = options.requestTimeoutMs ?? 3000;
    this.fetchImpl = options.fetchImpl ?? fetch;
    this.now = options.now ?? Date.now;
  }

  get isRunning(): boolean {
    return this.running;
  }

  /** Timestamp of the last successful poll, null before the first one. */
  get okAt(): number | null {
    return this.lastOkAt;
  }

  start(): void {
    if (this.running) return;
    this.running = true;
    const tick = async (): Promise<void> => {
      if (!this.running || this.inFlight) return;
      this.inFlight = true;
      try {
        const raw = await this.fetchOnce();
        this.lastOkAt = this.now();
        this.emit("sample", { at: this.lastOkAt, metrics: parsePrometheusText(raw), raw } satisfies MetricsSampleEvent);
      } catch (error) {
        this.emit("error", error instanceof Error ? error : new Error(String(error)));
      } finally {
        this.inFlight = false;
        if (this.running) this.timer = setTimeout(() => void tick(), this.intervalMs);
      }
    };
    void tick();
  }

  private async fetchOnce(): Promise<string> {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), this.requestTimeoutMs);
    try {
      const response = await this.fetchImpl(`${this.baseUrl}${this.path}`, {
        signal: controller.signal,
        headers: { "cache-control": "no-store" },
      });
      if (!response.ok) throw new Error(`metrics poll got HTTP ${response.status}`);
      return await response.text();
    } finally {
      clearTimeout(timer);
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

/** Metric families the engine actually exposes (include/mistlane/dashboard.hpp). */
export const MISTLANE_FAMILIES = {
  ready: "mistlane_ready",
  activeSessions: "mistlane_active_sessions",
  maxConnections: "mistlane_max_connections",
  scriptGauge: "mistlane_script_gauge",
  messagesTotal: "mistlane_messages_total",
  requestSecondsSum: "mistlane_request_seconds_sum",
  requestSecondsCount: "mistlane_request_seconds_count",
  eventLoopLagMaxSeconds: "mistlane_event_loop_lag_max_seconds",
  processResidentMemoryBytes: "mistlane_process_resident_memory_bytes",
  entityResultsTotal: "mistlane_entity_results_total",
} as const;
