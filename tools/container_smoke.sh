#!/bin/sh
# Container deployment smoke: build the runtime image from the local binary,
# start a gameServer container, wait for the image healthcheck to report
# healthy and for the node's `ready <role> <listen>` line to appear, then stop
# it and assert the graceful SIGTERM path exits 0. Repeats once with a mounted
# instance-file override so the config-override mount path is covered too.
#
# Exits 0 (with a SKIP note) when docker is unavailable so the drill can be
# run in environments without a container runtime; never leaves a container or
# image behind.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
binary=${MISTLANE_BIN:-$root/build/debug/mistlane}
if ! command -v docker >/dev/null 2>&1; then
  echo "SKIP: docker is not installed"
  exit 0
fi
if ! docker info >/dev/null 2>&1; then
  echo "SKIP: docker daemon is not reachable"
  exit 0
fi
[ -x "$binary" ] || { echo "SKIP: no mistlane binary at $binary (build first)"; exit 0; }

image=mistlane:smoke
container=mistlane-smoke-$$
override_dir=$(mktemp -d)
cleanup() {
  docker compose -f "$root/deploy/compose.yaml" down -v --remove-orphans >/dev/null 2>&1 || true
  docker rm -f "$container" >/dev/null 2>&1 || true
  rm -rf "$root/deploy/bin" "$override_dir"
}
trap cleanup EXIT

# Stage the binary into the image build context; deploy/bin/ is gitignored.
mkdir -p "$root/deploy/bin"
cp "$binary" "$root/deploy/bin/mistlane"
docker build -q -t "$image" -f "$root/deploy/Dockerfile" "$root/deploy" >/dev/null

start_case() {
  name=$1; listen=$2
  shift 2
  docker run -d --name "$container" "$@" "$image" "$name" >/dev/null
  # Poll the image healthcheck until it reports healthy.
  n=0
  while [ "$n" -lt 60 ]; do
    status=$(docker inspect --format '{{.State.Health.Status}}' "$container" 2>/dev/null || true)
    [ "$status" = "healthy" ] && break
    sleep 1
    n=$((n + 1))
  done
  if [ "$status" != "healthy" ]; then
    echo "FAIL: $name container did not become healthy in 60s"
    docker logs "$container" 2>&1 | tail -n 20 || true
    exit 1
  fi
  docker logs "$container" 2>&1 | grep -Fq "ready $name $listen" || {
    echo "FAIL: $name did not log ready $name $listen"
    docker logs "$container" 2>&1 | tail -n 20 || true
    exit 1
  }
  echo "ready: $name on $listen"
}

echo "== default instance config =="
start_case gameServer 0.0.0.0:7102
docker stop -t 30 "$container" >/dev/null
code=$(docker inspect --format '{{.State.ExitCode}}' "$container")
[ "$code" = "0" ] || { echo "FAIL: SIGTERM exit code $code (expected 0)"; exit 1; }
echo "graceful stop: exit 0"
docker rm -f "$container" >/dev/null

echo "== mounted instance-file override =="
# Point the same instance at a different port through a bind-mounted override;
# the healthcheck must read the overridden LISTEN, not the image default.
printf 'ROLE=gameServer\nLISTEN=0.0.0.0:7302\nSCRIPT=scripts/echo.lua\nTRANSPORT=tcp\nUPSTREAMS=\nROUTE_EPOCH=\n' \
  > "$override_dir/gameServer.env"
start_case gameServer 0.0.0.0:7302 \
  -v "$override_dir/gameServer.env:/opt/mistlane/config/instances/gameServer.env:ro"
docker stop -t 30 "$container" >/dev/null
code=$(docker inspect --format '{{.State.ExitCode}}' "$container")
[ "$code" = "0" ] || { echo "FAIL: override SIGTERM exit code $code (expected 0)"; exit 1; }
echo "override: ready + graceful stop"
docker rm -f "$container" >/dev/null

# Compose cluster: gameServer first, then a gateway that waits on its health
# check (depends_on: service_healthy) and reaches it over the compose network
# by service name.
echo "== compose cluster =="
if ! docker compose version >/dev/null 2>&1; then
  echo "SKIP: docker compose is not installed"
else
  # Reuse the layers the smoke image already built.
  docker build -q -t mistlane:dev -f "$root/deploy/Dockerfile" "$root/deploy" >/dev/null
  docker compose -f "$root/deploy/compose.yaml" up -d >/dev/null
  for name in mistlane-gameServer-1 mistlane-gateway-1; do
    n=0
    while [ "$n" -lt 90 ]; do
      status=$(docker inspect --format '{{.State.Health.Status}}' "$name" 2>/dev/null || true)
      [ "$status" = "healthy" ] && break
      sleep 1
      n=$((n + 1))
    done
    if [ "$status" != "healthy" ]; then
      echo "FAIL: $name did not become healthy in 90s"
      docker compose -f "$root/deploy/compose.yaml" logs "$name" 2>&1 | tail -n 20 || true
      exit 1
    fi
  done
  # Readiness marker per node; the gateway only becomes ready once it has its
  # gameServer upstream from the mounted override file.
  docker logs mistlane-gameServer-1 2>&1 | grep -Fq "ready gameServer 0.0.0.0:7102" \
    || { echo "FAIL: gameServer did not log ready"; exit 1; }
  docker logs mistlane-gateway-1 2>&1 | grep -Fq "ready gateway 0.0.0.0:7100" \
    || { echo "FAIL: gateway did not log ready"; exit 1; }
  echo "cluster: gameServer and gateway both ready"
  # stop keeps the containers so exit codes stay inspectable; the compose
  # stop_grace_period (30s) lets the SIGTERM graceful path finish.
  docker compose -f "$root/deploy/compose.yaml" stop -t 30 >/dev/null
  for name in mistlane-gameServer-1 mistlane-gateway-1; do
    code=$(docker inspect --format '{{.State.ExitCode}}' "$name" 2>/dev/null || true)
    [ "$code" = "0" ] || { echo "FAIL: $name exit code $code (expected 0)"; exit 1; }
  done
  echo "cluster: graceful stop exit 0"
  docker compose -f "$root/deploy/compose.yaml" rm -f >/dev/null 2>&1 || true
fi

echo "PASS: container deployment smoke"
