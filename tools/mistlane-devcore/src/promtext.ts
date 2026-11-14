// Prometheus text exposition parser, version 0.0.4, matching the semantics of
// the parser embedded in the node's own dashboard page
// (include/mistlane/dashboard.hpp parse()): `name{labels} value` pairs, one per
// line; comments (HELP/TYPE/anything starting with '#') dropped; samples with
// a non-numeric value ignored. Duplicate names are normal (labels split one
// family into many samples), so every name maps to a list of samples.

export interface PrometheusSample {
  /** Sample key exactly as written: `name` or `name{label list}`. */
  key: string;
  /** Label text between the braces, without the braces themselves. */
  labels: string;
  value: number;
}

export type PrometheusText = Map<string, PrometheusSample[]>;

export function parsePrometheusText(text: string): PrometheusText {
  const out: PrometheusText = new Map();
  for (const line of text.split("\n")) {
    if (!line || line.startsWith("#")) continue;
    const brace = line.indexOf("{");
    if (brace > 0) {
      const close = line.indexOf("}", brace);
      if (close > brace) {
        const key = line.slice(0, close + 1);
        const name = line.slice(0, brace);
        const labels = line.slice(brace + 1, close);
        const value = Number.parseFloat(line.slice(close + 2));
        if (Number.isNaN(value)) continue;
        push(out, name, { key, labels, value });
        continue;
      }
    }
    const space = line.lastIndexOf(" ");
    if (space < 1) continue;
    const key = line.slice(0, space);
    const value = Number.parseFloat(line.slice(space + 1));
    if (Number.isNaN(value)) continue;
    push(out, key, { key, labels: "", value });
  }
  return out;
}

function push(map: PrometheusText, name: string, sample: PrometheusSample): void {
  const list = map.get(name);
  if (list) list.push(sample);
  else map.set(name, [sample]);
}

/** First sample of a family whose label text contains the given needle. */
export function pickSample(map: PrometheusText, name: string, labelNeedle?: string): number | null {
  const rows = map.get(name);
  if (!rows) return null;
  for (const row of rows) {
    const hit = !labelNeedle || row.labels.includes(labelNeedle);
    if (hit) return row.value;
  }
  return null;
}

/** Label value for a sample key, e.g. labelsFromKey('role') on `role="game"`. */
export function labelValue(labels: string, name: string): string | null {
  const match = new RegExp(`${name}="([^"]*)"`).exec(labels);
  return match ? match[1]! : null;
}
