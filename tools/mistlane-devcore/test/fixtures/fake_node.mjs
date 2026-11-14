// A minimal stand-in for an attached node process, used by attach.live.test.ts:
// a real OS process that logs to stdout/stderr (the parent redirects both into
// a file, the same ">>$root/var/log/<name>.log 2>&1" mechanism the scaffold ops
// scripts use), serves a /metrics surface with moving values, and burns a
// little CPU each cycle so the /proc sampler sees a changing curve. The client
// side is the real devcore code; nothing here is mocked at that boundary.
import * as http from "node:http";

const [port] = process.argv.slice(2);
let sessions = 0;
let messages = 0;
let cpuBurn = 0;

const server = http.createServer((request, response) => {
  if (request.url === "/metrics") {
    response.writeHead(200, { "content-type": "text/plain; version=0.0.4" });
    response.end(
      [
        `mistlane_active_sessions ${sessions}`,
        `mistlane_ready{role="gameServer"} 1`,
        `mistlane_messages_total{direction="tx"} ${messages}`,
        `mistlane_messages_total{direction="rx"} ${messages}`,
        `mistlane_process_resident_memory_bytes ${process.memoryUsage().rss}`,
        `mistlane_event_loop_lag_max_seconds 0.001`,
      ].join("\n") + "\n",
    );
    return;
  }
  response.writeHead(404);
  response.end();
});
server.listen(Number(port), "127.0.0.1");

setInterval(() => {
  sessions = 1 + (sessions % 8);
  messages += 4;
  console.log(`info session joined count=${sessions}`);
  console.error(`warn load sample messages=${messages}`);
  // Burn ~15ms of CPU so the /proc cpu tick counter moves measurably.
  const start = process.hrtime.bigint();
  cpuBurn += 1;
  while (Number(process.hrtime.bigint() - start) < 15_000_000) cpuBurn = (cpuBurn + 1) % Number.MAX_SAFE_INTEGER;
}, 200);
