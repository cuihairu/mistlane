#!/usr/bin/env python3
"""Integration checks against real node processes, using only Python stdlib."""
import base64
import hashlib
import ssl
import concurrent.futures
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
from client import receive, request

binary = str(Path(sys.argv[1]).resolve())
root = Path(sys.argv[2]).resolve()
processes = []
ports = set()


def port():
    while True:
        with socket.socket() as s:
            s.bind(('127.0.0.1', 0))
            value = s.getsockname()[1]
        if value not in ports:
            ports.add(value)
            return value


def start(role, listen, *args):
    log = tempfile.TemporaryFile(mode='w+t')
    p = subprocess.Popen([binary, '--role', role, '--listen', f'127.0.0.1:{listen}', *args],
                         stdout=log, stderr=log)
    processes.append((p, log))
    # Check logs instead of consuming one session/round-robin slot via a probe.
    until = time.monotonic() + 5
    while time.monotonic() < until:
        log.seek(0)
        if 'ready ' in log.read():
            return p, log
        if p.poll() is not None:
            raise AssertionError('startup failed')
        time.sleep(.02)
    raise AssertionError('startup timeout')


def connect(p):
    return socket.create_connection(('127.0.0.1', p), timeout=5)


def wait_log(log, text):
    until = time.monotonic() + 5
    while time.monotonic() < until:
        log.seek(0)
        if text in log.read():
            return
        time.sleep(.02)
    raise AssertionError(f'log missing: {text}')


def websocket_connect(port, context=None):
    sock = connect(port)
    if context:
        sock = context.wrap_socket(sock, server_hostname='localhost')
    key = base64.b64encode(os.urandom(16)).decode()
    sock.sendall((f'GET / HTTP/1.1\r\nHost: localhost:{port}\r\n'
                  f'Upgrade: websocket\r\nConnection: Upgrade\r\n'
                  f'Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n').encode())
    response = bytearray()
    while not response.endswith(b'\r\n\r\n'):
        response.extend(receive(sock, 1))
    assert response.startswith(b'HTTP/1.1 101 '), response
    accept = base64.b64encode(hashlib.sha1((key+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
    assert accept.lower() in response.lower()
    return sock


def ws_send(sock, data, opcode=2, final=True):
    mask = os.urandom(4)
    length = len(data)
    header = bytes([(128 if final else 0) | opcode])
    if length < 126:
        header += bytes([128 | length])
    elif length <= 65535:
        header += bytes([128 | 126])+struct.pack('!H', length)
    else:
        header += bytes([128 | 127])+struct.pack('!Q', length)
    sock.sendall(header+mask+bytes(b ^ mask[i % 4] for i, b in enumerate(data)))


def ws_read(sock):
    flags, length = receive(sock, 2)
    assert not length & 128  # Server frames must not be masked.
    length &= 127
    if length == 126:
        length, = struct.unpack('!H', receive(sock, 2))
    elif length == 127:
        length, = struct.unpack('!Q', receive(sock, 8))
    return flags, receive(sock, length)


def ws_message(sock):
    result = bytearray()
    while True:
        flags, data = ws_read(sock)
        assert flags & 15 in (0, 2)
        result.extend(data)
        if flags & 128:
            return bytes(result)


try:
    with tempfile.TemporaryDirectory() as temp:
        script = Path(temp) / 'service.lua'
        script.write_text('return {message = function(p) return p end}')
        worker, router, gateway = port(), port(), port()
        wp, wl = start('worker', worker, '--script', str(script))
        start('router', router, '--upstream', f'127.0.0.1:{worker}')
        start('gateway', gateway, '--upstream', f'127.0.0.1:{router}')
        with connect(gateway) as sock:
            for data in (b'', b'hello', b'\x00\xffbinary', '中文'.encode(), b'x' * (1024*1024)):
                assert request(sock, data) == data
            # Fragmented header/body and two coalesced frames.
            frame = struct.pack('!I', 5) + b'abcde'
            for byte in frame:
                sock.sendall(bytes([byte]))
            assert receive(sock, 9) == frame
            sock.sendall(frame + frame)
            assert receive(sock, 18) == frame + frame
            # Hundreds of callbacks on the same VM/connection catch stack regressions.
            for _ in range(100):
                assert request(sock, b'stack') == b'stack'
        def parallel(_):
            with connect(gateway) as s:
                assert request(s, b'parallel') == b'parallel'
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            list(pool.map(parallel, range(24)))
        with connect(gateway) as s:
            s.sendall(struct.pack('!I', 1024*1024+1))
            assert s.recv(1) == b''
        print('PASS: three-process roundtrip, binary/max/empty frames, fragmentation, concurrent clients')
        # WS framing/handshake/control handling is implemented by Boost.Beast.
        ws_port = port()
        start('gateway', ws_port, '--transport', 'ws', '--upstream', f'127.0.0.1:{router}')
        with websocket_connect(ws_port) as sock:
            for data in (b'', b'ws', b'\x00\xff', b'w'*65536):
                ws_send(sock, data)
                assert ws_message(sock) == data
            ws_send(sock, b'frag', final=False)
            ws_send(sock, b'ment', opcode=0)
            assert ws_message(sock) == b'fragment'
            ws_send(sock, b'ping', opcode=9)
            flags, data = ws_read(sock)
            assert flags & 15 == 10 and data == b'ping'
        cert, key = Path(temp)/'cert.pem', Path(temp)/'key.pem'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                        '-keyout', str(key), '-out', str(cert), '-days', '1',
                        '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost'],
                       check=True, capture_output=True)
        wss_port = port()
        start('gateway', wss_port, '--transport', 'wss', '--cert', str(cert), '--key', str(key),
              '--upstream', f'127.0.0.1:{router}')
        context = ssl.create_default_context(cafile=str(cert))
        with websocket_connect(wss_port, context) as sock:
            ws_send(sock, b'tls-verified')
            assert ws_message(sock) == b'tls-verified'
        print('PASS: Beast WS/WSS handshake, masking, fragmentation, ping/pong, trusted TLS echo')

        script.write_text('return {message = function(p) return "v2:" .. p end}')
        wp.send_signal(signal.SIGHUP)
        wait_log(wl, 'script reloaded')
        with connect(gateway) as s:
            assert request(s, b'hi') == b'v2:hi'
        script.write_text('this is not valid lua !!!')
        wp.send_signal(signal.SIGHUP)
        wait_log(wl, 'reload rejected')
        with connect(gateway) as s:
            assert request(s, b'hi') == b'v2:hi'
        script.write_text('return {}')
        wp.send_signal(signal.SIGHUP)
        wait_log(wl, 'service.message is required')
        with connect(gateway) as s:
            assert request(s, b'hi') == b'v2:hi'
        print('PASS: successful reload and rejected reload preserves old VM')
        # Callback failures and runaway Lua loops must not kill or hang the node.
        bad = Path(temp) / 'bad.lua'
        bad.write_text('return {message=function(p) if p=="error" then error("bad") end '
                       'if p=="loop" then while true do end end return p end}')
        bad_port = port()
        start('worker', bad_port, '--script', str(bad))
        for value in (b'error', b'loop'):
            with connect(bad_port) as s:
                s.sendall(struct.pack('!I', len(value)) + value)
                assert s.recv(1) == b''
        with connect(bad_port) as s:
            assert request(s, b'healthy') == b'healthy'
        # Extracted MinHeap is exercised by the actual Lua version we link.
        heap = Path(temp) / 'heap.lua'
        heap.write_text('local H = dofile(' + repr(str(root/'lua/mistlane/min_heap.lua')) + ')\n'
                        'assert(MinHeap == nil)\n'
                        'local h=H:new(3,function(a,b) return a<b end)\n'
                        'h:push(3);h:push(1);h:push(2)\n'
                        'assert(h:pop()==1 and h:pop()==2 and h:pop()==3)\n'
                        'return {message=function(p) return p end}')
        hp = port()
        start('worker', hp, '--script', str(heap))
        with connect(hp) as s:
            assert request(s, b'heap') == b'heap'
        # Session affinity: requests on one session stay at the selected worker.
        a = Path(temp)/'a.lua'
        b = Path(temp)/'b.lua'
        a.write_text('return {message=function(p) return "a" end}')
        b.write_text('return {message=function(p) return "b" end}')
        ap, bp, rp = port(), port(), port()
        start('worker', ap, '--script', str(a))
        start('worker', bp, '--script', str(b))
        start('router', rp, '--upstream', f'127.0.0.1:{ap}', '--upstream', f'127.0.0.1:{bp}')
        with connect(rp) as s:
            assert request(s, b'x') == b'a'
            assert request(s, b'x') == b'a'
        with connect(rp) as s:
            assert request(s, b'x') == b'b'
        # Upstream connection failure closes the client, process remains healthy.
        dead, gp = port(), port()
        start('gateway', gp, '--upstream', f'127.0.0.1:{dead}')
        with connect(gp) as s:
            s.sendall(struct.pack('!I', 1)+b'x')
            assert s.recv(1) == b''
        # CLI validation must fail before starting a process.
        for args in (['--role', 'invalid'], ['--listen', '127.0.0.1:65536'],
                     ['--role', 'worker'], ['--role', 'gateway'], ['--max-connections', '0']):
            assert subprocess.run([binary, *args], capture_output=True, timeout=3).returncode != 0
        limited = port()
        start('worker', limited, '--script', str(root/'examples/echo.lua'), '--max-connections', '1')
        with connect(limited) as first:
            assert request(first, b'first') == b'first'
            with connect(limited) as extra:
                assert extra.recv(1) == b''
            assert request(first, b'first') == b'first'
        with connect(bad_port) as burst:
            frame = struct.pack('!I', 1) + b'x'
            burst.sendall(frame * 1000)
            count = 0
            while True:
                try:
                    size, = struct.unpack('!I', receive(burst, 4))
                    receive(burst, size)
                    count += 1
                except (ConnectionError, ConnectionResetError):
                    break
            assert 0 < count < 1000
        print('PASS: Lua errors/budget, extracted heap, session affinity, upstream failure, CLI validation')
    for p, _ in processes:
        p.send_signal(signal.SIGTERM)
    for p, _ in processes:
        assert p.wait(timeout=5) == 0
    print('PASS: SIGTERM closes active listeners and drains cancellation handlers')
except BaseException:
    for p, log in processes:
        log.seek(0)
        print(f'node pid={p.pid}:\n{log.read()}', file=sys.stderr)
    raise
finally:
    for p, log in processes:
        if p.poll() is None:
            p.kill()
        p.wait()
        log.close()
