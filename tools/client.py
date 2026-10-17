#!/usr/bin/env python3
"""A dependency-free client for the u32-big-endian framed protocol."""
import argparse
import socket
import struct


def receive(sock, size):
    result = bytearray()
    while len(result) < size:
        part = sock.recv(size - len(result))
        if not part:
            raise ConnectionError("peer closed")
        result.extend(part)
    return bytes(result)


def request(sock, data):
    sock.sendall(struct.pack('!I', len(data)) + data)
    size, = struct.unpack('!I', receive(sock, 4))
    if size > 1024 * 1024:
        raise ValueError('oversize response')
    return receive(sock, size)


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('message', nargs='?', default='hello mistlane')
    p.add_argument('--host', default='127.0.0.1')
    p.add_argument('--port', type=int, default=7100)
    args = p.parse_args()
    with socket.create_connection((args.host, args.port), timeout=5) as sock:
        print(request(sock, args.message.encode()).decode())
