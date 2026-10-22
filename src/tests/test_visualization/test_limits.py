"""Bounded results, real SQLite cancellation/deadline, concurrent isolation and shutdown."""
import concurrent.futures
import contextlib
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import time
import uuid

from test_entry import port
from test_compute import call


@contextlib.contextmanager
def running(output, directory, db, limits, report):
    endpoint = f'http://127.0.0.1:{port()}'
    options = dict(port=int(endpoint.rsplit(':', 1)[1]), static_dir=str(output / 'exploration'), limits=limits)
    config = directory / 'channels.yaml'
    config.write_text('channels:\n  database_channels:\n    - type: sqlite\n      name: fixture\n      path: ' + str(db) + '\n      readonly: "true"\n')
    plugins = f'{output}/libflowsql_database.so:config_file={config},{output}/libflowsql_visualization.so'
    env = dict(os.environ, LD_LIBRARY_PATH=str(output) + ':' + os.environ.get('LD_LIBRARY_PATH', ''))
    with report.open('w+') as log:
        process = subprocess.Popen([str(output / 'flowsql'), '--role', 'visualization', '--plugins', plugins,
            '--option', json.dumps(options)], cwd=directory, env=env, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 15
            while True:
                try:
                    if call(endpoint, 'capabilities')[0] == 200:
                        break
                except OSError:
                    pass
                if process.poll() is not None or time.monotonic() > deadline:
                    log.seek(0); raise AssertionError(log.read())
                time.sleep(.05)
            status, sources = call(endpoint, 'sources')
            assert status == 200, sources
            metadata = {}
            for source in sources['sources']:
                status, data = call(endpoint, 'sources/' + source['source_id'])
                assert status == 200, data
                metadata[source['label'].split('.', 1)[1]] = data
            yield endpoint, metadata, process, config
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait(); raise AssertionError('shutdown exceeded 2s')


def payload(source, aggregate=False, request_id=None, **options):
    query = dict(op='aggregate', groupBy=[], measures=[dict(field='f0', agg='mean', asFieldKey='avg')]) if aggregate else dict(op='raw', fields=['f0'])
    workflow = {'workflow': [{'type': 'view', 'query': [query]}]}
    result = dict(request_id=request_id or str(uuid.uuid4()), source_id=source['source_id'],
        schema_revision=source['schema_revision'], workflow_version='graphic-walker-0.5.3', workflow=workflow, detail_mode='complete')
    result.update(options)
    return result


def main():
    output = Path(sys.argv[1]).resolve()
    evidence = Path(__file__).resolve().parents[3] / 'evidence/visualization-database/t3'
    evidence.mkdir(parents=True, exist_ok=True)
    checks = []
    def passed(name):
        checks.append(name); print('PASS', name, flush=True)
    with tempfile.TemporaryDirectory(prefix='flowsql-exploration-limits-') as temporary:
        directory = Path(temporary); db = directory / 'data.db'
        with sqlite3.connect(db) as connection:
            connection.executescript('CREATE TABLE small(n INTEGER); CREATE TABLE slow(n INTEGER); CREATE TABLE huge(n INTEGER);'
                'CREATE TABLE mixed(n INTEGER);CREATE TABLE large(n TEXT);')
            connection.executemany('INSERT INTO small VALUES(?)', [(n,) for n in range(40)])
            connection.execute('WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<3000000) INSERT INTO slow SELECT x FROM n')
            connection.execute('INSERT INTO huge VALUES(9007199254740993)')
            connection.execute("INSERT INTO mixed VALUES('not a number')")
            connection.execute('INSERT INTO large VALUES(?)', ('"\\' * 3000,))
        before = hashlib.sha256(db.read_bytes()).hexdigest()
        with running(output, directory, db, dict(max_rows=10, max_bytes=4096, timeout_ms=5000, max_active=2), evidence/'limits.log') as (base, sources, process, config):
            config_before = config.read_bytes()
            assert call(base, 'compute', payload(sources['small']))[1]['code'] == 'RESULT_LIMIT'
            limited = call(base, 'compute', payload(sources['small'], detail_mode='limited'))
            assert limited[0] == 200 and limited[1]['scope'] == 'limited_detail' and limited[1]['returned_rows'] == 10
            top = payload(sources['small']); top['workflow']['limit'] = 3
            top_result = call(base, 'compute', top)
            assert top_result[0] == 200 and top_result[1]['scope'] == 'top_n' and top_result[1]['returned_rows'] == 3
            grouped = payload(sources['small']); grouped['workflow']['workflow'][0]['query'][0] = dict(op='aggregate',groupBy=['f0'],measures=[])
            assert call(base, 'compute', grouped)[1]['code'] == 'RESULT_LIMIT'
            passed('row_limits_and_scope')
            assert call(base, 'compute', payload(sources['large']))[1]['code'] == 'RESULT_LIMIT'
            passed('byte_limit')
            for name in ('huge', 'mixed'):
                assert call(base, 'compute', payload(sources[name]))[1]['code'] == 'UNSUPPORTED_TYPE'
                assert call(base, 'compute', payload(sources[name], aggregate=True))[1]['code'] == 'UNSUPPORTED_TYPE'
            passed('exact_integer_and_mixed_type_protection')
            with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
                one, two = payload(sources['slow'], True), payload(sources['slow'], True)
                future_one = pool.submit(call, base, 'compute', one)
                time.sleep(.025)
                assert call(base, 'compute', one)[1]['code'] == 'DUPLICATE_REQUEST'
                future_two = pool.submit(call, base, 'compute', two)
                time.sleep(.025)
                assert call(base, 'compute', payload(sources['small'], True))[1]['code'] == 'BUSY'
                started = time.monotonic()
                assert call(base, 'cancel', {'request_id': one['request_id']})[1]['status'] == 'cancel_requested'
                assert future_one.result(timeout=2)[1]['code'] == 'CANCELLED'
                assert time.monotonic() - started < 2
                second = future_two.result(timeout=4)
                assert second[0] == 200 and second[1]['rows'] == [{'avg': 1500000.5}], second
                assert call(base, 'compute', payload(sources['small'], True))[0] == 200
            passed('real_cancel_duplicate_busy_and_second_request_isolation')
            assert config.read_bytes() == config_before
            passed('read_only_shared_configuration')
        with running(output, directory, db, dict(timeout_ms=20), evidence/'timeout.log') as (base, sources, process, config):
            started = time.monotonic()
            result = call(base, 'compute', payload(sources['slow'], True))
            assert result[1]['code'] == 'TIMEOUT', result
            assert time.monotonic() - started < 2
            passed('real_deadline')
        with running(output, directory, db, dict(timeout_ms=5000), evidence/'shutdown.log') as (base, sources, process, config):
            with concurrent.futures.ThreadPoolExecutor() as pool:
                query = pool.submit(call, base, 'compute', payload(sources['slow'], True))
                time.sleep(.025)
                started = time.monotonic(); process.terminate(); process.wait(timeout=2)
                assert time.monotonic() - started < 2
                try:
                    query.result(timeout=2)
                except OSError:
                    pass
            assert process.returncode == 0, process.returncode
            passed('real_shutdown_drains_sessions')
        assert hashlib.sha256(db.read_bytes()).hexdigest() == before
        passed('source_database_unchanged')
    (evidence/'limits.json').write_text(json.dumps(dict(selected=8, executed=len(checks), passed=len(checks), failed=0, skipped=0,not_executed=8-len(checks),checks=checks),indent=2))


if __name__ == '__main__':
    main()
