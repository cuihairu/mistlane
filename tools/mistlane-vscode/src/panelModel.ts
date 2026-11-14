// Pure panel state, deliberately free of the vscode API so it is unit
// testable: the webview renders whatever snapshot this produces, and the
// filter/search rules live here, not in the DOM code.

import type { MetricsSampleEvent } from "@mistlane/devcore/metrics";
import type { ProcSample } from "@mistlane/devcore/proc";
import type { LogLinesEvent } from "@mistlane/devcore/logtail";
import type { ContractMethodRow } from "@mistlane/devcore/contract";

export interface SeriesPoint {
  at: number;
  value: number;
}

export interface ProcCurvePoint {
  at: number;
  cpuPercent: number | null;
  rssBytes: number | null;
}

export interface LogEntry {
  at: number;
  text: string;
}

/** Buffer caps: bounded memory for a long attach session. */
export const MAX_SERIES_POINTS = 240;
export const MAX_LOG_ENTRIES = 2000;
/** The webview never renders more than this many matched lines at once. */
export const RENDER_LOG_LIMIT = 500;

export type SourceName = "metrics" | "log" | "proc";

export class PanelModel {
  readonly cpu: ProcCurvePoint[] = [];
  readonly rss: SeriesPoint[] = [];
  readonly sessions: SeriesPoint[] = [];
  readonly traffic: SeriesPoint[] = [];
  readonly logs: LogEntry[] = [];
  readonly contract: ContractMethodRow[] = [];
  contractPath = "";
  filter = "";
  status: Record<SourceName, string> = { metrics: "off", log: "off", proc: "off" };

  reset(): void {
    this.cpu.length = 0;
    this.rss.length = 0;
    this.sessions.length = 0;
    this.traffic.length = 0;
    this.logs.length = 0;
    this.status = { metrics: "off", log: "off", proc: "off" };
  }

  addProc(sample: ProcSample): void {
    this.pushCapped(this.cpu, {
      at: sample.at,
      cpuPercent: sample.cpuPercent,
      rssBytes: sample.rssBytes,
    }, MAX_SERIES_POINTS);
    if (sample.rssBytes !== null) {
      this.pushCapped(this.rss, { at: sample.at, value: sample.rssBytes }, MAX_SERIES_POINTS);
    }
  }

  /** Extracts the faces the panel plots: session gauge and message counter. */
  addMetrics(event: MetricsSampleEvent): void {
    const sessions = firstSample(event.metrics, "mistlane_active_sessions");
    if (sessions !== null) this.pushCapped(this.sessions, { at: event.at, value: sessions }, MAX_SERIES_POINTS);
    const messages = sumFamily(event.metrics, "mistlane_messages_total");
    if (messages !== null) this.pushCapped(this.traffic, { at: event.at, value: messages }, MAX_SERIES_POINTS);
  }

  addLog(event: LogLinesEvent): void {
    for (const text of event.lines) this.pushCapped(this.logs, { at: event.at, text }, MAX_LOG_ENTRIES);
  }

  setContract(rows: ContractMethodRow[], path: string): void {
    this.contract.length = 0;
    for (const row of rows) this.contract.push(row);
    this.contractPath = path;
  }

  setFilter(text: string): void {
    this.filter = text;
  }

  /** Log lines matching the current filter, newest last, bounded for render. */
  visibleLogs(): LogEntry[] {
    const matched = this.logs.filter((entry) => matchesFilter(entry.text, this.filter));
    return matched.length > RENDER_LOG_LIMIT ? matched.slice(matched.length - RENDER_LOG_LIMIT) : matched;
  }

  private pushCapped<T>(array: T[], value: T, cap: number): void {
    array.push(value);
    if (array.length > cap) array.splice(0, array.length - cap);
  }
}

/** Value of the first sample of a metric family, or null when absent. */
export function firstSample(metrics: MetricsSampleEvent["metrics"], family: string): number | null {
  const rows = metrics.get(family);
  return rows && rows.length > 0 ? rows[0]!.value : null;
}

/** Sum of every sample in a family (e.g. both directions of a counter). */
export function sumFamily(metrics: MetricsSampleEvent["metrics"], family: string): number | null {
  const rows = metrics.get(family);
  if (!rows || rows.length === 0) return null;
  return rows.reduce((total, row) => total + row.value, 0);
}

/**
 * Filter rule: empty matches everything; `/pattern/` (with flags like
 * `/warn/i`) is a regular expression; anything else is a case-insensitive
 * substring. A broken regex never throws — it just matches nothing.
 */
export function matchesFilter(text: string, filter: string): boolean {
  const trimmed = filter.trim();
  if (trimmed === "") return true;
  if (trimmed.length >= 2 && trimmed.startsWith("/") && trimmed.endsWith("/")) {
    try {
      return new RegExp(trimmed.slice(1, -1)).test(text);
    } catch {
      return false;
    }
  }
  if (trimmed.length >= 3 && trimmed.startsWith("/") && trimmed.lastIndexOf("/") > 0) {
    const last = trimmed.lastIndexOf("/");
    try {
      return new RegExp(trimmed.slice(1, last), trimmed.slice(last + 1)).test(text);
    } catch {
      return false;
    }
  }
  return text.toLowerCase().includes(trimmed.toLowerCase());
}

export function formatBytes(bytes: number): string {
  const units = ["B", "KiB", "MiB", "GiB"];
  let value = bytes;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit += 1;
  }
  return `${unit === 0 ? value : value.toFixed(1)} ${units[unit]}`;
}

/** Snapshot the extension posts into the webview after each event batch. */
export interface PanelSnapshot {
  cpu: ProcCurvePoint[];
  rss: SeriesPoint[];
  sessions: SeriesPoint[];
  traffic: SeriesPoint[];
  visibleLogs: LogEntry[];
  totalLogs: number;
  filter: string;
  status: Record<SourceName, string>;
  contract: ContractMethodRow[];
  contractPath: string;
}

export function snapshot(model: PanelModel): PanelSnapshot {
  return {
    cpu: [...model.cpu],
    rss: [...model.rss],
    sessions: [...model.sessions],
    traffic: [...model.traffic],
    visibleLogs: model.visibleLogs(),
    totalLogs: model.logs.length,
    filter: model.filter,
    status: { ...model.status },
    contract: [...model.contract],
    contractPath: model.contractPath,
  };
}
