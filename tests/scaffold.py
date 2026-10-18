#!/usr/bin/env python3
"""Checks the built-in project initializer without external packages."""
from pathlib import Path
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1]).resolve())

with tempfile.TemporaryDirectory() as temp:
    root = Path(temp) / "sample service"
    result = subprocess.run([binary, "init", str(root)], check=True, capture_output=True, text=True)
    assert "Initialized sample-service" in result.stdout
    expected = {
        "README.md", "scripts/echo.lua", "scripts/http.lua", "scripts/cross.lua",
        "run-local.sh", "run-cross-local.sh", "ops/run-instance.sh", "ops/cluster.sh",
        "ops/systemd/mistlane@.service", "config/instances/worker.env",
        "config/instances/router.env", "config/instances/gateway.env",
        "config/instances/shard-a.env", "config/instances/shard-b.env",
        "config/instances/cross.env", "config/instances/http.env",
    }
    actual = {p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file()}
    assert actual == expected, actual
    for script in (root / "run-local.sh", root / "run-cross-local.sh",
                   root / "ops/run-instance.sh", root / "ops/cluster.sh"):
        assert script.stat().st_mode & 0o111
        subprocess.run(["sh", "-n", str(script)], check=True)
    readme = (root / "README.md").read_text()
    assert "# sample-service" in readme
    assert "127.0.0.1:7300" in readme
    assert (root / "var/run").is_dir() and (root / "var/log").is_dir()

    # Run the generated worker configuration with the just-built binary and
    # verify the project-relative Lua script path is resolved correctly.
    import os
    import socket
    import struct
    import time
    env = os.environ.copy()
    env["MISTLANE_BIN"] = binary
    manager = [str(root / "ops/cluster.sh")]
    subprocess.run(manager + ["start", "worker"], cwd=root, env=env,
                   check=True, capture_output=True, text=True)
    try:
        deadline = time.monotonic() + 5
        while True:
            try:
                sock = socket.create_connection(("127.0.0.1", 7102), timeout=.2)
                break
            except OSError:
                status = subprocess.run(manager + ["status", "worker"], cwd=root, env=env,
                                        capture_output=True, text=True)
                if "worker stopped" in status.stdout or time.monotonic() >= deadline:
                    raise AssertionError("generated worker did not start")
                time.sleep(.05)
        with sock:
            payload = b"template-path-check"
            sock.sendall(struct.pack("!I", len(payload)) + payload)
            assert sock.recv(4) == struct.pack("!I", len(payload))
            assert sock.recv(len(payload)) == payload
    finally:
        subprocess.run(manager + ["stop", "worker"], cwd=root, env=env,
                       check=True, capture_output=True, text=True)

    sentinel = root / "keep.txt"
    sentinel.write_text("preserve")
    failed = subprocess.run([binary, "init", str(root)], capture_output=True, text=True)
    assert failed.returncode != 0
    assert sentinel.read_text() == "preserve"

help_result = subprocess.run([binary, "init", "--help"], check=True, capture_output=True, text=True)
assert "DIRECTORY" in help_result.stdout
print("PASS: initializer creates a complete non-overwriting multi-role starter")
