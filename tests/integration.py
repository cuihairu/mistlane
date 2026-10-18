#!/usr/bin/env python3
"""Integration checks against real node processes, using only Python stdlib."""
import base64
import http.client
import hashlib
import ssl
import concurrent.futures
import os
import re
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
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
        worker, router, gateway, metrics = port(), port(), port(), port()
        wp, wl = start('worker', worker, '--script', str(script), '--metrics-listen', f'127.0.0.1:{metrics}')
        start('router', router, '--upstream', f'127.0.0.1:{worker}')
        start('gateway', gateway, '--upstream', f'127.0.0.1:{router}')
        client = http.client.HTTPConnection('127.0.0.1', metrics, timeout=5)
        try:
            client.request('GET', '/metrics')
            response = client.getresponse()
            metrics_body = response.read().decode()
            assert response.status == 200
            assert 'mistlane_ready{role="worker"} 1' in metrics_body
        finally:
            client.close()
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
            client = http.client.HTTPConnection('127.0.0.1', metrics, timeout=5)
            try:
                client.request('GET', '/metrics')
                metrics_body = client.getresponse().read().decode()
                active = re.search(r'mistlane_active_sessions\{role="worker"\} ([\d.]+)', metrics_body)
                assert active and float(active.group(1)) >= 1
                callback_count = re.search(r'mistlane_lua_callback_seconds_count\{role="worker"\} (\d+)', metrics_body)
                assert callback_count and int(callback_count.group(1)) > 0
            finally:
                client.close()
        def parallel(_):
            with connect(gateway) as s:
                assert request(s, b'parallel') == b'parallel'
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            list(pool.map(parallel, range(24)))
        with connect(gateway) as s:
            s.sendall(struct.pack('!I', 1024*1024+1))
            assert s.recv(1) == b''
        print('PASS: three-process roundtrip, binary/max/empty frames, fragmentation, concurrent clients')
        # WS framing/handshake/control handling is implemented by Drogon.
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
        print('PASS: Drogon WS/WSS handshake, masking, fragmentation, ping/pong, trusted TLS echo')

        # HTTP adapters and libcurl execute real requests, including TLS verification and size limits.
        probe = str(Path(binary).with_name('mistlane-http-probe'))
        for transport in ('http', 'https'):
            hp = port()
            options = ['--transport', transport, '--upstream', f'127.0.0.1:{router}']
            if transport == 'https': options += ['--cert', str(cert), '--key', str(key)]
            start('gateway', hp, *options)
            conn = (http.client.HTTPSConnection('localhost', hp, context=context, timeout=5)
                    if transport == 'https' else http.client.HTTPConnection('localhost', hp, timeout=5))
            for payload in (b'', b'http', b'\x00\xff', b'x'*65536):
                conn.request('POST', '/message', payload)
                response = conn.getresponse()
                assert response.status == 200 and response.read() == payload
            conn.request('GET', '/missing')
            assert conn.getresponse().status == 404
            conn.close()
            url = f'{transport}://localhost:{hp}/message'
            args = [probe, url, 'curl', str(cert) if transport == 'https' else '']
            result = subprocess.run(args, capture_output=True, timeout=5)
            assert result.returncode == 0 and result.stdout == b'200\ncurl', result.stderr
            assert subprocess.run(args + ['2'], capture_output=True, timeout=5).returncode != 0
            if transport == 'https':
                assert subprocess.run([probe, url, 'bad-trust'], capture_output=True, timeout=5).returncode != 0
        assert subprocess.run([probe, 'file:///etc/hosts'], capture_output=True, timeout=5).returncode != 0
        print('PASS: Drogon HTTP/HTTPS, binary requests, curl TLS trust/protocol/response limits')

        # Lua HTTP services: full request/response metadata, middleware, errors, and VM lifecycle.
        web_script = Path(temp)/'http.lua'
        web_script.write_text("""
local M = {}
function M.init(c) assert(c.role=='worker' and c.transport=='http'); print('HOOK init v1') end
function M.start() print('HOOK start v1') end
function M.before_request(r)
  r.verified = true
  if r.path=='/private' then return {status=401,body='denied'} end
end
function M.http(r)
  assert(r.verified)
  if r.path=='/echo' then
    assert(r.method=='POST' and r.query=='x=1' and r.headers['x-test']=='yes')
    return {status=201,body=r.body}
  end
  if r.path=='/error' or r.path=='/fatal' then error('private detail') end
  if r.path=='/badheader' then return {headers={['bad header']='x'}} end
  return {status=404,body='missing'}
end
function M.after_request(r,res)
  res.headers = {['x-hook']='after'}
  if r.path=='/replace' then return {status=202,body='replaced'} end
end
function M.on_error(r,e)
  if r.path=='/fatal' then error('error hook failed') end
  return {status=502,body='handled'}
end
function M.tick() end
function M.stop() print('HOOK stop v1') end
function M.shutdown() print('HOOK shutdown v1') end
return M
""")
        lua_http_port = port()
        web_process, web_log = start('worker', lua_http_port, '--transport', 'http', '--script', str(web_script))
        def lua_http(path, payload=None, headers=None):
            conn = http.client.HTTPConnection('127.0.0.1', lua_http_port, timeout=5)
            conn.request('POST' if payload is not None else 'GET', path, body=payload, headers=headers or {})
            result = conn.getresponse()
            data = (result.status, result.read(), result.getheader('x-hook'))
            conn.close()
            return data
        assert lua_http('/echo?x=1', b'\x00\xff', {'x-test':'yes'}) == (201, b'\x00\xff', 'after')
        assert lua_http('/private') == (401, b'denied', 'after')
        assert lua_http('/replace')[:2] == (202, b'replaced')
        for path in ('/error', '/badheader'): assert lua_http(path)[:2] == (502, b'handled')
        assert lua_http('/fatal')[:2] == (500, b'Internal Server Error')
        assert lua_http('/missing')[:2] == (404, b'missing')
        web_script.write_text("return {http=function(r) return {body='bad'} end,start=function() error('start rejected') end}")
        web_process.send_signal(signal.SIGHUP)
        wait_log(web_log, 'reload rejected')
        assert lua_http('/missing')[:2] == (404, b'missing')
        web_script.write_text("return {http=function(r) return {body='v2'} end,"
                              "init=function() print('HOOK init v2') end,"
                              "start=function() print('HOOK start v2') end,"
                              "stop=function() print('HOOK stop v2') end,"
                              "shutdown=function() print('HOOK shutdown v2') end}")
        web_process.send_signal(signal.SIGHUP)
        wait_log(web_log, 'script reloaded')
        assert lua_http('/')[:2] == (200, b'v2')
        web_process.send_signal(signal.SIGTERM)
        assert web_process.wait(timeout=5) == 0
        web_log.seek(0)
        hooks = [line for line in web_log.read().splitlines() if line.startswith('HOOK ')]
        assert hooks == ['HOOK init v1','HOOK start v1','HOOK init v2','HOOK start v2',
                         'HOOK stop v1','HOOK shutdown v1','HOOK stop v2','HOOK shutdown v2'], hooks
        print('PASS: Lua HTTP metadata, middleware, response validation, errors, reload rollback and lifecycle order')

        # Same room across realms must keep shard affinity, with hard admission and released leases.
        cross_script = Path(temp)/'cross.lua'
        cross_script.write_text('return {message=function(p,c) return c.realm..":"..c.group.."/"..c.room..":"..p end}')
        s0, s1, cp, cp2 = port(), port(), port(), port()
        shard0, _ = start('shard', s0, '--script', str(cross_script), '--max-room-sessions', '2')
        start('shard', s1, '--script', str(cross_script), '--max-room-sessions', '2')
        upstreams = ['--upstream', f'127.0.0.1:{s0}', '--upstream', f'127.0.0.1:{s1}']
        start('cross', cp, *upstreams, '--max-room-sessions', '2')
        start('cross', cp2, *upstreams, '--max-room-sessions', '2')
        def shard_index(room):
            h = 14695981039346656037
            for ch in ('g/'+room).encode(): h = ((h ^ ch)*1099511628211) & ((1<<64)-1)
            return h % 2
        room0 = next('r'+str(i) for i in range(100) if shard_index('r'+str(i)) == 0)
        room1 = next('r'+str(i) for i in range(100) if shard_index('r'+str(i)) == 1)
        def join(sock, realm, room, epoch=1):
            return request(sock, f'MLC1 {epoch} {realm} g {room}'.encode())
        with connect(cp) as first, connect(cp) as second:
            assert join(first, 'a', room0) == b'MLC1 OK 1'
            assert join(second, 'b', room0) == b'MLC1 OK 1'
            assert request(first, b'x') == f'a:g/{room0}:x'.encode()
            assert request(second, b'x') == f'b:g/{room0}:x'.encode()
            for ingress in (cp, cp2):
                with connect(ingress) as third:
                    assert join(third, 'c', room0) == b'MLC1 ERR ROOM_FULL'
                    assert third.recv(1) == b''
            with connect(cp) as other:
                assert join(other, 'c', room1) == b'MLC1 OK 1'
                assert request(other, b'x') == f'c:g/{room1}:x'.encode()
        # Lease release is asynchronous across both processes; wait for convergence, with a deadline.
        until = time.monotonic()+3
        while True:
            with connect(cp) as released:
                result = join(released, 'd', room0)
                if result == b'MLC1 OK 1': break
            assert time.monotonic() < until
            time.sleep(.02)
        for payload, expected in [(b'MLC1 2 a g r', b'EPOCH'), (b'invalid', b'INVALID')]:
            with connect(cp) as invalid:
                assert request(invalid, payload) == b'MLC1 ERR '+expected
        # A failed shard does not reroute its room; the other shard remains usable.
        shard0.send_signal(signal.SIGTERM)
        assert shard0.wait(timeout=5) == 0
        with connect(cp) as dead_room:
            dead_room.sendall(struct.pack('!I', len(f'MLC1 1 a g {room0}')) + f'MLC1 1 a g {room0}'.encode())
            assert dead_room.recv(1) == b''
        with connect(cp) as healthy_room:
            assert join(healthy_room, 'a', room1) == b'MLC1 OK 1'
            assert request(healthy_room, b'ok') == f'a:g/{room1}:ok'.encode()
        print('PASS: native cross/shard context, affinity, final admission, lease release, epoch and failure isolation')

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
        if p.poll() is None: p.send_signal(signal.SIGTERM)
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
