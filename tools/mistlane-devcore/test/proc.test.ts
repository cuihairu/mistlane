import { describe, expect, it } from "vitest";
import * as fs from "node:fs/promises";
import * as os from "node:os";
import * as path from "node:path";
import { ProcSampler, parseProcStat, parseProcStatus, readPidFile } from "../src/proc.js";

// Fixture shaped exactly like the real /proc/<pid>/status fields (proc(5)).
const STATUS = [
  "Name:\tmistlane",
  "Umask:\t0022",
  "State:\tS (sleeping)",
  "Pid:\t4242",
  "VmPeak:\t  220000 kB",
  "VmSize:\t  180000 kB",
  "VmRSS:\t   65536 kB",
  "Threads:\t12",
  "Cpus_allowed:\tff",
].join("\n");

// Field 2 (comm) contains spaces, the case that breaks naive splits.
const STAT = "4242 (mistlane node) S 1 4242 4242 0 -1 4194560 100 200 0 0 80 40 0 0 20 0 12 0 12345 18446744073709551615 0 0 0 0 0 0 0 0";

describe("proc parsers", () => {
  it("parses status kB values and thread count", () => {
    const status = parseProcStatus(STATUS);
    expect(status.rssBytes).toBe(65536 * 1024);
    expect(status.vmSizeBytes).toBe(180000 * 1024);
    expect(status.threads).toBe(12);
  });

  it("tolerates absent fields", () => {
    const status = parseProcStatus("Name:\tx\n");
    expect(status.rssBytes).toBeNull();
    expect(status.threads).toBeNull();
  });

  it("parses stat after the last paren, summing utime+stime", () => {
    const stat = parseProcStat(STAT)!;
    // utime=80 stime=40 → 120; starttime is field 22.
    expect(stat.cpuTicks).toBe(120);
    expect(stat.starttimeTicks).toBe(12345);
  });

  it("rejects stat text without a comm paren", () => {
    expect(parseProcStat("garbage")).toBeNull();
  });

  it("computes cpu fraction from the tick delta between two reads", async () => {
    let utime = 100;
    let clock = 0;
    const reads = [
      "Name:\tx\nVmRSS:\t   1 kB\nThreads:\t1\n",
      "Name:\tx\nVmRSS:\t   2 kB\nThreads:\t1\n",
    ];
    let call = 0;
    const sampler = new ProcSampler({
      pid: 1,
      intervalMs: 5,
      clockTicks: 100,
      readFile: (file) => {
        if (file.endsWith("/status")) return Promise.resolve(reads[Math.min(call++, 1)]!);
        // +25 utime ticks per read at a 100 ms cadence = 2.5 cores.
        utime += 25;
        return Promise.resolve(`1 (x) S ${"0 ".repeat(10)}${utime} 0 0 0 20 0 1 0 7`);
      },
      now: () => (clock += 100),
    });
    const first = await sampler.sampleOnce();
    expect(first!.cpuFraction).toBeNull(); // no baseline yet
    const second = await sampler.sampleOnce();
    expect(second!.cpuFraction).toBeCloseTo(2.5);
    expect(second!.cpuPercent).toBeCloseTo(250);
    expect(second!.rssBytes).toBe(2 * 1024);
  });

  it("fails softly when the pid disappears", async () => {
    const errors: Error[] = [];
    const sampler = new ProcSampler({
      pid: 1,
      readFile: () => Promise.reject(new Error("ENOENT")),
    });
    sampler.on("error", (error) => errors.push(error));
    const sample = await sampler.sampleOnce();
    expect(sample).toBeNull();
    expect(errors).toHaveLength(1);
  });
});

describe("readPidFile (scaffold var/run/<name>.pid)", () => {
  it("reads the plain integer the ops script writes", async () => {
    const dir = await fs.mkdtemp(path.join(os.tmpdir(), "mistlane-pid-"));
    const pidFile = path.join(dir, "gameServer.pid");
    await fs.writeFile(pidFile, "4242\n");
    await expect(readPidFile(pidFile)).resolves.toBe(4242);
    await fs.writeFile(pidFile, "not a pid\n");
    await expect(readPidFile(pidFile)).rejects.toThrow();
    await fs.rm(dir, { recursive: true, force: true });
  });
});
