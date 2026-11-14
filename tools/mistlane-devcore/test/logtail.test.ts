import { afterAll, beforeAll, describe, expect, it } from "vitest";
import * as fssync from "node:fs";
import * as fs from "node:fs/promises";
import * as os from "node:os";
import * as path from "node:path";
import { FileTailPoller } from "../src/logtail.js";

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

async function waitFor(predicate: () => boolean, timeoutMs = 4000): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (predicate()) return;
    await sleep(25);
  }
  throw new Error("condition not reached in time");
}

describe("FileTailPoller", () => {
  let dir: string;
  let logPath: string;

  beforeAll(async () => {
    dir = await fs.mkdtemp(path.join(os.tmpdir(), "mistlane-logtail-"));
    logPath = path.join(dir, "node.log");
  });
  afterAll(async () => {
    await fs.rm(dir, { recursive: true, force: true });
  });

  it("emits appended lines and holds a partial final line", async () => {
    await fs.writeFile(logPath, "first line\nsecond line\n", { flag: "w" });
    const poller = new FileTailPoller({ path: logPath, intervalMs: 30, fromEnd: false });
    const batches: string[][] = [];
    poller.on("lines", (event) => batches.push(event.lines));
    poller.start();
    try {
      await waitFor(() => batches.flat().length >= 2);
      expect(batches.flat()).toEqual(["first line", "second line"]);

      await fs.appendFile(logPath, "third line\npartial with no newline");
      await waitFor(() => batches.flat().includes("third line"));
      // The unterminated tail must not be emitted yet.
      expect(batches.flat()).not.toContain("partial with no newline");

      await fs.appendFile(logPath, " end\n");
      await waitFor(() => batches.flat().includes("partial with no newline end"));
    } finally {
      poller.stop();
    }
  });

  it("starts at EOF by default (live attach, no replay)", async () => {
    await fs.writeFile(logPath, "history\n", { flag: "w" });
    const poller = new FileTailPoller({ path: logPath, intervalMs: 30 });
    const lines: string[] = [];
    poller.on("lines", (event) => lines.push(...event.lines));
    poller.start();
    await sleep(80); // let one poll see only the history
    await fs.appendFile(logPath, "live line\n");
    await waitFor(() => lines.length > 0);
    expect(lines).toEqual(["live line"]);
    poller.stop();
  });

  it("recovers from truncation without replaying garbage", async () => {
    await fs.writeFile(logPath, "old 1\nold 2\nold 3\n", { flag: "w" });
    const poller = new FileTailPoller({ path: logPath, intervalMs: 30, fromEnd: true });
    const resets: string[] = [];
    const lines: string[] = [];
    let opened = false;
    poller.on("open", () => {
      opened = true;
    });
    poller.on("reset", (event) => resets.push(event.reason));
    poller.on("lines", (event) => lines.push(...event.lines));
    poller.start();
    await waitFor(() => opened); // attach first, so later appends are live traffic
    await fs.appendFile(logPath, "more old\n");
    await waitFor(() => lines.includes("more old"));
    await fs.writeFile(logPath, "fresh after truncate\n", { flag: "w" }); // offset now beyond size
    await waitFor(() => resets.includes("truncated"));
    await waitFor(() => lines.includes("fresh after truncate"));
    expect(lines).not.toContain("old 1");
    poller.stop();
  });

  it("follows rotation into a new inode (tail -F)", async () => {
    await fs.writeFile(logPath, "before rotation\n", { flag: "w" });
    const poller = new FileTailPoller({ path: logPath, intervalMs: 30, fromEnd: true });
    const resets: string[] = [];
    const lines: string[] = [];
    let opened = false;
    poller.on("open", () => {
      opened = true;
    });
    poller.on("reset", (event) => resets.push(event.reason));
    poller.on("lines", (event) => lines.push(...event.lines));
    poller.start();
    await waitFor(() => opened);
    await fs.appendFile(logPath, "tail of old file\n");
    await waitFor(() => lines.includes("tail of old file"));
    // Synchronous rename+create: no ENOENT gap for a poll tick to land in.
    fssync.renameSync(logPath, path.join(dir, "node.log.1"));
    fssync.writeFileSync(logPath, "new file content\n", { flag: "w" });
    await waitFor(() => resets.includes("rotated"));
    await waitFor(() => lines.includes("new file content"));
    poller.stop();
  });

  it("waits for the log file to appear and then tails it", async () => {
    const missing = path.join(dir, "spawn-later.log");
    const poller = new FileTailPoller({ path: missing, intervalMs: 30 });
    const waiting: unknown[] = [];
    const lines: string[] = [];
    poller.on("waiting", (event) => waiting.push(event));
    poller.on("lines", (event) => lines.push(...event.lines));
    poller.start();
    await waitFor(() => waiting.length > 0);
    await fs.writeFile(missing, "first after spawn\n", { flag: "w" });
    await waitFor(() => lines.includes("first after spawn"));
    poller.stop();
  });
});
