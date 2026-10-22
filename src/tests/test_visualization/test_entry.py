"""Actual product entry and browser verification, isolated loopback processes only."""
import contextlib
import json
import os
from pathlib import Path
import socket
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request


def port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def request(url):
    try:
        return urllib.request.urlopen(url, timeout=2)
    except urllib.error.HTTPError as error:
        return error


@contextlib.contextmanager
def service(output, directory, name, options, endpoint):
    env = dict(os.environ, LD_LIBRARY_PATH=str(output) + ':' + os.environ.get('LD_LIBRARY_PATH', ''))
    with (directory / (name + '-' + str(port()) + '.log')).open('w+') as log:
        process = subprocess.Popen([str(output / 'flowsql'), '--role', name, '--plugins', str(output / ('libflowsql_' + name + '.so')),
                                    '--option', options], cwd=directory, env=env, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 15
            while True:
                try:
                    with request(endpoint) as response:
                        if response.status == 200:
                            break
                except OSError:
                    pass
                if process.poll() is not None or time.monotonic() > deadline:
                    log.flush(); log.seek(0)
                    raise AssertionError(log.read())
                time.sleep(.1)
            yield process
        finally:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait(timeout=5)


def main():
    output, node = Path(sys.argv[1]).resolve(), sys.argv[2]
    evidence = Path(__file__).resolve().parents[3] / 'evidence/visualization-database/t1'
    evidence.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='flowsql-exploration-entry-') as temporary:
        directory = Path(temporary)
        visual, web, empty, unavailable = port(), port(), port(), port()
        direct, host, unconfigured, unreachable = [f'http://127.0.0.1:{p}' for p in (visual, web, empty, unavailable)]
        static = directory / 'exploration'
        shutil.copytree(output / 'exploration', static)
        options = json.dumps(dict(port=visual, static_dir=str(static)))
        with service(output, directory, 'visualization', options, direct + '/api/visualization/v1/capabilities'):
            def web_options(p, suffix, upstream=''):
                return f'port={p};db_path={directory}/{suffix}.db;upload_dir={directory}/{suffix}-uploads;visualization_upstream={upstream}'
            with service(output, directory, 'web', web_options(web, 'web', direct), host + '/api/version'), \
                 service(output, directory, 'web', web_options(empty, 'empty'), unconfigured + '/api/version'), \
                 service(output, directory, 'web', web_options(unavailable, 'unavailable', f'http://127.0.0.1:{port()}'), unreachable + '/api/version'):
                subprocess.run([node, str(Path(__file__).with_name('browser.mjs')), direct, host, unconfigured,
                                str(evidence / 'browser.json'), unreachable], check=True, timeout=120)
            with request(direct + '/exploration/') as response:
                assert response.status == 200, 'standalone must survive host Web stop'
            manifest = static / 'manifest.json'
            package = json.loads(manifest.read_text())
            package['bridge_version'] = 2
            manifest.write_text(json.dumps(package))
            with request(direct + '/exploration/') as response:
                assert response.status == 409, 'incompatible static package must be rejected'
            package['bridge_version'] = 1
            manifest.write_text(json.dumps(package))
            with (static / 'index.html').open('a') as html:
                html.write('corrupted')
            with request(direct + '/exploration/') as response:
                assert response.status == 409, 'corrupt package must be rejected'
    print('PASS actual entry/browser and independent process lifecycle')


if __name__ == '__main__':
    main()
