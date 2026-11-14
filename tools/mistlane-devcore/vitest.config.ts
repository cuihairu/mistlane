import { defineConfig } from "vitest/config";

export default defineConfig({
  test: {
    include: ["test/**/*.test.ts"],
    // The live attach round binds ports and spawns processes; keep the pool
    // single-threaded so the fixed port ranges never collide.
    pool: "threads",
    poolOptions: { threads: { singleThread: true } },
    testTimeout: 15000,
    reportOnUnnamedTestFailure: false,
  },
});
