import { afterAll, beforeAll, describe, expect, it } from "vitest";
import * as http from "node:http";
import { MetricsPoller, MISTLANE_FAMILIES } from "../src/metrics.js";
import { pickSample } from "../src/promtext.js";

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

async function waitFor(predicate: () => boolean, timeoutMs = 4000): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (predicate()) return;
    await sleep(20);
  }
  throw new Error("condition not reached in time");
}

// Serves the same shape the DashboardServer serves (src/observability.hpp):
// plain HTTP, one response per connection, Content-Length, Connection: close.
function startServer(port: number, body: () => string): Promise<http.Server> {
  return new Promise((resolve) => {
    const server = http.createServer((request, response) => {
      if (request.url === "/metrics") {
        response.writeHead(200, { "content-type": "text/plain; version=0.0.4; charset=utf-8" });
        response.end(body());
        return;
      }
      response.writeHead(404);
      response.end("not found\n");
    });
    server.listen(port, "127.0.0.1", () => resolve(server));
  });
}

describe("MetricsPoller against a live HTTP surface", () => {
  let port = 19801;
  let server: http.Server;
  let sessions = 4;

  beforeAll(async () => {
    server = await startServer(port, () =>
      [
        "# HELP mistlane_active_sessions live game sessions",
        'mistlane_active_sessions ' + sessions,
        'mistlane_ready{role="gameServer"} 1',
        'mistlane_process_resident_memory_bytes 52428800',
      ].join("\n") + "\n",
    );
  });
  afterAll(async () => {
    server.closeAllConnections();
    server.close();
    await sleep(50);
  });

  it("polls /metrics and emits parsed samples on the cadence", async () => {
    const poller = new MetricsPoller({ baseUrl: `http://127.0.0.1:${port}`, intervalMs: 60 });
    const samples: number[] = [];
    const errors: Error[] = [];
    poller.on("sample", (event) => samples.push(pickSample(event.metrics, "mistlane_active_sessions")!));
    poller.on("error", (error) => errors.push(error));
    poller.start();
    sessions = 5;
    try {
      await waitFor(() => samples.length >= 2);
      expect(samples[0]).toBeGreaterThanOrEqual(4);
      expect(samples).toContain(5);
      expect(errors).toEqual([]);
      expect(poller.okAt).not.toBeNull();
    } finally {
      poller.stop();
    }
  });

  it("reports failures and keeps polling until the surface answers again", async () => {
    server.closeAllConnections();
    server.close();
    const deadPort = port;
    const poller = new MetricsPoller({
      baseUrl: `http://127.0.0.1:${deadPort}`,
      intervalMs: 40,
      requestTimeoutMs: 500,
    });
    const errors: Error[] = [];
    const samples: number[] = [];
    poller.on("error", (error) => errors.push(error));
    poller.on("sample", (event) =>
      samples.push(pickSample(event.metrics, MISTLANE_FAMILIES.ready, 'role="gameServer"')!));
    poller.start();
    await waitFor(() => errors.length >= 1);
    expect(poller.okAt).toBeNull();

    // Bring the surface back on the same port.
    server = await startServer(deadPort, () => 'mistlane_ready{role="gameServer"} 1\n');
    await waitFor(() => samples.length >= 1);
    expect(samples[0]).toBe(1);
    poller.stop();
  });

  it("surfaces a non-2xx answer as an error, not a sample", async () => {
    const badPort = ++port;
    const bad = await startServer(badPort, () => "");
    const poller = new MetricsPoller({ baseUrl: `http://127.0.0.1:${badPort}`, path: "/nope", intervalMs: 50 });
    const errors: Error[] = [];
    poller.on("error", (error) => errors.push(error));
    poller.start();
    await waitFor(() => errors.length >= 1);
    expect(errors[0]!.message).toMatch(/HTTP 404/);
    poller.stop();
    bad.closeAllConnections();
    bad.close();
  });

  it("rejects a non-http baseUrl up front", () => {
    expect(() => new MetricsPoller({ baseUrl: "tcp://127.0.0.1:9000" })).toThrow();
  });
});
