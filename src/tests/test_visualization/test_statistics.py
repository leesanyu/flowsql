"""Native statistics API and actual standalone/embedded SQLite chart acceptance."""
from contextlib import contextmanager
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import time
import uuid

from test_compute import call
from test_entry import port, service

ROOT = Path(__file__).resolve().parents[3]
ROWS = [('2024-02-29T23:30:00-03:00', 'x', 9007199254740993),
        ('2024-02-29T01:00:00Z', 'x', 9007199254740992),
        ('2024-03-01', '', 2), ('2025-01-01', None, 3), (None, 'y', 4)]


@contextmanager
def explorer(output, directory, plugins):
    base = f'http://127.0.0.1:{port()}'
    env = dict(os.environ, LD_LIBRARY_PATH=str(output) + ':' + os.environ.get('LD_LIBRARY_PATH', ''))
    with (directory/'statistics-service.log').open('w+') as log:
        child = subprocess.Popen([str(output/'flowsql'), '--role', 'visualization', '--plugins', plugins,
            '--option', json.dumps(dict(port=int(base.rsplit(':', 1)[1]), static_dir=str(output/'exploration')))],
            cwd=directory, env=env, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 15
            while True:
                try:
                    if call(base, 'capabilities')[0] == 200:
                        break
                except OSError:
                    pass
                if child.poll() is not None or time.monotonic() >= deadline:
                    raise AssertionError('isolated statistics service failed to start')
                time.sleep(.05)
            yield base
        finally:
            child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill(); child.wait()
                raise AssertionError('statistics service shutdown timeout')
            assert child.returncode == 0


def compute(base, source, query, filters=None, **options):
    steps = ([dict(type='filter', filters=filters)] if filters else []) + [dict(type='view', query=[query])]
    return call(base, 'compute', dict(request_id=str(uuid.uuid4()), source_id=source['source_id'],
        schema_revision=source['schema_revision'], workflow_version='graphic-walker-0.5.3',
        workflow=dict(workflow=steps, **options), detail_mode='complete'))


def main():
    output, node = Path(sys.argv[1]).resolve(), sys.argv[2]
    evidence = ROOT/'evidence/visualization-statistics/sqlite'
    evidence.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='flowsql-statistics-') as temporary:
        directory = Path(temporary)
        database = directory/'data.db'
        with sqlite3.connect(database) as db:
            for table in ('samples', 'other'):
                db.execute(f'CREATE TABLE {table}(stamp TEXT,identity TEXT,wide INTEGER)')
                db.executemany(f'INSERT INTO {table} VALUES(?,?,?)', ROWS)
        plugins = f'{output}/libflowsql_database.so:type=sqlite;name=statistics;path={database};readonly=true,{output}/libflowsql_visualization.so'
        with explorer(output, directory, plugins) as base:
            status, listing = call(base, 'sources')
            assert status == 200
            item = next(s for s in listing['sources'] if s['label'] == 'statistics.samples')
            status, source = call(base, 'sources/'+item['source_id'])
            assert status == 200 and source['time_bucket_fields'] == ['f0', 'f1']
            assert source['semantics']['timezone'] == 'source_calendar'
            for field, expected in [('f1', 3), ('f2', 5)]:
                status, data = compute(base, source, dict(op='aggregate',groupBy=[],measures=[dict(field=field,agg='distinctCount',asFieldKey='n')]))
                assert status == 200 and data['rows'] == [{'n':expected}], data
            query = dict(op='aggregate',groupBy=[dict(field='f0',unit='month',asFieldKey='bucket')],
                measures=[dict(field='f1',agg='distinctCount',asFieldKey='n')])
            status, data = compute(base, source, query, [dict(fid='f1',rule=dict(type='one of',value=['x']))])
            assert status == 200 and data['rows'] == [{'bucket':'2024-02-01','n':1}], data
            status, data = compute(base, source, query, limit=1)
            assert status == 200 and data['scope'] == 'top_n' and len(data['rows']) == 1
            query['groupBy'][0]['unit'] = 'week'
            assert compute(base, source, query)[1]['code'] == 'UNSUPPORTED_OPERATION'
            web_port = port()
            host = f'http://127.0.0.1:{web_port}'
            options = f'port={web_port};db_path={directory}/web.db;upload_dir={directory}/uploads;visualization_upstream={base}'
            with service(output, directory, 'web', options, host+'/api/version'):
                report = evidence/'browser.json'
                subprocess.run([node, str(Path(__file__).with_name('statistics_browser.mjs')), base, host,
                    'statistics.samples', str(report)], check=True, timeout=180)
                result = json.loads(report.read_text())
                assert result['selected'] == result['executed'] == result['passed'] == 14 and not result['failed']
    print('PASS statistics SQLite native API, exact distinct wide integers and 14 browser checks')


if __name__ == '__main__':
    main()
