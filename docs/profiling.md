# Profiler workflow

The profiler route decision ([cross-server and overload](./cross-server.md)) fixed the
approach: sample a running node from outside the process. A sampler inside the Asio loop
spends the same CPU budget it measures, so it cannot establish its own overhead. This page
is the working recipe — access, a repeatable load scenario, recording and symbolization,
and the measured sampling impact on the development host.

## Access: why `sudo perf`

The development host sets `kernel.perf_event_paranoid=4`, which refuses event recording to
processes without `CAP_PERFMON`, and `kernel.yama.ptrace_scope=1`, which blocks attaching
strace or gdb to a non-child process. The privileged path still works: a process holding
`CAP_PERFMON` (sudo) is exempt from `perf_event_paranoid`, so `sudo perf record` opens
events against a user-owned node (verified 2026-11-15, perf 7.0.14, kernel 7.0.0-43).
`perf stat` and `perf record` under sudo are the whole requirement; no sysctl needs to
change and no binary needs capabilities.

## Repeatable load scenario

Same topology as the cross bench in [cross-server and overload](./cross-server.md):
a crossServer hosting a script, one cross gateway in front of it, the bundled generator
driving both.

```
mistlane --role crossServer --listen 127.0.0.1:7210 \
  --script examples/cross.lua --metrics-listen 127.0.0.1:7213 &
mistlane --role gateway --routing cross --listen 127.0.0.1:7211 \
  --upstream 127.0.0.1:7210 --metrics-listen 127.0.0.1:7214 &
mistlane bot --url tcp://127.0.0.1:7211 \
  --join "MLC1 1 realm g bench" --clients 32 --count 500
```

The generator is closed-loop (32 serial connections), so throughput and per-request
percentiles come straight from the bot's summary line; `mistlane_event_loop_lag_max_seconds`
and RSS (poll `/proc/PID/status`) cover the node side. `--join` performs the `MLC1`
handshake once per connection and expects `MLC1 OK 1` (src/bot.cpp); keep `--clients`
below the admission limit printed at startup.

Comparability discipline:

- Identical generator flags, script and topology, or the numbers are noise (see
  [cross-server and overload](./cross-server.md)).
- Pin every process: both nodes are single-threaded, so `taskset -c 2/3/4` for
  crossServer, gateway and bot removes scheduler migration as a variable.
- Gate runs on load average. On this shared host, two arms of one sweep lost their nodes
  mid-run and two baseline arms of the same sweep differed by 3.2x in throughput while a
  co-tenant build pushed loadavg to 169 on 14 cores — starved nodes blow their own bounded
  admission and timeout windows and clients see `TCP: End of file`. Compare arms only
  within one sweep session under comparable load, and rerun any arm that died mid-run.

## Record and symbolize

```
sudo perf record -e cycles:u -F 99 --call-graph=dwarf,4096 \
  -p $(cat cross.pid) -o perf.data &
# start the bot run; when it returns:
sudo pkill -INT -f "perf record.*perf\.data"   # SIGINT flushes the data file
sudo perf report -i perf.data --stdio --no-children --percent-limit 2 -f
```

- Debug builds carry full symbols; user-space frames resolve directly.
- perf embeds build ids. Report against the same binary that was recorded — a rebuild
  between record and report silently breaks attribution (`perf buildid-list` shows what
  the file expects).
- Kernel samples resolve only for the privileged user (`kptr_restrict=1`); user-only
  recording (`cycles:u`) keeps the file small and the interesting frames userspace.
- The `-f` flag is needed when the data file was written by root and is read back after a
  `chown` (perf refuses files not owned by the current user by default).

Symbolized output from the demo capture (2026-11-15, `-F 99` DWARF, the cross bench
above; 75 samples, 0 lost) — user-space frames resolve to the binary's own symbols with
full call chains:

```
     3.47%  mistlane  mistlane  [.] boost::asio::detail::conditionally_enabled_mutex<boost::asio::detail::futex_slim_mutex>::scoped_lock::unlock()
     3.06%  mistlane  mistlane  [.] luaS_hash
            ---luaS_hash
               internshrstr
               luaS_newlstr
               luaV_concat
               luaV_execute
               ccall
               luaD_callnoyield
               f_call
               luaD_rawrunprotected
               luaD_pcall
               lua_pcallk
```

The two leading entries are the expected shape for this scenario: Asio session plumbing,
and Lua string interning on the echo path (`lua_pcallk` → `luaV_execute` →
`luaV_concat` → `luaS_newlstr`).

## Measured sampling impact

Two measurement sessions on the development host (2026-11-15, debug build, co-tenant
loadavg 86–195 on 14 cores throughout; every process pinned as above).

**Session 1 — sequential arms** (one node pair at a time, identical flags per arm):
the host itself invalidates sequential comparison. Two baseline arms of the same sweep
differed by 3.2x in throughput, and two other arms lost their nodes mid-run when co-tenant
load peaked (every bot client reports `TCP: End of file`):

| arm | completed | rps | p50 / p99 ms |
|---|---|---|---|
| baseline | 16000/16000 | 712 | 41.0 / 110.3 |
| 99 Hz DWARF | 16000/16000 | 949 | 29.1 / 95.0 |
| 99 Hz fp | env-killed | — | — |
| 999 Hz DWARF | env-killed | — | — |
| baseline (closing) | 16000/16000 | 2298 | 7.5 / 64.4 |

**Session 2 — paired runs** (two identical stacks started at the same instant, the
treatment crossServer under sudo perf; shared-host load hits both stacks, so ratios
remove it). Three repetitions per condition; the sanity condition runs no profiler on
either side:

| condition | rps ratio trt/ctrl | sampled-node CPU delta | RSS |
|---|---|---|---|
| sanity (no sampler) | 1.33 / 1.31 / 1.42 | +10% (1 sample) | flat ~17 MB |
| 99 Hz DWARF | 0.98 / 1.02 (third pair env-killed) | −1% (median) | flat |
| 999 Hz DWARF | 1.00 / 0.97 / 0.89 | **+20%** (+14/+20/+27) | flat |
| 99 Hz fp | 1.14 / 1.48 / 0.88 | +13% (median) | flat |

Observations:

- **RSS is flat** (~17 MB, all 12 pairs plus session 1) — the sampler is out-of-process
  and adds no resident memory to the node.
- **Correctness is unaffected**: every valid repetition completed exactly `planned`
  requests with `failed_clients=0`. The killed repetitions died on *both* stacks of a
  pair simultaneously — an environmental kill, symmetric, not a sampler fault.
- **The sanity pair is not interchangeable**: its ratios run consistently above 1 (the
  two core sets are not equivalent under heavy co-tenant load), so absolute ratios are
  not comparable across conditions. The load-robust signal is the sampled node's own
  CPU: **999 Hz DWARF adds a consistent +14–27% CPU**, while both 99 Hz modes sit at or
  below the +10% accounting floor of the sanity pair. Wall-clock trends agree in
  direction (999 Hz ratios drift down to 0.89).
- **`-F 999` with DWARF callchains also trips the kernel's auto-throttle on this host**:
  dmesg cascaded `kernel.perf_event_max_sample_rate` down (100000 → 79000 → 61000 →
  47000 → 37000) while a sustained 999 Hz arm ran, and that data file holds almost no
  samples. Treat 999 Hz DWARF as a short-burst tool here, not a sustained mode.
- **`-F 99` with DWARF callchains is below this host's measurement floor** — the default
  recipe in [cross-server and overload](./cross-server.md) holds, and a quieter window
  can only shrink the bound.

Guidance: record at `-F 99 --call-graph=dwarf,4096`; keep 999 Hz DWARF to short bursts
(and expect the kernel to throttle it); use frame pointers only when DWARF unwinding is
measurably too costly on the target host; on a shared host, measure sampler cost with
paired runs and the node's own CPU time, never with sequential wall-clock arms.
