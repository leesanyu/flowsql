"""Query the actual CLI and both HTTP listeners, with isolated process state."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.request

OUTPUT = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else Path('build/output').resolve()


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


class ProductVersionRuntimeTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='flowsql-version-runtime-')
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.info = json.loads((OUTPUT / 'build-info.json').read_text())
        self.env = dict(os.environ, PYTHONPATH=str(OUTPUT / 'python'),
                        LD_LIBRARY_PATH=str(OUTPUT) + ':' + os.environ.get('LD_LIBRARY_PATH', ''))

    def test_native_and_worker_without_runtime_dependencies(self):
        dirty = 'unknown' if self.info['dirty'] is None else str(self.info['dirty']).lower()
        expected = (f"FlowSQL {self.info['version']}\nrevision={self.info['revision'] or 'unknown'}"
                    f"\ndirty={dirty}\n")
        for command in [[str(OUTPUT / 'flowsql'), '--config', 'missing.yml', '--version'],
                        [str(OUTPUT / 'flowsql-worker'), '--version'],
                        [sys.executable, '-S', '-m', 'flowsql.worker', '--version']]:
            with self.subTest(command=command):
                result = subprocess.run(command, cwd=self.directory, env=self.env, text=True,
                                        capture_output=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, expected)
                self.assertEqual(result.stderr, '')
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_web_direct_and_enumerated_routes(self):
        direct, router = free_port(), free_port()
        plugins = (f"{OUTPUT}/libflowsql_web.so:host=127.0.0.1;port={direct};db_path={self.directory}/web.db,"
                   f"{OUTPUT}/libflowsql_router.so:host=127.0.0.1;port={router}")
        with (self.directory / 'server.log').open('w') as log:
            process = subprocess.Popen([str(OUTPUT / 'flowsql'), '--role', 'version-test', '--plugins', plugins],
                                       cwd=self.directory, env=self.env, stdout=log, stderr=log)
            try:
                for port in (direct, router):
                    deadline = time.monotonic() + 15
                    while True:
                        try:
                            response = urllib.request.urlopen(f'http://127.0.0.1:{port}/api/version', timeout=1)
                            break
                        except (OSError, urllib.error.URLError):
                            if time.monotonic() >= deadline or process.poll() is not None:
                                self.fail((self.directory / 'server.log').read_text())
                            time.sleep(0.1)
                    with response:
                        self.assertEqual(response.status, 200)
                        self.assertEqual(response.headers.get_content_type(), 'application/json')
                        self.assertEqual(response.headers.get('Cache-Control'), 'no-store')
                        self.assertEqual(json.load(response), self.info)
                    with urllib.request.urlopen(f'http://127.0.0.1:{port}/api/health', timeout=2) as response:
                        self.assertEqual(json.load(response), {'status': 'ok'})
            finally:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)


if __name__ == '__main__':
    unittest.main()
