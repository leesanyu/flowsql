"""Real database plugin + exploration process + browser computation acceptance."""
import contextlib
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import uuid

from test_entry import port, request


def call(base, path, body=None):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(base + '/api/visualization/v1/' + path, data=data,
                                 headers={'Content-Type': 'application/json'})
    try:
        response = urllib.request.urlopen(req, timeout=40)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        return response.status, json.load(response)


def main():
    output, node = Path(sys.argv[1]).resolve(), sys.argv[2]
    evidence = Path(__file__).resolve().parents[3] / 'evidence/visualization-database/t2'
    evidence.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='flowsql-exploration-compute-') as temporary:
        directory = Path(temporary)
        db = directory / 'data.db'
        with sqlite3.connect(db) as connection:
            connection.executescript('CREATE TABLE samples(host TEXT,time TEXT,value REAL,x REAL,allnull REAL,mixed INTEGER);')
            connection.executemany('INSERT INTO samples VALUES(?,?,?,?,?,?)', [
                ('alpha', '2026-01-01T00:00:00Z', 10, 1, None, 1),
                ('alpha', '2026-01-01T00:01:00Z', None, 2, None, 'text'),
                ('beta', '2026-01-01T00:02:00Z', 20, 3, None, 3)])
        visual = port()
        base = f'http://127.0.0.1:{visual}'
        env = dict(os.environ, LD_LIBRARY_PATH=str(output) + ':' + os.environ.get('LD_LIBRARY_PATH', ''))
        plugins = f'{output}/libflowsql_database.so:type=sqlite;name=example;path={db};readonly=true,{output}/libflowsql_visualization.so'
        with (evidence / 'compute-service.log').open('w+') as log:
            process = subprocess.Popen([str(output / 'flowsql'), '--role', 'visualization', '--plugins', plugins,
                '--option', json.dumps(dict(port=visual, static_dir=str(output / 'exploration')))], cwd=directory,
                env=env, stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 15
                while True:
                    try:
                        if call(base, 'capabilities')[0] == 200:
                            break
                    except OSError:
                        pass
                    if process.poll() is not None or time.monotonic() > deadline:
                        log.flush(); log.seek(0); raise AssertionError(log.read())
                    time.sleep(.1)
                status, sources = call(base, 'sources')
                assert status == 200 and len(sources['sources']) == 1
                source_id = sources['sources'][0]['source_id']
                status, source = call(base, 'sources/' + source_id)
                assert status == 200 and len(source['fields']) == 6
                assert str(db) not in json.dumps(source)
                def compute(workflow, **overrides):
                    payload = dict(request_id=str(uuid.uuid4()), source_id=source_id,
                        schema_revision=source['schema_revision'], workflow_version='graphic-walker-0.5.3',
                        workflow=workflow, detail_mode='complete')
                    payload.update(overrides)
                    return call(base, 'compute', payload)
                workflow = {'workflow': [{'type': 'view', 'query': [{'op': 'aggregate', 'groupBy': [],
                    'measures': [{'field': 'f2', 'agg': 'mean', 'asFieldKey': 'mean'}, {'field': '*', 'agg': 'count', 'asFieldKey': 'rows'},
                                 {'field': 'f2', 'agg': 'count', 'asFieldKey': 'nonnull'}]}]}]}
                status, result = compute(workflow)
                assert status == 200 and result['rows'] == [{'mean': 15, 'rows': 3, 'nonnull': 2}], result
                assert result['execution'] == 'database'
                assert compute(workflow, schema_revision='stale')[1]['code'] == 'SCHEMA_CHANGED'
                invalid = {'workflow': [{'type': 'transform', 'transform': []}]}
                assert compute(invalid)[1]['code'] == 'UNSUPPORTED_OPERATION'
                status, cancel = call(base, 'cancel', {'request_id': str(uuid.uuid4())})
                assert status == 200 and cancel['status'] == 'not_running'
                (evidence / 'api.json').write_text(json.dumps(dict(source=source, result=result), indent=2))
                subprocess.run([node, str(Path(__file__).with_name('compute_browser.mjs')), base,
                                str(evidence / 'browser.json')], check=True, timeout=120)
                browser = json.loads((evidence / 'browser.json').read_text())
                baselines = []
                for workflow in browser['workflows']:
                    query = next(step['query'][0] for step in workflow['workflow'] if step['type'] == 'view')
                    if query['op'] == 'raw':
                        baselines.append(('SELECT x,value FROM samples', ['f3', 'f2']))
                    elif query['groupBy'] == ['f1']:
                        baselines.append(('SELECT time,AVG(value) FROM samples GROUP BY time', ['f1', 'f2_mean']))
                    elif any(step['type'] == 'filter' for step in workflow['workflow']):
                        baselines.append(("SELECT host,AVG(value) FROM samples WHERE host='alpha' GROUP BY host", ['f0', 'f2_mean']))
                    else:
                        baselines.append(('SELECT host,AVG(value) FROM samples GROUP BY host', ['f0', 'f2_mean']))
                with sqlite3.connect(db) as connection:
                    for result, (sql, names) in zip(browser['results'], baselines, strict=True):
                        expected = [dict(zip(names, row)) for row in connection.execute(sql)]
                        assert result['rows'] == expected, (sql, result)
                browser['native_sql_baselines'] = [sql for sql, _ in baselines]
                browser['executed'] = browser['passed'] + browser['failed']
                (evidence / 'browser.json').write_text(json.dumps(browser, indent=2))
                with sqlite3.connect(db) as connection:
                    assert connection.execute('SELECT COUNT(*) FROM samples').fetchone()[0] == 3
            finally:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait(timeout=5)
    print('PASS real database discovery, native computation, schema/version validation and browser charts')


if __name__ == '__main__':
    main()
