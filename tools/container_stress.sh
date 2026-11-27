#!/bin/sh
# Target-environment stress drill: the documented cross bench topology
# (docs/cross-server.md, docs/profiling.md) driven for real in the container
# deployment environment. Leg A runs the topology bare-metal on the host as
# the paired reference (identical binary, script, generator flags): first at
# the default per-session flood guard, where the documented bench load is
# expected to exhaust the bucket (200 burst tokens, 100 per second refill) and
# the guard counter must fire; then with the guard raised, which is how the
# bench measures throughput. Leg B brings up deploy/compose.stress.yaml
# (cross-routing gateway in front of two crossServer nodes, upstreams by
# compose service name, guard raised by the mounted instance files), drives
# the bundled bot through the published gateway port at three load levels
# (documented bench, near the generator's connection ceiling, and a 1 MiB
# payload round-trip), and scrapes each node's published metrics face. A
# final two-room leg runs two bots with distinct room names concurrently and
# checks the rx deltas: one room name pins every join to one crossServer, so
# only rooms with different names demonstrate the hash spreading rooms over
# the upstream list. Both legs end with the graceful SIGTERM stop. Every
# assertion prints its own line; a FAIL line exits nonzero.
#
# Exits 0 (with a SKIP note) when docker, the daemon or the binary is missing.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
binary=${MISTLANE_BIN:-$root/build/debug/mistlane}
compose="docker compose -f $root/deploy/compose.stress.yaml"
if ! command -v docker >/dev/null 2>&1; then
  echo "SKIP: docker is not installed"
  exit 0
fi
if ! docker info >/dev/null 2>&1; then
  echo "SKIP: docker daemon is not reachable"
  exit 0
fi
[ -x "$binary" ] || { echo "SKIP: no mistlane binary at $binary (build first)"; exit 0; }

work=$(mktemp -d)
bare_pids=""
cleanup() {
  for pid in $bare_pids; do kill -TERM "$pid" 2>/dev/null || true; done
  $compose down -v --remove-orphans >/dev/null 2>&1 || true
  rm -rf "$work"
}
trap cleanup EXIT

# Stage the binary into the image build context; deploy/bin/ is gitignored.
mkdir -p "$root/deploy/bin"
cp "$binary" "$root/deploy/bin/mistlane"
docker build -q -t mistlane:dev -f "$root/deploy/Dockerfile" "$root/deploy" >/dev/null
echo "image: mistlane:dev built"

bot_line() { # level clients count url extra...
  level=$1; clients=$2; count=$3; url=$4; shift 4
  echo "-- $level: $clients clients x $count requests"
  "$binary" bot --url "$url" --join "MLC1 1 realm g bench" \
    --clients "$clients" --count "$count" "$@" | tee "$work/$level.bot"
  grep -Fq "planned=$((clients * count)) completed=$((clients * count)) failed_clients=0" \
    "$work/$level.bot" || { echo "FAIL: $level did not complete every request"; exit 1; }
}

scrape() { # name port -> one line with the observed families
  name=$1; port=$2
  body=$(curl -sf "http://127.0.0.1:$port/metrics") || { echo "FAIL: $name metrics scrape failed"; exit 1; }
  printf '%s' "$body" > "$work/$name.metrics"
  for family in mistlane_request_seconds mistlane_messages_total \
    mistlane_event_loop_lag_max_seconds mistlane_active_sessions \
    mistlane_admission_rejections_total mistlane_process_resident_memory_bytes \
    mistlane_rate_limit_stops_total; do
    grep -Fq "$family" "$work/$name.metrics" || { echo "FAIL: $name metrics lack $family"; exit 1; }
  done
  lag=$(grep -F 'mistlane_event_loop_lag_max_seconds' "$work/$name.metrics" | awk '$1 !~ /^#/ {print $2; exit}')
  rss=$(grep -F 'mistlane_process_resident_memory_bytes' "$work/$name.metrics" | awk '$1 !~ /^#/ {print $2; exit}')
  rx=$(grep -E 'mistlane_messages_total\{.*direction="rx"\}' "$work/$name.metrics" | awk '$1 !~ /^#/ {print $2; exit}')
  stops=$(grep -F 'mistlane_rate_limit_stops_total' "$work/$name.metrics" | awk '$1 !~ /^#/ {print $2; exit}')
  echo "metrics: $name lag_max=${lag:-?} rss_bytes=${rss:-?} rx=${rx:-?} guard_stops=${stops:-?}"
}

rx_of() { # name port -> the rx message counter as an integer
  name=$1; port=$2
  body=$(curl -sf "http://127.0.0.1:$port/metrics") || { echo "FAIL: $name metrics scrape failed"; exit 1; }
  rx=$(printf '%s' "$body" | grep -E 'mistlane_messages_total\{.*direction="rx"\}' \
    | awk '$1 !~ /^#/ {print $2; exit}')
  case "$rx" in
    ''|*[!0-9]*) echo "FAIL: $name reported no rx counter ($rx)"; exit 1 ;;
  esac
  printf '%s' "$rx"
}

wait_ready() { # logfile marker timeout_seconds process_description
  n=0
  while [ "$n" -lt "$3" ]; do
    grep -Fq "$2" "$1" 2>/dev/null && return 0
    sleep 1; n=$((n + 1))
  done
  echo "FAIL: $4 did not report ready in $3s"; tail -n 5 "$1"; exit 1
}

start_bare() { # log_prefix flags... (applied to both processes)
  prefix=$1; shift
  "$binary" --role crossServer --listen 127.0.0.1:7210 --script "$root/examples/cross.lua" \
    --metrics-listen 127.0.0.1:7213 "$@" >"$work/$prefix-cross.log" 2>&1 &
  bare_pids="$bare_pids $!"
  "$binary" --role gateway --routing cross --listen 127.0.0.1:7211 \
    --upstream 127.0.0.1:7210 --metrics-listen 127.0.0.1:7214 "$@" >"$work/$prefix-gateway.log" 2>&1 &
  bare_pids="$bare_pids $!"
}

bare_codes=""
stop_bare() { # sets bare_codes to the exit codes of the two bare processes;
  # runs in the main shell so wait can reap the children (a command
  # substitution would make every wait answer 127 for a foreign pid)
  for pid in $bare_pids; do
    kill -TERM "$pid" 2>/dev/null || true
  done
  bare_codes=""
  for pid in $bare_pids; do
    if wait "$pid" 2>/dev/null; then bare_codes="$bare_codes 0"; else bare_codes="$bare_codes $?"; fi
  done
  bare_pids=""
  bare_codes=$(echo $bare_codes)
}

echo "== leg A1: bare-metal, default flood guard =="
start_bare a1
wait_ready "$work/a1-gateway.log" "ready gateway 127.0.0.1:7211" 60 "bare-metal gateway"
wait_ready "$work/a1-cross.log" "ready crossServer 127.0.0.1:7210" 60 "bare-metal crossServer"
grep -Eq 'session admission limit [0-9]+' "$work/a1-gateway.log" \
  || { echo "FAIL: gateway printed no admission limit"; exit 1; }
echo "-- guard probe: the documented bench load against the default bucket"
"$binary" bot --url tcp://127.0.0.1:7211 --join "MLC1 1 realm g bench" \
  --clients 32 --count 500 | tee "$work/a1-guard.bot" || true
grep -Fq "failed_clients=32" "$work/a1-guard.bot" \
  || { echo "FAIL: the default guard did not stop the pipelined bench"; exit 1; }
for port in 7213 7214; do
  curl -sf "http://127.0.0.1:$port/metrics" | grep -F 'mistlane_rate_limit_stops_total' \
    | awk '$1 !~ /^#/ && $2 + 0 >= 1 {found=1} END {exit !found}' \
    || { echo "FAIL: guard counter did not fire on the node behind port $port"; exit 1; }
done
echo "guard: default bucket stopped sessions on both nodes, counters fired"
stop_bare
[ "$bare_codes" = "0 0" ] || { echo "FAIL: leg A1 SIGTERM exit codes:$bare_codes (expected 0 0)"; exit 1; }
echo "graceful stop: leg A1 exit 0 0"

echo "== leg A2: bare-metal reference, raised guard =="
start_bare a2 --rate-capacity 1000000 --rate-refill 1000000
wait_ready "$work/a2-gateway.log" "ready gateway 127.0.0.1:7211" 60 "bare-metal gateway"
wait_ready "$work/a2-cross.log" "ready crossServer 127.0.0.1:7210" 60 "bare-metal crossServer"
bot_line a-bench 32 500 tcp://127.0.0.1:7211
scrape cross 7213
scrape gateway 7214
for pid in $bare_pids; do
  grep -E '^VmRSS:' "/proc/$pid/status" >/dev/null || { echo "FAIL: no RSS for pid $pid"; exit 1; }
done
for pid in $bare_pids; do grep -h '^VmRSS:' "/proc/$pid/status"; done
stop_bare
[ "$bare_codes" = "0 0" ] || { echo "FAIL: leg A2 SIGTERM exit codes:$bare_codes (expected 0 0)"; exit 1; }
echo "graceful stop: leg A2 exit 0 0"

echo "== leg A3: bare-metal, raised guard and admission envelope =="
# The connection-ceiling leg needs more than the default envelope (256 MiB
# reserves ~6.3 MiB per session, so the default node admits ~42 sessions);
# 2 GiB admits ~320 and keeps the 128-connection leg inside admission. The
# per-room cap (default 64) is raised on BOTH processes for the same reason:
# the cross gateway counts room participants too, and all 128 joins target
# one room name.
start_bare a3 --rate-capacity 1000000 --rate-refill 1000000 \
  --max-connections 1024 --memory-budget-mib 2048 --max-room-sessions 256
wait_ready "$work/a3-gateway.log" "ready gateway 127.0.0.1:7211" 60 "bare-metal gateway"
wait_ready "$work/a3-cross.log" "ready crossServer 127.0.0.1:7210" 60 "bare-metal crossServer"
limit=$(grep -F 'session admission limit' "$work/a3-gateway.log" | awk '{print $NF}')
[ "$limit" -ge 128 ] || { echo "FAIL: raised envelope admits only $limit sessions"; exit 1; }
echo "envelope: gateway admission limit raised to $limit"
bot_line a-ceiling 128 781 tcp://127.0.0.1:7211
stop_bare
[ "$bare_codes" = "0 0" ] || { echo "FAIL: leg A3 SIGTERM exit codes:$bare_codes (expected 0 0)"; exit 1; }
echo "graceful stop: leg A3 exit 0 0"

echo "== leg B: compose stress cluster (deploy/compose.stress.yaml) =="
$compose up -d >/dev/null
for name in mistlane-stress-cross-a-1 mistlane-stress-cross-b-1 mistlane-stress-gateway-cross-1; do
  n=0
  while [ "$n" -lt 90 ]; do
    status=$(docker inspect --format '{{.State.Health.Status}}' "$name" 2>/dev/null || true)
    [ "$status" = "healthy" ] && break
    sleep 1; n=$((n + 1))
  done
  if [ "$status" != "healthy" ]; then
    echo "FAIL: $name did not become healthy in 90s"
    $compose logs "$name" 2>&1 | tail -n 20 || true
    exit 1
  fi
done
$compose logs cross-a 2>&1 | grep -Fq "ready crossServer 0.0.0.0:7201" \
  || { echo "FAIL: cross-a not ready"; exit 1; }
$compose logs cross-b 2>&1 | grep -Fq "ready crossServer 0.0.0.0:7202" \
  || { echo "FAIL: cross-b not ready"; exit 1; }
$compose logs gateway-cross 2>&1 | grep -Fq "ready gateway 0.0.0.0:7300" \
  || { echo "FAIL: gateway-cross not ready"; exit 1; }
echo "ready: compose cross-a/cross-b/gateway-cross"

bot_line b-bench 32 500 tcp://127.0.0.1:7300
bot_line b-ceiling 128 781 tcp://127.0.0.1:7300
head -c 1048576 /dev/urandom > "$work/payload.bin"
bot_line b-payload 32 16 tcp://127.0.0.1:7300 --payload-file "$work/payload.bin" --expect-echo
scrape cross-a 7203
scrape cross-b 7204
scrape gateway 7301
docker stats --no-stream --format '{{.Name}} {{.MemUsage}}' \
  mistlane-stress-cross-a-1 mistlane-stress-cross-b-1 mistlane-stress-gateway-cross-1 \
  > "$work/b-stats" || { echo "FAIL: docker stats failed"; exit 1; }
cat "$work/b-stats"

echo "== leg B2: room-hash distribution across both crossServers =="
# Every join above uses the room name "bench", and one group/room pins to one
# crossServer, so those legs leave the other shard idle. Two concurrent bots
# with distinct room names show the FNV-1a room hash spreading rooms over the
# ordered upstream list: each bot should land entirely on one shard, and the
# two shards should each take one room. The names are pinned because their
# hashes differ modulo the two upstreams (verified against this topology);
# another name pair may legally collide on a single shard.
before_a=$(rx_of cross-a 7203)
before_b=$(rx_of cross-b 7204)
"$binary" bot --url tcp://127.0.0.1:7300 --join "MLC1 1 realm g room-a" \
  --clients 32 --count 150 >"$work/b-room-a.bot" 2>&1 &
pa=$!
"$binary" bot --url tcp://127.0.0.1:7300 --join "MLC1 1 realm g room-b" \
  --clients 32 --count 150 >"$work/b-room-b.bot" 2>&1 &
pb=$!
wait "$pa" || { echo "FAIL: room-a bot exited $?"; cat "$work/b-room-a.bot"; exit 1; }
wait "$pb" || { echo "FAIL: room-b bot exited $?"; cat "$work/b-room-b.bot"; exit 1; }
for room in room-a room-b; do
  cat "$work/b-$room.bot"
  grep -Fq "planned=4800 completed=4800 failed_clients=0" "$work/b-$room.bot" \
    || { echo "FAIL: $room did not complete every request"; exit 1; }
done
delta_a=$(($(rx_of cross-a 7203) - before_a))
delta_b=$(($(rx_of cross-b 7204) - before_b))
echo "distribution: cross-a rx +$delta_a, cross-b rx +$delta_b"
# Each bots' 32 sessions hand over 32 MLC1 joins plus 32x150 frames, so a
# shard that took one whole room gained 4832 rx messages; requiring both
# deltas above the per-room frame count proves both shards took real load.
[ "$delta_a" -ge 4800 ] && [ "$delta_b" -ge 4800 ] \
  || { echo "FAIL: one crossServer stayed idle (rx +$delta_a/+$delta_b)"; exit 1; }

$compose stop -t 30 >/dev/null
for name in mistlane-stress-cross-a-1 mistlane-stress-cross-b-1 mistlane-stress-gateway-cross-1; do
  code=$(docker inspect --format '{{.State.ExitCode}}' "$name" 2>/dev/null || true)
  [ "$code" = "0" ] || { echo "FAIL: $name exit code $code (expected 0)"; exit 1; }
done
echo "graceful stop: compose exit 0/0/0"

echo "PASS: container stress drill"
