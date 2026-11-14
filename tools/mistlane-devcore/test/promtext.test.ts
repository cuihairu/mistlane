import { describe, expect, it } from "vitest";
import { labelValue, parsePrometheusText, pickSample } from "../src/promtext.js";

// Fixture text uses exactly the families the node exposes
// (include/mistlane/dashboard.hpp render()).
const FIXTURE = [
  "# HELP mistlane_active_sessions Currently admitted game sessions.",
  "# TYPE mistlane_active_sessions gauge",
  'mistlane_active_sessions 4',
  'mistlane_max_connections 1024',
  'mistlane_script_gauge{name="rooms"} 2',
  'mistlane_messages_total{direction="tx"} 40',
  'mistlane_messages_total{direction="rx"} 40',
  'mistlane_request_seconds_sum 0.0125',
  'mistlane_request_seconds_count 16',
  'mistlane_event_loop_lag_max_seconds 0.0042',
  'mistlane_process_resident_memory_bytes 5.24288e+07',
  'mistlane_entity_results_total{result="ok"} 32',
  'mistlane_entity_results_total{result="unrouted"} 1',
  'mistlane_ready{role="gameServer"} 1',
  "not-a-number x",
  "",
].join("\n");

describe("Prometheus text parser (text 0.0.4)", () => {
  const parsed = parsePrometheusText(FIXTURE);

  it("drops comments, blank lines and non-numeric samples", () => {
    expect(parsed.has("not-a-number")).toBe(false);
    expect(parsed.size).toBe(10);
  });

  it("splits one family into labeled samples", () => {
    const messages = parsed.get("mistlane_messages_total")!;
    expect(messages).toHaveLength(2);
    expect(pickSample(parsed, "mistlane_messages_total", 'direction="tx"')).toBe(40);
    expect(pickSample(parsed, "mistlane_messages_total", 'direction="rx"')).toBe(40);
  });

  it("reads unlabeled families directly", () => {
    expect(pickSample(parsed, "mistlane_active_sessions")).toBe(4);
    expect(pickSample(parsed, "mistlane_request_seconds_sum")).toBeCloseTo(0.0125);
  });

  it("parses scientific notation and label values", () => {
    expect(pickSample(parsed, "mistlane_process_resident_memory_bytes")).toBeCloseTo(52_428_800);
    const ready = parsed.get("mistlane_ready")![0]!;
    expect(labelValue(ready.labels, "role")).toBe("gameServer");
    expect(labelValue(ready.labels, "missing")).toBeNull();
  });

  it("keeps the full sample key for identity", () => {
    expect(parsed.get("mistlane_script_gauge")![0]!.key).toBe('mistlane_script_gauge{name="rooms"}');
  });

  it("returns null when a family or label is absent", () => {
    expect(pickSample(parsed, "mistlane_absent")).toBeNull();
    expect(pickSample(parsed, "mistlane_messages_total", 'direction="zzz"')).toBeNull();
  });
});
