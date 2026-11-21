# Runtime image: packages a prebuilt mistlane binary as the deployment unit.
#
# Source builds stay CI's job (see .github/workflows/ci.yml); this image wraps a
# compiled binary so every node role ships as one reproducible container. The
# build context must contain `bin/mistlane` (staged by the build workflow or the
# container smoke drill, tools/container_smoke.sh); nothing else is required.
#
# The base image's glibc must be at least as new as the toolchain that built the
# binary: the binary links symbol versions from the build host, and an older
# base aborts with "version GLIBC_x.y not found". ubuntu:26.04 matches the
# project's build toolchain (glibc 2.43); override BASE_IMAGE when the artifact
# was built on an older runner (for example ubuntu:24.04 for glibc 2.39).
ARG BASE_IMAGE=ubuntu:26.04
FROM ${BASE_IMAGE}

# libquadmath0 is the GCC math runtime the binary links; the base already
# carries libstdc++6, libc and bash (the healthcheck relies on bash /dev/tcp).
RUN apt-get update \
 && apt-get install -y --no-install-recommends libquadmath0 \
 && rm -rf /var/lib/apt/lists/*

# The compiled node binary.
COPY bin/mistlane /usr/local/bin/mistlane

# Generate the deployment scaffold (ops/, config/instances/, scripts/) inside the
# image so one image serves every role: the instance to run is the command, and
# operators override config or Lua entrypoints by mounting over these paths.
#
# The scaffold defaults bind loopback, which is correct for a local cluster and
# wrong inside a container, where the published interface is a bridge address.
# Rewrite LISTEN to 0.0.0.0 so a published port reaches the node; the rewrite
# only touches the loopback default and leaves every other field alone.
RUN /usr/local/bin/mistlane init /opt/mistlane \
 && sed -i 's/^LISTEN=127\.0\.0\.1:/LISTEN=0.0.0.0:/' /opt/mistlane/config/instances/*.env \
 && chown -R root:root /opt/mistlane

# Liveness probe for HEALTHCHECK: reads the node's own --listen from PID 1's
# cmdline and checks the port accepts connections.
COPY healthcheck.sh /opt/mistlane/deploy/healthcheck.sh

# Dedicated service account; the node writes nothing outside /opt/mistlane/var.
RUN useradd --uid 10001 --user-group --home-dir /nonexistent --shell /usr/sbin/nologin mistlane

USER mistlane
WORKDIR /opt/mistlane
ENV MISTLANE_BIN=/usr/local/bin/mistlane \
    MISTLANE_CONFIG_DIR=/opt/mistlane/config/instances

# One container runs one instance. run-instance.sh execs the binary, so the node
# is PID 1: `docker stop` delivers SIGTERM to it and the node's existing
# graceful shutdown path answers with exit 0.
ENTRYPOINT ["/opt/mistlane/ops/run-instance.sh"]
CMD ["gameServer"]

# Liveness only: the listen port accepting means the process is up and bound.
# The node's real readiness signal is its `ready <role> <listen>` stdout line
# (the same marker ops/cluster.sh waits for); use the internal probe listener
# (PROBE1) when an orchestrator needs that answer, not this port check.
HEALTHCHECK --interval=10s --timeout=5s --start-period=20s --retries=5 \
  CMD /opt/mistlane/deploy/healthcheck.sh
