import { describe, expect, it } from "vitest";
import { ReconnectBackoff } from "../src/backoff.js";

describe("ReconnectBackoff", () => {
  it("grows exponentially from the base", () => {
    const backoff = new ReconnectBackoff({ baseMs: 100, factor: 2, maxMs: 10000 });
    expect(backoff.next()).toBe(100);
    expect(backoff.next()).toBe(200);
    expect(backoff.next()).toBe(400);
    expect(backoff.attemptCount).toBe(3);
  });

  it("caps the delay", () => {
    const backoff = new ReconnectBackoff({ baseMs: 100, factor: 4, maxMs: 500 });
    expect(backoff.next()).toBe(100);
    expect(backoff.next()).toBe(400);
    expect(backoff.next()).toBe(500);
    expect(backoff.next()).toBe(500);
  });

  it("restarts from the base after a reset (a successful connection)", () => {
    const backoff = new ReconnectBackoff({ baseMs: 50, factor: 2 });
    backoff.next();
    backoff.next();
    backoff.next();
    backoff.reset();
    expect(backoff.attemptCount).toBe(0);
    expect(backoff.next()).toBe(50);
  });

  it("adds jitter strictly inside the bound", () => {
    const draws = [0, 0.5, 0.999];
    let call = 0;
    const backoff = new ReconnectBackoff(
      { baseMs: 100, factor: 1, maxMs: 1000, jitterMs: 50 },
      () => draws[call++ % draws.length]!,
    );
    expect(backoff.next()).toBe(100); // jitter 0
    expect(backoff.next()).toBe(125); // jitter half
    expect(backoff.next()).toBeCloseTo(149.95, 2); // jitter top, still < base+bound
    expect(backoff.next()).toBeLessThan(150);
  });

  it("rejects impossible option combinations", () => {
    expect(() => new ReconnectBackoff({ baseMs: 0 })).toThrow();
    expect(() => new ReconnectBackoff({ baseMs: 100, maxMs: 50 })).toThrow();
    expect(() => new ReconnectBackoff({ factor: 0.5 })).toThrow();
    expect(() => new ReconnectBackoff({ jitterMs: -1 })).toThrow();
  });
});
