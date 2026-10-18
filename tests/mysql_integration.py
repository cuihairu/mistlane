#!/usr/bin/env python3
"""Run the mature MySQL pool against a disposable local MariaDB, no existing DB touched."""
import os
from pathlib import Path
import secrets
import shutil
import socket
import subprocess
import sys
import tempfile
import time

binary = str(Path(sys.argv[1]).resolve())
required = ['mariadb-install-db', 'mysqld', 'mysql', 'openssl']
for tool in required:
    if not shutil.which(tool):
        raise SystemExit(f'{tool} required for this optional integration check')
with tempfile.TemporaryDirectory(prefix='mistlane-db-') as temp:
    root = Path(temp)
    data, sock, cert, key = root/'data', root/'db.sock', root/'cert.pem', root/'key.pem'
    subprocess.run(['mariadb-install-db', '--no-defaults', f'--datadir={data}',
                    '--auth-root-authentication-method=normal', '--skip-test-db'],
                   check=True, capture_output=True)
    subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                    '-keyout', str(key), '-out', str(cert), '-days', '1',
                    '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost'],
                   check=True, capture_output=True)
    with socket.socket() as reserve:
        reserve.bind(('127.0.0.1', 0))
        port = reserve.getsockname()[1]
    log = (root/'server.log').open('w+')
    server = subprocess.Popen(['mysqld', '--no-defaults', f'--datadir={data}', f'--socket={sock}',
                               f'--pid-file={root / "server.pid"}', '--bind-address=127.0.0.1',
                               f'--port={port}', f'--ssl-cert={cert}', f'--ssl-key={key}',
                               f'--ssl-ca={cert}', '--skip-name-resolve'], stdout=log, stderr=log)
    try:
        ready = False
        for _ in range(200):
            if server.poll() is not None:
                break
            ping = subprocess.run(['mysql', '--no-defaults', '--protocol=socket', f'--socket={sock}',
                                   '-u', 'root', '-e', 'SELECT 1'], capture_output=True)
            if ping.returncode == 0:
                ready = True
                break
            time.sleep(.05)
        if not ready:
            log.seek(0)
            raise RuntimeError('disposable database failed to start:\n'+log.read())
        password = secrets.token_hex(24)
        sql = f"CREATE USER 'mistlane_test'@'127.0.0.1' IDENTIFIED BY '{password}';"
        subprocess.run(['mysql', '--no-defaults', '--protocol=socket', f'--socket={sock}',
                        '-u', 'root'], input=sql, text=True, check=True, capture_output=True)
        env = dict(os.environ, MISTLANE_MYSQL_HOST='127.0.0.1', MISTLANE_MYSQL_PORT=str(port),
                   MISTLANE_MYSQL_USER='mistlane_test', MISTLANE_MYSQL_PASSWORD=password,
                   MISTLANE_MYSQL_TLS='disable')
        subprocess.run([binary], env=env, check=True, timeout=15)
        # A CA-trusted hostname-verified TLS connection exercises the production default.
        env.update(MISTLANE_MYSQL_HOST='localhost', MISTLANE_MYSQL_TLS='require', SSL_CERT_FILE=str(cert))
        subprocess.run([binary], env=env, check=True, timeout=15)
        # Wrong credentials must fail within the acquisition deadline.
        env['MISTLANE_MYSQL_PASSWORD'] = secrets.token_hex(24)
        result = subprocess.run([binary], env=env, capture_output=True, timeout=15)
        assert result.returncode != 0, 'invalid credentials unexpectedly accepted'
        print('PASS: pooled acquisition/return, parameter escaping, verified TLS and authentication timeout')
    finally:
        server.terminate()
        try:
            server.wait(timeout=10)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()
        log.close()
