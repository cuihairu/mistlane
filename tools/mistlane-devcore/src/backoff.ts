// Reconnect backoff shared by the transport and any polling source that
// needs to retry: exponential growth from a base delay up to a hard cap, an
// attempt counter the caller resets on a successful round trip, and optional
// bounded jitter so many clients do not reconnect in lockstep.

export interface BackoffOptions {
  /** Delay after the first failure, in milliseconds. Default 500. */
  baseMs?: number;
  /** Growth multiplier. Default 2. */
  factor?: number;
  /** Upper bound, in milliseconds. Default 15000. */
  maxMs?: number;
  /** Additive jitter bound: delay is uniform in [value, value + jitterMs). Default 0. */
  jitterMs?: number;
}

export class ReconnectBackoff {
  private attempts = 0;
  private readonly baseMs: number;
  private readonly factor: number;
  private readonly maxMs: number;
  private readonly jitterMs: number;
  private readonly random: () => number;

  constructor(options: BackoffOptions = {}, random: () => number = Math.random) {
    const base = options.baseMs ?? 500;
    const max = options.maxMs ?? 15000;
    if (!(base > 0) || !(options.factor === undefined || options.factor >= 1) || !(max >= base) ||
        !(options.jitterMs === undefined || options.jitterMs >= 0)) {
      throw new Error("invalid backoff options");
    }
    this.baseMs = base;
    this.factor = options.factor ?? 2;
    this.maxMs = max;
    this.jitterMs = options.jitterMs ?? 0;
    this.random = random;
  }

  /** Delay for the next attempt; increases every call until reset(). */
  next(): number {
    this.attempts += 1;
    // attempts starts at 1, so the first delay is the base itself.
    const raw = this.baseMs * Math.pow(this.factor, this.attempts - 1);
    const capped = Math.min(raw, this.maxMs);
    if (this.jitterMs === 0) return capped;
    return capped + this.random() * this.jitterMs;
  }

  get attemptCount(): number {
    return this.attempts;
  }

  reset(): void {
    this.attempts = 0;
  }
}
