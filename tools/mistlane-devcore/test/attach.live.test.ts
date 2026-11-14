// Live attach round: spawn a real node-like process (moving /metrics values,
// a stdout+stderr redirect log file, a real PID for /proc sampling), attach
// every devcore source at once, and verify the three IDE faces get live data —
// the "attach -> curves move -> logs flow" acceptance at the devtools level.
import { afterAll, beforeAll, describe, expect, it } from "vitest";
import { spawn } from "node:child_process";
import { openSync } from "node:fs";
import * as fs from "node:fs/promises";
import * as os from "node:os";
import * as path from "node:path";
import { AttachSession } from "../src/attach.js";
import { contractRows, parseContractDef } from "../src/contract.js";
import { readFileSync } from "node:fs";

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

async function waitFor(predicate: () => boolean | Promise<boolean>, timeoutMs = 8000): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    if (await predicate()) return;
    if (Date.now() >= deadline) throw new Error("condition not reached in time");
    await sleep(25);
  }
}

describe("AttachSession live round", () => {
  let dir: string;
  let logPath: string;
  let port: number;
  let child: ReturnType<typeof spawn>;

  beforeAll(async () => {
    dir = await fs.mkdtemp(path.join(os.tmpdir(), "mistlane-attach-"));
    logPath = path.join(dir, "gameServer.log");
    port = 19970 + Math.floor(Math.random() * 20);
    // Redirect the child's stdout and stderr into the log file exactly like
    // the scaffold ops scripts do (src/scaffold.cpp:347 `>>"$logfile" 2>&1`).
    const logFd = openSync(logPath, "a");
    child = spawn(
      process.execPath,
      [new URL("./fixtures/fake_node.mjs", import.meta.url).pathname, String(port)],
      { stdio: ["ignore", logFd, logFd] },
    );
    // Wait until the metrics surface actually answers, like ops readiness_wait.
    await waitFor(async () => {
      try {
        const response = await fetch(`http://127.0.0.1:${port}/metrics`);
        return response.ok;
      } catch {
        return false;
      }
    });
  });

  afterAll(async () => {
    child.kill("SIGKILL");
    await fs.rm(dir, { recursive: true, force: true });
  });

  it("streams logs, metrics and process samples from one attach", async () => {
    const session = new AttachSession({
      metrics: { baseUrl: `http://127.0.0.1:${port}`, intervalMs: 100 },
      log: { path: logPath, intervalMs: 50 },
      proc: { pid: child.pid!, intervalMs: 100 },
    });
    const logs: string[] = [];
    const sessionsSeen = new Set<number>();
    const rssSeen: number[] = [];
    const cpuSeen: number[] = [];
    const errors: string[] = [];
    session.on("log", (event) => logs.push(...event.lines));
    session.on("metrics", (event) => {
      const row = event.metrics.get("mistlane_active_sessions")?.[0];
      if (row) sessionsSeen.add(row.value);
    });
    session.on("proc", (sample) => {
      if (sample.rssBytes !== null) rssSeen.push(sample.rssBytes);
      if (sample.cpuFraction !== null) cpuSeen.push(sample.cpuFraction);
    });
    session.on("log-error", (error) => errors.push(`log: ${error.message}`));
    session.on("metrics-error", (error) => errors.push(`metrics: ${error.message}`));
    session.on("proc-error", (error) => errors.push(`proc: ${error.message}`));
    session.attach();

    await waitFor(() => logs.length >= 4, 8000);
    await waitFor(() => sessionsSeen.size >= 2, 8000);
    // The fixture burns CPU in short bursts, so an idle 100ms window can
    // legitimately sample 0; the curve is alive when some window catches ticks.
    await waitFor(() => cpuSeen.some((value) => value > 0), 8000);

    expect(errors).toEqual([]);
    expect(logs.filter((line) => line.startsWith("info session joined")).length).toBeGreaterThan(0);
    expect(logs.some((line) => /warn load sample/.test(line))).toBe(true);
    expect(sessionsSeen.size).toBeGreaterThanOrEqual(2); // the curve moved
    expect(rssSeen[0]).toBeGreaterThan(0);
    expect(cpuSeen.some((value) => value > 0)).toBe(true);
    expect(session.status).toMatchObject({ metrics: "up", log: "up", proc: "up" });
    session.detach();
    expect(session.status).toMatchObject({ metrics: "off", log: "off", proc: "off" });
  });

  it("renders the shipped contract example into panel rows", () => {
    const business = readFileSync(
      new URL("../../../examples/contracts/business.def", import.meta.url),
      "utf8",
    );
    const rows = contractRows(parseContractDef(business));
    expect(rows).toHaveLength(5);
    expect(rows.map((row) => row.method)).toEqual(["login", "joinRoom", "move", "heartbeat", "leaveRoom"]);
  });
});
