# Deployment workflow

Mistlane runs as plain processes (see [Getting started](./guide.md) for the
`mistlane init` cluster and systemd template). This page covers the container
deployment workflow: a runtime image that wraps a compiled binary, a compose
example, the healthcheck semantics, and the drill that validates all of it.

## What ships

- `deploy/Dockerfile` — runtime image. One container runs one node instance.
- `deploy/compose.yaml` — example two-service cluster (`gameServer` plus the
  session-routing `gateway` in front of it), with a config override mounted.
- `deploy/compose/instances/gateway.env` — the gateway's container override
  (upstream by service name instead of the scaffold's loopback default).
- `deploy/compose.stress.yaml` + `deploy/compose/instances/*stress*.env` — the
  cross bench topology (two crossServers and a cross gateway) for the stress drill.
- `deploy/healthcheck.sh` — the image liveness probe.
- `tools/container_smoke.sh` — the deployment validation drill.
- `tools/container_stress.sh` — the paired bare-metal/container stress drill.
- Hostname endpoint addresses (below) — the one framework change the container
  surface needed.

## Build the image

The image wraps a **prebuilt binary**; source builds stay CI's job
(`.github/workflows/ci.yml` builds and tests on push). Stage the binary into
the build context, then build:

```sh
mkdir -p deploy/bin
cp build/debug/mistlane deploy/bin/mistlane     # or a published artifact
docker build -t mistlane:dev -f deploy/Dockerfile deploy/
```

The base image's glibc must be at least as new as the toolchain that built the
binary — the binary links symbol versions from its build host, and an older
base aborts with `version GLIBC_x.y not found`. `BASE_IMAGE` is a build arg,
defaulting to `ubuntu:26.04` (the project build toolchain, glibc 2.43); pass
`--build-arg BASE_IMAGE=ubuntu:24.04` when the artifact came from an older
runner.

The image runs `mistlane init` at build time, so `ops/`, `config/instances/`
and `scripts/` ship with it, and every role is one command away. Loopback-only
`LISTEN` defaults are rewritten to `0.0.0.0`, because a container must bind the
interface Docker publishes rather than the loopback a local cluster uses.

## Run one container

```sh
docker run --rm -p 7102:7102 mistlane:dev gameServer
```

The instance name is the command; the entrypoint is `ops/run-instance.sh`,
which `exec`s the node, so the node is PID 1. `docker stop` therefore delivers
SIGTERM straight to it and the node's graceful shutdown path answers with
exit 0 — the same path `ops/cluster.sh` and the test suite already rely on.

Override configuration or Lua entrypoints by binding over the image paths:

```sh
docker run --rm -p 7102:7102 \
  -v /srv/mistlane/gameServer.env:/opt/mistlane/config/instances/gameServer.env:ro \
  -v /srv/mistlane/scripts:/opt/mistlane/scripts:ro \
  mistlane:dev gameServer
```

The instance files are trusted shell assignments written by the project owner,
and secrets still belong in deployment-managed environment or
`LoadCredential=` (see [Getting started](./guide.md)); do not commit them.

## Cluster with compose

```sh
docker compose -f deploy/compose.yaml up -d
```

The example starts `gameServer` first and the `gateway` only after the health
check reports healthy (`depends_on: condition: service_healthy`). The gateway
reaches `gameServer` over the compose network **by service name**; its override
file is the only mount, so everything else keeps the image default.

## Endpoint addresses accept hostnames

A framework change landed alongside the image: server-side endpoint addresses
now accept a hostname, not only a numeric literal. The client already resolved
names; `parse_address` (`src/node_internal.hpp`) now falls back to
`getaddrinfo` when the text is not an address literal, preferring the first
IPv4 answer and only falling back to IPv6 (an explicit bracketed `[::1]`
literal is still honored). This is what lets `--upstream gameServer:7102`,
`--listen localhost:7102` and FQDNs work on the listener, upstream,
coordinator, data, probe and dashboard endpoints alike.

## Healthcheck semantics

The image healthcheck answers **liveness**, not readiness: it reads the node's
own `--listen` from PID 1's cmdline and opens a TCP connection. A socket that
accepts means the process is up and bound. The node's readiness signal is its
`ready <role> <listen>` stdout line — the same marker `ops/cluster.sh` waits
for — and the internal probe listener (`PROBE1`) is the protocol-level answer
an orchestrator uses when it needs that, not the healthcheck. The drill
asserts both the healthcheck and the ready line, so neither is over-claimed.

## Validation drill

`tools/container_smoke.sh` builds the image from the local binary, then
checks three paths and tears everything down afterwards:

1. the default instance config comes up ready and stops cleanly on SIGTERM
   (exit 0);
2. a bind-mounted instance-file override is honored (the node comes up ready
   on the overridden port, proving the mount and the healthcheck agree);
3. the compose cluster reaches readiness across both services, the gateway
   upstream over service-name DNS, and both stop cleanly.

It skips with a note when `docker` or `docker compose` is unavailable so it
can run in environments without a container runtime. Verified 2026-11-21: all
three paths pass, with the full test suite green (81/81).

## Stress drill

`tools/container_stress.sh` exercises the documented cross bench topology in
the target container environment: leg A runs the same crossServer + cross
gateway on the host as the paired reference — first at the default per-session
flood guard, where the documented bench load is expected to exhaust the token
bucket (the drill asserts all 32 generator clients are stopped and
`mistlane_rate_limit_stops_total` fires on both nodes), then with the guard
raised for the bench level (32 × 500) and with the admission envelope and
per-room cap also raised for the ceiling level (128 × 781); leg B runs
`deploy/compose.stress.yaml` with two crossServers and a cross gateway whose
upstreams are compose service names, at the same raised parameters. Each leg
drives the bundled bot through the published gateway port with the documented
`--join "MLC1 1 realm g bench"` handshake (leg B covers three levels: the
bench, the ceiling, and a 1 MiB `--expect-echo` payload round-trip), then
scrapes `mistlane_request_seconds`, `mistlane_messages_total`,
`mistlane_event_loop_lag_max_seconds`, `mistlane_active_sessions`,
`mistlane_admission_rejections_total`,
`mistlane_process_resident_memory_bytes` and `mistlane_rate_limit_stops_total`
from each published metrics port. A final leg runs two bots concurrently with
distinct room names (`room-a` / `room-b`) and checks the per-shard rx deltas:
one group/room pins every join to one crossServer, so the single-room legs
above leave the other shard idle, while two different room names must spread
across the ordered upstream list. Both legs stop with SIGTERM and assert exit
0. Comparability flags (script, epoch, generator flags, topology) are fixed in
the drill; see [Cross-server and overload](./cross-server.md) and
[Profiling](./profiling.md) for how to read the numbers.

**Verified 2026-11-27** (host = Ubuntu 26.04.1 LTS, Docker 29.1.3, 14 cores,
co-tenant load 5–8): see [Validation](./validation.md) for the measured
throughput, percentiles, lag and RSS. This is a deployment-environment drill,
not a version release and not an orchestration decision.

## Not covered yet

- No source build inside the image: CI produces the binary and this image
  packages it. Building from source in a container is possible but is not
  what the image ships.
- No container-orchestration decisions: lease/fencing, room drain/migration
  and orchestration choices follow real stress tests (see
  [Migration status](./migration-status.md)).
- Crashpad capture is not wired into the container defaults: the scaffold
  instance files have no crashpad fields yet, so containerized dump capture
  needs a scaffold extension first. Crashpad generation and retention on a
  host node are validated ([Validation](./validation.md)).
- No version publish: the image is built and validated locally; publishing a
  versioned image is a release decision (see `RELEASE_NOTES.md` at the
  repository root).
