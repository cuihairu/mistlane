#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
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
        "ops/systemd/mistlane@.service", "config/instances/gameServer.env",
        "config/instances/gateway.env",
        "config/instances/gateway-ws.env", "config/instances/gateway-wss.env",
        "config/instances/crossServer-a.env", "config/instances/crossServer-b.env",
        "config/instances/gateway-cross.env", "config/instances/apiServer.env",
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

    # Check TLS path/argument forwarding independently of certificate generation.
    # The transport integration suite separately verifies real WS/WSS handshakes.
    import os
    capture = root / "captured-args.txt"
    recorder = root / "record-args.sh"
    recorder.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "$MISTLANE_CAPTURE"\n')
    recorder.chmod(0o755)
    tls_env = os.environ.copy()
    tls_env.update(MISTLANE_BIN=str(recorder), MISTLANE_CAPTURE=str(capture))
    launcher = str(root / "ops/run-instance.sh")
    subprocess.run([launcher, "gateway-ws"], cwd="/", env=tls_env, check=True)
    assert "--transport\nws\n" in capture.read_text()
    missing = subprocess.run([launcher, "gateway-wss"], cwd="/", env=tls_env,
                             capture_output=True, text=True)
    assert missing.returncode != 0 and "unreadable TLS" in missing.stderr
    cert = root / "certs/server chain.pem"
    key = root / "certs/server key.pem"
    cert.parent.mkdir()
    cert.write_text("placeholder for argument validation")
    key.write_text("placeholder for argument validation")
    config = root / "config/instances/gateway-wss.env"
    config.write_text(config.read_text().replace("CERT=certs/fullchain.pem", "CERT='certs/server chain.pem'")
                      .replace("KEY=certs/private-key.pem", f"KEY='{key}'"))
    subprocess.run([launcher, "gateway-wss"], cwd="/", env=tls_env, check=True)
    args = capture.read_text().splitlines()
    assert args[args.index("--cert") + 1] == str(cert)
    assert args[args.index("--key") + 1] == str(key)
    assert args[args.index("--transport") + 1] == "wss"
    assert args[args.index("--upstream") + 1] == "127.0.0.1:7102"

    # Run the generated gameServer configuration with the just-built binary and
    # verify the project-relative Lua script path is resolved correctly.
    import os
    import socket
    import struct
    import time
    env = os.environ.copy()
    env["MISTLANE_BIN"] = binary
    manager = [str(root / "ops/cluster.sh")]
    subprocess.run(manager + ["start", "gameServer"], cwd=root, env=env,
                   check=True, capture_output=True, text=True)
    try:
        deadline = time.monotonic() + 5
        while True:
            try:
                sock = socket.create_connection(("127.0.0.1", 7102), timeout=.2)
                break
            except OSError:
                status = subprocess.run(manager + ["status", "gameServer"], cwd=root, env=env,
                                        capture_output=True, text=True)
                if "gameServer stopped" in status.stdout or time.monotonic() >= deadline:
                    raise AssertionError("generated gameServer did not start")
                time.sleep(.05)
        with sock:
            payload = b"template-path-check"
            sock.sendall(struct.pack("!I", len(payload)) + payload)
            assert sock.recv(4) == struct.pack("!I", len(payload))
            assert sock.recv(len(payload)) == payload
    finally:
        subprocess.run(manager + ["stop", "gameServer"], cwd=root, env=env,
                       check=True, capture_output=True, text=True)

    sentinel = root / "keep.txt"
    sentinel.write_text("preserve")
    failed = subprocess.run([binary, "init", str(root)], capture_output=True, text=True)
    assert failed.returncode != 0
    assert sentinel.read_text() == "preserve"

help_result = subprocess.run([binary, "init", "--help"], check=True, capture_output=True, text=True)
assert "DIRECTORY" in help_result.stdout
print("PASS: initializer creates a complete non-overwriting multi-role starter")
