// The dashboard webview document: CSP with a per-render nonce, inline style
// and script (no external resources), VS Code theme variables. The script
// keeps its own copy of the snapshot and redraws on every message; filtering
// happens inside the webview over the snapshot's visibleLogs window.

import type { PanelSnapshot } from "./panelModel";

export function randomNonce(): string {
  const bytes = new Uint8Array(16);
  crypto.getRandomValues(bytes);
  return Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("");
}

export function buildPanelHtml(initial: PanelSnapshot, nonce: string): string {
  // `</script>` inside the payload would terminate the bootstrap tag early.
  const payload = JSON.stringify(initial).replace(/</g, "\\u003c");
  return `<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; script-src 'nonce-${nonce}'; style-src 'nonce-${nonce}';">
<style nonce="${nonce}">
  body { font-family: var(--vscode-font-family); color: var(--vscode-foreground);
         background: var(--vscode-editor-background); margin: 12px; }
  h2 { font-size: 1.05em; margin: 14px 0 6px; }
  .row { display: flex; gap: 14px; flex-wrap: wrap; }
  .chart { flex: 1 1 220px; min-width: 220px; }
  .chart canvas { width: 100%; height: 110px; background: var(--vscode-editorWidget-background);
                  border: 1px solid var(--vscode-widget-border, #8882); border-radius: 4px; }
  .chart .label { font-size: 0.85em; opacity: 0.85; margin-bottom: 2px; }
  .status { font-size: 0.9em; margin-bottom: 8px; }
  .status .src { margin-right: 12px; }
  .on { color: var(--vscode-testing-iconPassed, #4ec99a); }
  .off { color: var(--vscode-testing-iconFailed, #f14c4c); }
  .waiting { color: var(--vscode-editorWarning-foreground, #cca700); }
  #filter { width: 40%; min-width: 200px; padding: 4px 6px; margin-bottom: 6px;
            background: var(--vscode-input-background); color: var(--vscode-input-foreground);
            border: 1px solid var(--vscode-input-border, transparent); }
  #log { height: 200px; overflow-y: auto; font-family: var(--vscode-editor-font-family, monospace);
         font-size: 0.85em; background: var(--vscode-editorWidget-background);
         border: 1px solid var(--vscode-widget-border, #8882); border-radius: 4px;
         padding: 4px 6px; white-space: pre-wrap; word-break: break-all; }
  #log .meta { opacity: 0.6; margin-right: 8px; }
  table { border-collapse: collapse; width: 100%; font-size: 0.88em; }
  th, td { text-align: left; padding: 3px 10px 3px 0; border-bottom: 1px solid var(--vscode-widget-border, #8884); }
  tbody tr { cursor: pointer; }
  tbody tr:hover { background: var(--vscode-list-hoverBackground); }
  .hint { font-size: 0.8em; opacity: 0.7; }
</style>
</head>
<body>
  <div class="status" id="status"></div>
  <div class="row">
    <div class="chart"><div class="label">CPU %</div><canvas id="cpuCanvas"></canvas></div>
    <div class="chart"><div class="label">RSS</div><canvas id="rssCanvas"></canvas></div>
    <div class="chart"><div class="label">mistlane_messages_total</div><canvas id="trafficCanvas"></canvas></div>
    <div class="chart"><div class="label">mistlane_active_sessions</div><canvas id="sessionsCanvas"></canvas></div>
  </div>
  <h2>Live log <span class="hint" id="logCount"></span></h2>
  <input id="filter" placeholder="filter: text, /regex/, /regex/i" spellcheck="false">
  <div id="log"></div>
  <h2>Entity contract <span class="hint" id="contractPath"></span></h2>
  <table>
    <thead><tr><th>entity</th><th>side</th><th>method</th><th>id</th><th>mode</th><th>caller</th><th>args</th><th>returns</th></tr></thead>
    <tbody id="contractBody"></tbody>
  </table>
  <p class="hint">Click a contract row to open the .def at that line.</p>
  <script nonce="${nonce}">
    "use strict";
    const vscode = acquireVsCodeApi();
    let snapshot = ${payload};

    function renderStatus() {
      const el = document.getElementById("status");
      el.innerHTML = "";
      for (const name of ["metrics", "log", "proc"]) {
        const state = snapshot.status[name] || "off";
        const cls = state === "up" ? "on" : state === "waiting" ? "waiting" : "off";
        const span = document.createElement("span");
        span.className = "src " + cls;
        span.textContent = name + ": " + state;
        el.appendChild(span);
      }
    }

    function draw(canvasId, points, format) {
      const canvas = document.getElementById(canvasId);
      const dpr = window.devicePixelRatio || 1;
      const width = canvas.clientWidth, height = canvas.clientHeight;
      canvas.width = width * dpr; canvas.height = height * dpr;
      const ctx = canvas.getContext("2d");
      ctx.scale(dpr, dpr);
      ctx.clearRect(0, 0, width, height);
      const values = points.map((p) => p.value).filter((v) => Number.isFinite(v));
      if (values.length === 0) return;
      let min = Math.min(...values), max = Math.max(...values);
      if (min === max) { min -= 1; max += 1; }
      const pad = 4;
      const x = (i) => pad + (i * (width - 2 * pad)) / Math.max(points.length - 1, 1);
      const y = (v) => height - pad - ((v - min) * (height - 2 * pad)) / (max - min);
      ctx.strokeStyle = "#4ec99a"; ctx.lineWidth = 1.5;
      ctx.beginPath();
      points.forEach((p, i) => { if (Number.isFinite(p.value)) { i === 0 ? ctx.moveTo(x(i), y(p.value)) : ctx.lineTo(x(i), y(p.value)); } });
      ctx.stroke();
      ctx.fillStyle = "var(--vscode-foreground, #ccc)";
      ctx.font = "10px sans-serif";
      ctx.fillText(format(max), pad, 10);
      ctx.fillText(format(min), pad, height - 3);
    }

    function renderCharts() {
      draw("cpuCanvas", snapshot.cpu.map((p) => ({ value: p.cpuPercent })).filter((p) => p.value !== null),
           (v) => v.toFixed(1) + "%");
      draw("rssCanvas", snapshot.rss, (v) => (v / 1048576).toFixed(1) + " MiB");
      draw("trafficCanvas", snapshot.traffic, (v) => v.toFixed(0));
      draw("sessionsCanvas", snapshot.sessions, (v) => v.toFixed(0));
    }

    function renderLog() {
      const el = document.getElementById("log");
      el.textContent = "";
      for (const entry of snapshot.visibleLogs) {
        const line = document.createElement("div");
        const time = new Date(entry.at);
        const hh = String(time.getHours()).padStart(2, "0");
        const mm = String(time.getMinutes()).padStart(2, "0");
        const ss = String(time.getSeconds()).padStart(2, "0");
        const meta = document.createElement("span");
        meta.className = "meta";
        meta.textContent = hh + ":" + mm + ":" + ss;
        line.appendChild(meta);
        line.appendChild(document.createTextNode(entry.text));
        el.appendChild(line);
      }
      el.scrollTop = el.scrollHeight;
      document.getElementById("logCount").textContent =
        snapshot.visibleLogs.length + " of " + snapshot.totalLogs + " lines";
    }

    function renderContract() {
      document.getElementById("contractPath").textContent = snapshot.contractPath || "";
      const body = document.getElementById("contractBody");
      body.textContent = "";
      for (const row of snapshot.contract) {
        const tr = document.createElement("tr");
        tr.setAttribute("data-line", String(row.line));
        for (const key of ["entity", "side", "method", "methodId", "mode", "caller", "args", "returns"]) {
          const td = document.createElement("td");
          td.textContent = String(row[key]);
          tr.appendChild(td);
        }
        body.appendChild(tr);
      }
    }

    document.getElementById("filter").addEventListener("input", (event) => {
      vscode.postMessage({ type: "setFilter", value: event.target.value });
    });
    document.getElementById("contractBody").addEventListener("click", (event) => {
      const tr = event.target.closest("tr");
      if (tr && tr.getAttribute("data-line")) {
        vscode.postMessage({ type: "openContract", line: Number(tr.getAttribute("data-line")) });
      }
    });

    function renderAll() { renderStatus(); renderCharts(); renderLog(); renderContract(); }
    window.addEventListener("message", (event) => {
      snapshot = event.data;
      if (event.data && typeof event.data.filter === "string") {
        const input = document.getElementById("filter");
        if (input.value !== event.data.filter && document.activeElement !== input) input.value = event.data.filter;
      }
      renderAll();
    });
    renderAll();
  </script>
</body>
</html>`;
}
