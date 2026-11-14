import { describe, expect, it } from "vitest";
import {
  MAX_LOG_ENTRIES,
  MAX_SERIES_POINTS,
  PanelModel,
  firstSample,
  formatBytes,
  matchesFilter,
  snapshot,
  sumFamily,
} from "../src/panelModel";
import type { MetricsSampleEvent } from "@mistlane/devcore/metrics";

function metricsEvent(families: Record<string, { value: number; labels?: string }[]>): MetricsSampleEvent {
  const metrics = new Map();
  for (const [name, rows] of Object.entries(families)) {
    metrics.set(name, rows.map((row) => ({ key: `${name}${row.labels ?? ""}`, labels: row.labels ?? "", value: row.value })));
  }
  return { at: 1000, metrics, raw: "" };
}

describe("matchesFilter", () => {
  it("matches everything on empty input", () => {
    expect(matchesFilter("ready gameServer", "")).toBe(true);
    expect(matchesFilter("ready gameServer", "   ")).toBe(true);
  });

  it("matches case-insensitive substrings", () => {
    expect(matchesFilter("session admission limit 42", "ADMISSION")).toBe(true);
    expect(matchesFilter("session admission limit 42", "nomatch")).toBe(false);
  });

  it("treats /slash-wrapped/ text as a regex", () => {
    expect(matchesFilter("warn load sample 12", "/warn \\w+/")).toBe(true);
    expect(matchesFilter("ready gameServer", "/^ready/")).toBe(true);
    expect(matchesFilter("ready gameServer", "/^goodbye/")).toBe(false);
  });

  it("supports flags after the closing slash and never throws on bad regex", () => {
    expect(matchesFilter("READY line", "/ready/i")).toBe(true);
    expect(matchesFilter("anything", "/([bad")).toBe(false);
  });
});

describe("firstSample and sumFamily", () => {
  it("reads the first sample of a family", () => {
    const event = metricsEvent({ mistlane_active_sessions: [{ value: 3 }, { value: 9 }] });
    expect(firstSample(event.metrics, "mistlane_active_sessions")).toBe(3);
    expect(firstSample(event.metrics, "mistlane_missing")).toBeNull();
  });

  it("sums every sample in a counter family", () => {
    const event = metricsEvent({
      mistlane_messages_total: [{ value: 4, labels: 'direction="in"' }, { value: 5, labels: 'direction="out"' }],
    });
    expect(sumFamily(event.metrics, "mistlane_messages_total")).toBe(9);
    expect(sumFamily(event.metrics, "mistlane_missing")).toBeNull();
  });
});

describe("PanelModel", () => {
  it("buffers proc curves and caps series length", () => {
    const model = new PanelModel();
    for (let i = 0; i < MAX_SERIES_POINTS + 30; i += 1) {
      model.addProc({ at: i, pid: 1, rssBytes: 1024 * i, vmSizeBytes: null, threads: null, cpuFraction: 0.5, cpuPercent: 50 });
    }
    expect(model.cpu).toHaveLength(MAX_SERIES_POINTS);
    expect(model.rss).toHaveLength(MAX_SERIES_POINTS);
    expect(model.cpu[0]!.at).toBe(30); // oldest points dropped, newest kept
    expect(model.rss[model.rss.length - 1]!.value).toBe(1024 * (MAX_SERIES_POINTS + 29));
  });

  it("keeps the log buffer bounded and drops oldest lines", () => {
    const model = new PanelModel();
    const lines = Array.from({ length: MAX_LOG_ENTRIES + 10 }, (_, i) => `line ${i}`);
    model.addLog({ at: 5, lines });
    expect(model.logs).toHaveLength(MAX_LOG_ENTRIES);
    expect(model.logs[0]!.text).toBe("line 10");
    expect(model.logs[model.logs.length - 1]!.text).toBe(`line ${MAX_LOG_ENTRIES + 9}`);
  });

  it("filters visible logs and bounds the render window", () => {
    const model = new PanelModel();
    model.addLog({ at: 1, lines: ["ready gameServer", "warn load sample", "script reloaded"] });
    model.setFilter("warn");
    expect(model.visibleLogs().map((entry) => entry.text)).toEqual(["warn load sample"]);
    model.setFilter("/^(ready|script)/");
    expect(model.visibleLogs().map((entry) => entry.text)).toEqual(["ready gameServer", "script reloaded"]);
    model.setFilter("");
    expect(model.visibleLogs()).toHaveLength(3);
  });

  it("extracts session gauge and message counter into series", () => {
    const model = new PanelModel();
    model.addMetrics(metricsEvent({
      mistlane_active_sessions: [{ value: 2 }],
      mistlane_messages_total: [{ value: 3, labels: 'direction="in"' }, { value: 4, labels: 'direction="out"' }],
    }));
    expect(model.sessions).toEqual([{ at: 1000, value: 2 }]);
    expect(model.traffic).toEqual([{ at: 1000, value: 7 }]);
  });

  it("carries contract rows and status through the snapshot", () => {
    const model = new PanelModel();
    model.setContract(
      [{ entity: "Player", entityId: 2, side: "server", method: "login", methodId: 1, mode: "request", caller: "server", args: "token:STRING", returns: "ok:UINT16", line: 13 }],
      "examples/contracts/business.def",
    );
    model.addProc({ at: 2, pid: 7, rssBytes: 2048, vmSizeBytes: null, threads: 4, cpuFraction: null, cpuPercent: null });
    const snap = snapshot(model);
    expect(snap.contract).toHaveLength(1);
    expect(snap.contract[0]!.method).toBe("login");
    expect(snap.contractPath).toBe("examples/contracts/business.def");
    expect(snap.totalLogs).toBe(0);
    expect(snap.cpu).toHaveLength(1);
    expect(snap.cpu[0]!.rssBytes).toBe(2048);
  });

  it("reset clears curves, logs and status", () => {
    const model = new PanelModel();
    model.addLog({ at: 1, lines: ["a"] });
    model.addProc({ at: 1, pid: 1, rssBytes: 1, vmSizeBytes: null, threads: null, cpuFraction: 1, cpuPercent: 100 });
    model.reset();
    const snap = snapshot(model);
    expect(snap.cpu).toHaveLength(0);
    expect(snap.totalLogs).toBe(0);
    expect(snap.status).toEqual({ metrics: "off", log: "off", proc: "off" });
  });
});

describe("formatBytes", () => {
  it("renders byte sizes readably", () => {
    expect(formatBytes(512)).toBe("512 B");
    expect(formatBytes(2048)).toBe("2.0 KiB");
    expect(formatBytes(17 * 1024 * 1024)).toBe("17.0 MiB");
  });
});
