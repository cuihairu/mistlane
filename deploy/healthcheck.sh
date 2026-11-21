#!/bin/bash
# Container liveness probe: check that the running node's --listen port accepts
# a TCP connection. The address is read from PID 1's own cmdline, so it always
# matches what the node actually bound — a mounted config override or a
# custom command is honored without any extra wiring.
#
# This answers liveness (process up, socket bound), not readiness; the node's
# readiness signal is its `ready <role> <listen>` stdout line, and the internal
# probe listener (PROBE1) is the protocol-level answer when one is needed.
set -eu

# run-instance.sh execs the binary, so PID 1 is the node and its argv carries
# the effective --listen value.
listen=$(tr '\0' '\n' </proc/1/cmdline | awk '/^--listen$/{getline; print; exit}')
[ -n "$listen" ] || { echo "no --listen in pid 1"; exit 1; }
case "$listen" in
  *:*) ;;
  *) echo "unparsable --listen: $listen"; exit 1 ;;
esac
host=${listen%:*}
port=${listen##*:}
# 0.0.0.0 is the bind-side wildcard; the probe connects over loopback.
case "$host" in 0.0.0.0|::|'[::]') host=127.0.0.1 ;; esac

# bash's /dev/tcp: open the connection and close it immediately.
if (exec 3<>"/dev/tcp/$host/$port") 2>/dev/null; then
  echo "up $listen"
  exit 0
fi
echo "connect failed: $listen"
exit 1
