#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""End-to-end Lua Redis Streams and leaderboard check using local Redis."""
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def request(sock, payload):
    data = payload.encode()
    sock.sendall(struct.pack("!I", len(data)) + data)
    header = sock.recv(4)
    if len(header) != 4:
        raise RuntimeError("worker closed before response")
    length = struct.unpack("!I", header)[0]
    body = bytearray()
    while len(body) < length:
        chunk = sock.recv(length - len(body))
        if not chunk:
            raise RuntimeError("worker closed during response")
        body.extend(chunk)
    return body.decode()


def wait_until(sock, predicate, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = request(sock, "status")
        if predicate(result):
            return result
        time.sleep(0.05)
    raise AssertionError("timed out waiting for Lua Redis callback")


def main():
    binary = Path(sys.argv[1]).resolve()
    source = Path(sys.argv[2]).resolve()
    redis_port, worker_port = free_port(), free_port()
    lua = r'''
local state = { stream = "pending", board = "pending" }
return {
  message = function(payload)
    if payload == "setup" then
      mistlane.stream.create_group("it", "workers", function(err)
        if err then state.stream = "error:" .. err; return end
        mistlane.stream.create_group("it", "workers", function(group_err)
          if group_err then state.stream = "error:" .. group_err; return end
          mistlane.stream.publish("it", {kind = "integration", value = "ok"}, function(pub_err, id)
            if pub_err then state.stream = "error:" .. pub_err else state.published = id end
          end)
        end)
      end)
      return "started"
    elseif payload == "read" then
      mistlane.stream.read("it", "workers", "worker-a", 10, function(err, entries)
        if err then state.stream = "error:" .. err; return end
        for _, entry in ipairs(entries) do
          if entry.fields.kind == "integration" then
            state.stream = entry.fields.value
            state.pending_id = entry.id
          end
        end
      end)
      return "reading"
    elseif payload == "claim" then
      mistlane.stream.claim("it", "workers", "worker-b", 0, "0-0", 10, function(err, result)
        if err then state.stream = "error:" .. err; return end
        if #result.entries == 0 then state.stream = "claim-empty"; return end
        mistlane.stream.ack("it", "workers", result.entries[1].id, function(ack_err, count)
          if ack_err then state.stream = "error:" .. ack_err else state.acked = count end
        end)
      end)
      return "claiming"
    elseif payload == "rank" then
      mistlane.leaderboard.add("season", "player-a", 7, function(err, added)
        if err then state.board = "error:" .. err; return end
        mistlane.leaderboard.get("season", "player-a", function(get_err, current)
          if get_err then state.board = "error:" .. get_err; return end
          mistlane.leaderboard.top("season", 0, 10, function(top_err, rows)
            if top_err then state.board = "error:" .. top_err; return end
            state.board = added.score .. ":" .. added.rank .. ":" .. current.score .. ":" .. #rows
          end)
        end)
      end)
      return "ranking"
    elseif payload == "remove" then
      mistlane.leaderboard.remove("season", "player-a", function(err, count)
        state.removed = err and ("error:" .. err) or tostring(count)
      end)
      return "removing"
    elseif payload == "status" then
      return (state.stream or "") .. "|" .. (state.board or "") .. "|" ..
        (state.published or "") .. "|" .. tostring(state.acked or "") .. "|" .. (state.removed or "")
    end
    return payload
  end
}
'''
    redis = subprocess.Popen([
        "redis-server", "--bind", "127.0.0.1", "--port", str(redis_port),
        "--save", "", "--appendonly", "no", "--daemonize", "no"
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    worker = None
    try:
        with tempfile.TemporaryDirectory(prefix="mistlane-redis-") as directory:
            script = Path(directory) / "service.lua"
            script.write_text(lua)
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                try:
                    with socket.create_connection(("127.0.0.1", redis_port), timeout=0.2):
                        break
                except OSError:
                    time.sleep(0.05)
            else:
                raise RuntimeError("local Redis did not start")
            worker = subprocess.Popen([
                str(binary), "--role", "gameServer", "--listen", f"127.0.0.1:{worker_port}",
                "--script", str(script), "--redis", f"127.0.0.1:{redis_port}"
            ], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            deadline = time.monotonic() + 8
            client = None
            while time.monotonic() < deadline:
                if worker.poll() is not None:
                    raise RuntimeError("worker exited: " + (worker.stdout.read() if worker.stdout else ""))
                try:
                    client = socket.create_connection(("127.0.0.1", worker_port), timeout=0.5)
                    break
                except OSError:
                    time.sleep(0.05)
            if client is None:
                raise RuntimeError("worker did not become ready")
            with client:
                request(client, "setup")
                wait_until(client, lambda x: len(x.split("|")) > 2 and bool(x.split("|")[2]))
                request(client, "read")
                wait_until(client, lambda x: x.startswith("ok|"))
                request(client, "claim")
                wait_until(client, lambda x: len(x.split("|")) > 3 and x.split("|")[0] == "ok" and x.split("|")[3] == "1")
                request(client, "rank")
                wait_until(client, lambda x: x.split("|")[1] not in ("pending", ""))
                result = request(client, "status")
                if "7:1:7:1" not in result:
                    raise AssertionError("unexpected leaderboard state: " + result)
                request(client, "remove")
                result = wait_until(client, lambda x: x.endswith("|1"))
                if not result.endswith("|1"):
                    raise AssertionError("leaderboard removal failed: " + result)
    finally:
        if worker is not None:
            worker.terminate()
            try:
                worker.wait(timeout=3)
            except subprocess.TimeoutExpired:
                worker.kill()
        redis.terminate()
        try:
            redis.wait(timeout=3)
        except subprocess.TimeoutExpired:
            redis.kill()
    print("Redis Streams and leaderboard integration passed")


if __name__ == "__main__":
    main()
