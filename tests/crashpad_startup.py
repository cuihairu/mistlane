#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Smoke-test optional local Crashpad handler startup and clean shutdown."""
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time

binary, handler, script = map(lambda item: str(Path(item).resolve()), sys.argv[1:4])
with socket.socket() as probe:
    probe.bind(("127.0.0.1", 0))
    listen_port = probe.getsockname()[1]
with tempfile.TemporaryDirectory(prefix="mistlane-crashpad-") as temporary:
    database = Path(temporary) / "reports"
    log_path = Path(temporary) / "node.log"
    with log_path.open("w+") as log:
        process = subprocess.Popen([
            binary, "--role", "worker", "--listen", f"127.0.0.1:{listen_port}",
            "--script", script, "--crashpad-handler", handler,
            "--crashpad-database", str(database),
        ], stdout=log, stderr=log)
        deadline = time.monotonic() + 8
        ready = False
        while time.monotonic() < deadline:
            log.flush()
            log.seek(0)
            output = log.read()
            if "ready worker " in output:
                ready = True
                break
            if process.poll() is not None:
                raise AssertionError(f"node exited during Crashpad startup: {output}")
            time.sleep(0.05)
        if not ready:
            process.kill()
            process.wait(timeout=3)
            raise AssertionError("node did not become ready with Crashpad enabled")
        assert (database / "settings.dat").exists() or any(database.iterdir())
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)
            raise AssertionError("node did not stop cleanly")
        assert process.returncode == 0, f"unexpected node exit: {process.returncode}"
print("PASS: Crashpad handler starts, local database initializes, and node shuts down cleanly")
