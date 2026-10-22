"""Real SQLite plugin, Web iframe and browser experience acceptance."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import threading
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import time
from test_entry import port, service
from test_compute import call


def main():
    output, node = Path(sys.argv[1]).resolve(), sys.argv[2]
    evidence = Path(__file__).resolve().parents[3] / 'evidence/visualization-chart-authoring/ui'
    evidence.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='flowsql-experience-') as temporary:
        directory = Path(temporary)
        database = directory / 'data.db'
        with sqlite3.connect(database) as db:
            for table in ('samples', 'other'):
                db.execute(f'CREATE TABLE {table}(host TEXT,time TEXT,value REAL,x REAL)')
                db.executemany(f'INSERT INTO {table} VALUES(?,?,?,?)', [('alpha', '2026-01-01T00:00:00Z', 10, 1), ('beta', '2026-01-02T00:00:00Z', 20, 2)])
        class Control(BaseHTTPRequestHandler):
            def do_GET(self):
                with sqlite3.connect(database) as db:
                    if self.path == '/schema':
                        count = len(db.execute('PRAGMA table_info(other)').fetchall())
                        db.execute(f'ALTER TABLE other ADD COLUMN extra{count} TEXT')
                    elif self.path == '/remove':
                        db.execute('DROP TABLE other')
                    elif self.path == '/restore':
                        db.execute('CREATE TABLE other(host TEXT,time TEXT,value REAL,x REAL)')
                        db.execute("INSERT INTO other VALUES('alpha','2026-01-01T00:00:00Z',30,3)")
                    else:
                        self.send_error(404); return
                self.send_response(200)
                self.send_header('Access-Control-Allow-Origin', '*')
                self.end_headers(); self.wfile.write(b'ok')
            def log_message(self, *args):
                pass
        control = ThreadingHTTPServer(('127.0.0.1', 0), Control)
        threading.Thread(target=control.serve_forever, daemon=True).start()
        visual, web = port(), port()
        direct, host = f'http://127.0.0.1:{visual}', f'http://127.0.0.1:{web}'
        plugins = f'{output}/libflowsql_database.so:type=sqlite;name=experience;path={database};readonly=true|type=sqlite;name=second;path={database};readonly=true,{output}/libflowsql_visualization.so'
        env = dict(os.environ, LD_LIBRARY_PATH=str(output) + ':' + os.environ.get('LD_LIBRARY_PATH', ''))
        with (evidence / 'service.log').open('w+') as log:
            process = subprocess.Popen([str(output / 'flowsql'), '--role', 'visualization', '--plugins', plugins,
                '--option', json.dumps(dict(port=visual, static_dir=str(output / 'exploration')))], cwd=directory, env=env, stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 15
                while True:
                    try:
                        if call(direct, 'capabilities')[0] == 200:
                            break
                    except OSError:
                        pass
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise AssertionError('isolated experience service failed to start')
                    time.sleep(.05)
                options = f'port={web};db_path={directory}/web.db;upload_dir={directory}/uploads;visualization_upstream={direct}'
                with service(output, directory, 'web', options, host + '/api/version'):
                    subprocess.run([node, str(Path(__file__).with_name('chart_browser.mjs')), direct, host, str(evidence)], check=True, timeout=120)
                    subprocess.run([node, str(Path(__file__).with_name('experience_browser.mjs')), direct, host,
                                    str(evidence), f'http://127.0.0.1:{control.server_port}'], check=True, timeout=180)
            finally:
                control.shutdown(); control.server_close()
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait(); raise AssertionError('experience service shutdown timeout')
                assert process.returncode == 0


if __name__ == '__main__':
    main()
