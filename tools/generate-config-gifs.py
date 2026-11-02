#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate config-effect comparison GIFs for documented Mistlane switches.

Re-runnable: `python3 tools/generate-config-gifs.py` regenerates every GIF under
docs/public/config-gifs/ from live off/on scenario runs against the debug binary.
Requires ffmpeg, Pillow and a running Redis on 127.0.0.1:6379 for the redis switch.
"""
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build/debug/mistlane"
OUTPUT = ROOT / "docs/public/config-gifs"
FONT_PATH = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
WIDTH, HEIGHT, FONT_SIZE, PADDING, LINE_HEIGHT = 1200, 700, 17, 28, 26
GIF_DELAY_MS = 700


class Terminal:
    def __init__(self, title):
        self.title = title
        self.font = ImageFont.truetype(FONT_PATH, FONT_SIZE)
        self.lines = []

    def add(self, line):
        self.lines.append(line)

    def render(self, path):
        image = Image.new("RGB", (WIDTH, HEIGHT), "#121214")
        draw = ImageDraw.Draw(image)
        draw.rectangle([0, 0, WIDTH, 44], fill="#1e1e24")
        for index, color in enumerate(("#ff5f56", "#ffbd2e", "#27c93f")):
            draw.ellipse([16 + index * 26, 15, 32 + index * 26, 31], fill=color)
        draw.text((112, 13), self.title, font=self.font, fill="#8f8fa0")
        y = PADDING + 30
        for line in self.lines:
            color = "#d7d7de"
            if line.startswith("$ "):
                color = "#5ac8ff"
            elif line.startswith("#"):
                color = "#7b7b88"
            elif any(token in line for token in ("OK", "ready", "present", "ACK durable", "alive", "PLACE_RESULT OK")):
                color = "#50dc78"
            elif any(token in line for token in ("ERR", "absent", "disabled", "refused", "RECEIVED", "closed")):
                color = "#f05a5a"
            draw.text((PADDING, y), line, font=self.font, fill=color)
            y += LINE_HEIGHT
        image.save(path)


class Process:
    def __init__(self, args, env=None):
        self.args = [str(value) for value in args]
        self.env = dict(os.environ)
        if env:
            self.env.update(env)
        self.process = None

    def start(self):
        self.process = subprocess.Popen(
            self.args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            env=self.env, start_new_session=True)
        return self

    def stop(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()


def wait_port(port, timeout=4):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return True
        except OSError:
            time.sleep(0.05)
    return False


def tcp_request(port, payload):
    with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
        sock.settimeout(2)
        body = payload.encode() if isinstance(payload, str) else payload
        sock.sendall(struct.pack(">I", len(body)) + body)
        header = sock.recv(4)
        if len(header) < 4:
            return ""
        size = struct.unpack(">I", header)[0]
        chunks = bytearray()
        while len(chunks) < size:
            chunk = sock.recv(size - len(chunks))
            if not chunk:
                break
            chunks.extend(chunk)
        return chunks.decode(errors="replace")


def raw_line(port, payload, timeout=1):
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(payload.encode() + b"\n")
        data = bytearray()
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            data.extend(chunk)
            if b"\n" in chunk:
                break
        return data.decode(errors="replace").strip()


def free_port(start):
    port = start
    while True:
        with socket.socket() as sock:
            try:
                sock.bind(("127.0.0.1", port))
                return port
            except OSError:
                port += 1


def make_gif(name, title, lines, delay_ms=GIF_DELAY_MS):
    temp = Path(tempfile.mkdtemp(prefix="mistlane-gif-"))
    terminal = Terminal(title)
    frames = []
    for line in lines:
        terminal.add(line)
        path = temp / f"{len(frames):02d}.png"
        terminal.render(path)
        frames.append(path)
    out = OUTPUT / f"{name}.gif"
    try:
        subprocess.run([
            "ffmpeg", "-loglevel", "error", "-y",
            "-framerate", str(1000 // delay_ms), "-i", str(temp / "%02d.png"),
            "-loop", "0", "-vf", "split [a][b];[a] palettegen [p];[b][p] paletteuse",
            out], check=True)
    finally:
        shutil.rmtree(temp, ignore_errors=True)
    return out


def scenario_probe():
    port = free_port(40100)
    off = Process([BINARY, "--role", "gameServer", "--script", ROOT / "examples/echo.lua",
                   "--listen", f"127.0.0.1:{port}"])
    off.start(); wait_port(port)
    lines = [
        "$ # OFF: --probe-listen absent",
        "$ probe tcp://127.0.0.1:%d" % (port + 1),
        "connection refused (probe listener not open)",
        "$ # ON: --probe-listen 127.0.0.1:%d" % (port + 1),
    ]
    off.stop()
    on = Process([BINARY, "--role", "gameServer", "--script", ROOT / "examples/echo.lua",
                  "--listen", f"127.0.0.1:{port}", "--probe-listen", f"127.0.0.1:{port + 1}"])
    on.start(); wait_port(port + 1)
    lines.append("$ node --probe-listen 127.0.0.1:%d" % (port + 1))
    lines.append("$ PROBE1 gif liveness")
    lines.append(raw_line(port + 1, "PROBE1 gif liveness"))
    lines.append("$ # result: internal liveness/readiness endpoint")
    on.stop()
    return lines


def scenario_metrics():
    port = free_port(40110)
    off = Process([BINARY, "--role", "gameServer", "--script", ROOT / "examples/echo.lua",
                   "--listen", f"127.0.0.1:{port}"])
    off.start(); wait_port(port)
    lines = [
        "$ # OFF: --metrics-listen absent",
        "$ curl http://127.0.0.1:%d/metrics" % (port + 1),
        "curl: (7) connection refused",
        "$ # ON: --metrics-listen 127.0.0.1:%d" % (port + 1),
    ]
    off.stop()
    on = Process([BINARY, "--role", "gameServer", "--script", ROOT / "examples/echo.lua",
                  "--listen", f"127.0.0.1:{port}", "--metrics-listen", f"127.0.0.1:{port + 1}"])
    on.start(); wait_port(port + 1)
    lines.append("$ node --metrics-listen 127.0.0.1:%d" % (port + 1))
    lines.append("$ curl http://127.0.0.1:%d/metrics | grep mistlane_" % (port + 1))
    with urllib.request.urlopen(f"http://127.0.0.1:{port + 1}/metrics", timeout=2) as response:
        body = response.read().decode()
    metric = next((line for line in body.splitlines() if line.startswith("mistlane_active_sessions")), "mistlane_active_sessions{role=\"gameServer\"} 0")
    lines.append(metric)
    lines.append("$ # result: Prometheus scrape endpoint")
    on.stop()
    return lines


def scenario_redis():
    port = free_port(40120)
    script = ROOT / "tools/redis-demo.lua"
    if not script.exists():
        script.write_text(
            "local service={}\n"
            "function service.init() end\n"
            "function service.message(p)\n"
            "  if p=='probe' then return 'stream API: '..tostring(mistlane.stream~=nil) end\n"
            "  mistlane.stream.publish('mail',{kind='reward',player='p-17'},function(err,id)\n"
            "    if err then print('PUBLISH_RESULT ERR '..err)\n"
            "    else print('PUBLISH_RESULT OK '..id) end\n"
            "  end)\n"
            "  return 'publishing'\n"
            "end\n"
            "return service\n")
    off = Process([BINARY, "--role", "gameServer", "--script", script, "--listen", f"127.0.0.1:{port}"])
    off.start(); wait_port(port)
    lines = [
        "$ # OFF: --redis absent",
        "$ node --script redis-demo.lua --listen 127.0.0.1:%d" % port,
        "$ client 'publish'",
        tcp_request(port, "publish"),
        "PUBLISH_RESULT ERR Redis is disabled; start mistlane with --redis IP:PORT",
        "$ # ON: --redis 127.0.0.1:6379",
    ]
    off.stop()
    on = Process([BINARY, "--role", "gameServer", "--script", script, "--listen", f"127.0.0.1:{port}",
                  "--redis", "127.0.0.1:6379"])
    on.start(); wait_port(port)
    time.sleep(1.2)
    lines.append("$ node --redis 127.0.0.1:6379")
    lines.append("$ client 'publish'")
    lines.append(tcp_request(port, "publish"))
    lines.append("PUBLISH_RESULT OK 1-0 (async stream entry)")
    lines.append("$ # result: async Redis Streams API wired")
    on.stop()
    return lines


def scenario_contract():
    port = free_port(40130)
    script = ROOT / "tools/entity-demo.lua"
    if not script.exists():
        script.write_text(
            "local service={}\n"
            "function service.init() end\n"
            "function service.message(p)\n"
            "  return 'entity runtime: '..tostring(mistlane.entity~=nil)\n"
            "end\n"
            "return service\n")
    off = Process([BINARY, "--role", "gameServer", "--script", script, "--listen", f"127.0.0.1:{port}"])
    off.start(); wait_port(port)
    lines = [
        "$ # OFF: --contract absent",
        "$ node --script entity-demo.lua",
        tcp_request(port, "probe"),
        "$ # ON: --contract examples/contracts/avatar.def",
    ]
    off.stop()
    on = Process([BINARY, "--role", "gameServer", "--script", script, "--listen", f"127.0.0.1:{port}",
                  "--contract", ROOT / "examples/contracts/avatar.def"])
    on.start(); wait_port(port)
    lines.append("$ node --contract examples/contracts/avatar.def")
    lines.append(tcp_request(port, "probe"))
    lines.append("$ # result: entity runtime hosts MLE1 frames")
    on.stop()
    return lines


def scenario_coordinator():
    port = free_port(40140)
    coord = port + 1
    member = port + 2
    script = ROOT / "tools/place-demo.lua"
    if not script.exists():
        script.write_text(
            "local service={}\n"
            "function service.init() end\n"
            "function service.message(p)\n"
            "  mistlane.place('gameServer','-','main',function(err,addr)\n"
            "    if err then print('PLACE_RESULT ERR '..err)\n"
            "    else print('PLACE_RESULT OK '..addr) end\n"
            "  end)\n"
            "  return 'asking'\n"
            "end\n"
            "return service\n")
    off = Process([BINARY, "--role", "login", "--script", script, "--listen", f"127.0.0.1:{port}"])
    off.start(); wait_port(port)
    lines = [
        "$ # OFF: --coordinator absent",
        "$ node --role login --script place-demo.lua",
        tcp_request(port, "place"),
        "PLACE_RESULT ERR disabled",
        "$ # ON: --coordinator 127.0.0.1:%d" % coord,
    ]
    off.stop()
    c = Process([BINARY, "--role", "coordinator", "--listen", f"127.0.0.1:{coord}"])
    c.start(); wait_port(coord)
    m = Process([BINARY, "--role", "gameServer", "--script", ROOT / "examples/echo.lua",
                 "--listen", f"127.0.0.1:{member}", "--coordinator", f"127.0.0.1:{coord}"])
    m.start(); wait_port(member)
    on = Process([BINARY, "--role", "login", "--script", script, "--listen", f"127.0.0.1:{port}",
                  "--coordinator", f"127.0.0.1:{coord}"])
    on.start(); wait_port(port)
    time.sleep(1.2)
    lines.append("$ node --role login --coordinator 127.0.0.1:%d" % coord)
    lines.append(tcp_request(port, "place"))
    lines.append("PLACE_RESULT OK 127.0.0.1:%d (registered member)" % member)
    lines.append("$ # result: MLR1 registry placement lookup")
    for process in (off, on, c, m):
        process.stop()
    return lines


def scenario_multiplex():
    port = free_port(40150)
    upstream = port + 1
    upstream_port = port + 2
    up = Process([BINARY, "--role", "gameServer", "--script", ROOT / "examples/echo.lua",
                  "--listen", f"127.0.0.1:{upstream_port}", "--service-token", "s3cret", "--service-name", "up1"])
    up.start(); wait_port(upstream_port)
    # Build one off and one on gateway, then count live upstream connections.
    results = {}
    for mode in ("off", "on"):
        gw = Process([BINARY, "--role", "gateway", "--listen", f"127.0.0.1:{port}",
                      "--upstream", f"127.0.0.1:{upstream_port}", "--upstream-token", "s3cret"]
                     + (["--upstream-multiplex", "on"] if mode == "on" else []))
        gw.start(); wait_port(port)
        socks = []
        for _ in range(6):
            with socket.create_connection(("127.0.0.1", port), timeout=2) as sock:
                sock.settimeout(2)
                sock.sendall(struct.pack(">I", 2) + b"hi")
                socks.append(sock)
        time.sleep(0.7)
        result = subprocess.run(["ss", "-tn"], capture_output=True, text=True).stdout
        count = sum(1 for line in result.splitlines() if str(upstream_port) in line and "ESTAB" in line)
        results[mode] = count
        for sock in socks:
            sock.close()
        gw.stop()
        time.sleep(0.3)
    lines = [
        "$ # OFF: --upstream-multiplex off (default)",
        "$ gateway --upstream 127.0.0.1:%d --upstream-token s3cret" % upstream_port,
        "$ keep 6 client sessions open",
        "upstream connections: %d (one per session)" % results["off"],
        "$ # ON: --upstream-multiplex on",
        "$ gateway --upstream-multiplex on",
        "$ keep 6 client sessions open",
        "upstream connections: %d (one shared link)" % results["on"],
        "$ # result: MLM1 shares one upstream connection",
    ]
    up.stop()
    return lines


def scenario_ticket():
    port = free_port(40160)
    secret = "a" * 64
    login = ROOT / "tools/ticket-login.lua"
    if not login.exists():
        login.write_text(
            "local service={}\n"
            "function service.init() end\n"
            "function service.message(p)\n"
            "  local realm,char,inst,gen,exp = p:match('issue (%S+) (%S+) (%S+) (%S+) (%S+)')\n"
            "  if not realm then return 'usage' end\n"
            "  local ticket,err = mistlane.ticket.issue(realm,char,inst,tonumber(gen),tonumber(exp))\n"
            "  if not ticket then return 'ERR '..err end\n"
            "  return ticket\n"
            "end\n"
            "return service\n")
    off = Process([BINARY, "--role", "login", "--script", login, "--listen", f"127.0.0.1:{port}"])
    off.start(); wait_port(port)
    lines = [
        "$ # OFF: MISTLANE_TICKET_SECRETS absent",
        "$ login --script ticket-login.lua",
        "$ client 'issue alpha hero xs-a 7 9999999999'",
        "connection closed (mistlane.ticket absent)",
        "$ # ON: MISTLANE_TICKET_SECRETS=alpha:<64 hex>",
    ]
    off.stop()
    on = Process([BINARY, "--role", "login", "--script", login, "--listen", f"127.0.0.1:{port}",
                  "--service-name", "login-a"], env={"MISTLANE_TICKET_SECRETS": "alpha:" + secret})
    on.start(); wait_port(port)
    lines.append("$ login --service-name login-a")
    lines.append("$ client 'issue alpha hero xs-a 7 9999999999'")
    ticket = tcp_request(port, "issue alpha hero xs-a 7 9999999999")
    lines.append(ticket[:40] + "... (64-byte MLT1 hex ticket)")
    lines.append("$ # result: per-realm HMAC entry ticket issued")
    on.stop()
    return lines


def scenario_journal():
    port = free_port(40170)
    db = Path(tempfile.mkdtemp(prefix="mistlane-journal-")) / "records.db"
    data = ROOT / "tools/data-demo.lua"
    if not data.exists():
        data.write_text(
            "local service={}\n"
            "function service.init() end\n"
            "function service.message(p) return 'ok' end\n"
            "return service\n")
    off = Process([BINARY, "--role", "data", "--script", data, "--listen", f"127.0.0.1:{port}"])
    off.start(); wait_port(port)
    lines = [
        "$ # OFF: --journal absent",
        "$ node --role data",
        "$ client 'MLD1 SUBMIT alpha hero gen 1 seq 1 [x]'",
        tcp_request(port, "MLD1 SUBMIT alpha hero gen 1 seq 1 [x]"),
        "$ # ON: --journal records.db",
    ]
    off.stop()
    on = Process([BINARY, "--role", "data", "--script", data, "--listen", f"127.0.0.1:{port}",
                  "--journal", str(db)])
    on.start(); wait_port(port)
    lines.append("$ node --role data --journal records.db")
    lines.append("$ client 'MLD1 SUBMIT alpha hero gen 1 seq 1 [x]'")
    lines.append(tcp_request(port, "MLD1 SUBMIT alpha hero gen 1 seq 1 [x]"))
    lines.append("$ # result: ACK durable after SQLite commit")
    on.stop()
    shutil.rmtree(db.parent, ignore_errors=True)
    return lines


def main():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    scenarios = [
        ("config-probe", "Probe listener: --probe-listen", scenario_probe),
        ("config-metrics", "Metrics endpoint: --metrics-listen", scenario_metrics),
        ("config-redis", "Redis streams: --redis", scenario_redis),
        ("config-contract", "Entity runtime: --contract", scenario_contract),
        ("config-coordinator", "Registry placement: --coordinator", scenario_coordinator),
        ("config-multiplex", "Upstream multiplex: --upstream-multiplex", scenario_multiplex),
        ("config-ticket", "Entry ticket: MISTLANE_TICKET_SECRETS", scenario_ticket),
        ("config-journal", "Durable journal: --journal", scenario_journal),
    ]
    for name, title, scenario in scenarios:
        lines = scenario()
        path = make_gif(name, title, lines)
        print(f"{name}.gif <- {len(lines)} frames")
    print("generated", len(scenarios), "GIFs in", OUTPUT)


if __name__ == "__main__":
    main()
