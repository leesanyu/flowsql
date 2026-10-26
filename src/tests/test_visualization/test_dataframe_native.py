# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""T5 acceptance: isolated native deployment from import/SQL output to versioned exploration."""
import json
import os
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import uuid
from pathlib import Path

output = Path(sys.argv[1]).resolve()
repository = Path(__file__).resolve().parents[3]
checks = []


def passed(name):
    checks.append(name)
    print('PASS', name, flush=True)


def port():
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


def call(base, path, body=None, timeout=60):
    data = None if body is None else json.dumps(body).encode()
    request = urllib.request.Request(base + path, data=data,
                                     headers={} if body is None else {'Content-Type': 'application/json'},
                                     method='GET' if body is None else 'POST')
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.status, json.loads(response.read() or b'{}')
    except urllib.error.HTTPError as error:
        return error.code, json.loads(error.read() or b'{}')


def wait_ready(base, path, child, log, label):
    deadline = time.monotonic() + 30
    while True:
        try:
            request = urllib.request.Request(base + path)
            with urllib.request.urlopen(request, timeout=3) as response:
                if response.status == 200:
                    return
        except (OSError, ValueError, urllib.error.HTTPError):
            pass
        if child.poll() is not None or time.monotonic() >= deadline:
            log.seek(0)
            raise AssertionError(f'{label} did not start: {log.read()[-2000:]}')
        time.sleep(0.05)


def stop(children):
    for child in children:
        child.terminate()
    for child in children:
        try:
            child.wait(timeout=10)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()


def dataframe_source(visual, label):
    status, listing = call(visual, '/api/visualization/v1/sources')
    assert status == 200, listing
    for item in listing['sources']:
        if item.get('kind') == 'dataframe' and item['label'] == label:
            return item['source_id']
    return ''


def acquire(visual, source_id):
    status, snapshot = call(visual, '/api/visualization/v1/snapshots/acquire',
                            dict(request_id=str(uuid.uuid4()), source_id=source_id))
    assert status == 200, snapshot
    return snapshot


def count_rows(visual, source_id, snapshot):
    workflow = [{'type': 'view', 'query': [{'op': 'aggregate', 'groupBy': [], 'measures': [
        {'field': '*', 'agg': 'count', 'asFieldKey': 'rows'}]}]}]
    status, result = call(visual, '/api/visualization/v1/compute',
                          dict(request_id=str(uuid.uuid4()), source_id=source_id, snapshot_id=snapshot['snapshot_id'],
                               schema_revision=snapshot['schema_revision'], workflow_version='graphic-walker-0.5.3',
                               workflow={'workflow': workflow}, detail_mode='complete'))
    assert status == 200, result
    assert result['execution'] == 'dataframe' and result['snapshot_id'] == snapshot['snapshot_id'], result
    return result['rows'][0]['rows']


def main():
    # Default source artifacts and configurations must already describe the data plane.
    for artifact in ('flowsql', 'libflowsql_catalog.so', 'libflowsql_visualization.so', 'exploration/manifest.json'):
        assert (output / artifact).exists(), artifact
    single = (repository / 'config/deploy-single.yaml').read_text()
    standalone = (repository / 'config/deploy-visualization.yaml').read_text()
    # Both sides need a 0700 directory, so the default must sit on a POSIX filesystem that stores
    # permission bits; a repository-relative path under /mnt/d (9p/DrvFs) can never be 0700.
    shared_dir = '/var/tmp/flowsql/exploration-shared'
    assert f'exploration_shared_dir={shared_dir}' in single
    assert '"dataframe_upstream":"http://127.0.0.1:18803"' in single
    assert f'"dataframe_shared_dir":"{shared_dir}"' in single
    assert '"dataframe_upstream":"http://127.0.0.1:18803"' in standalone
    assert f'"dataframe_shared_dir":"{shared_dir}"' in standalone
    assert not str(repository) in shared_dir
    doc = (repository / 'docs/exploration.md').read_text()
    for phrase in ('DataFrame 快照探索', 'exploration_shared_dir', '获取最新快照', 'SNAPSHOT_EXPIRED', '15 分钟'):
        assert phrase in doc, phrase
    passed('default artifacts and user documentation describe the data plane')

    root = Path(tempfile.mkdtemp(prefix='flowsql-dataframe-native-'))
    shared = root / 'exploration-shared'
    dataframes = root / 'dataframes'
    dataframes.mkdir()
    (dataframes / 'restored.csv').write_text('name,value\nalpha,1\nbeta,2\n')
    (dataframes / 'preview_only.csv').write_text('name,value\ngamma,3\n')
    (dataframes / 'unused.csv').write_text('name,value\nunread,9\n')
    (dataframes / 'overwrite.csv').write_text('')
    database = root / 'source.db'
    with sqlite3.connect(database) as connection:
        connection.execute('CREATE TABLE samples(host TEXT, value INTEGER)')
        connection.executemany('INSERT INTO samples VALUES(?,?)', [('alpha', 1), ('beta', 2)])
    source_url = f'http://127.0.0.1:{port()}'
    visual_url = f'http://127.0.0.1:{port()}'
    env = dict(os.environ, LD_LIBRARY_PATH=f'{output}:' + os.environ.get('LD_LIBRARY_PATH', ''))
    core_plugins = ','.join([
        f'{output}/libflowsql_builtin.so',
        f'{output}/libflowsql_catalog.so:data_dir={dataframes};exploration_shared_dir={shared};'
        f'operator_db_path={root}/meta.db',
        f'{output}/libflowsql_database.so:type=sqlite;name=local;path={database};readonly=true',
        f'{output}/libflowsql_scheduler.so',
        f'{output}/libflowsql_router.so:host=127.0.0.1;port={source_url.rsplit(":", 1)[1]}',
    ])
    visualization_plugins = f'{output}/libflowsql_visualization.so'
    visualization_options = json.dumps(dict(port=int(visual_url.rsplit(':', 1)[1]),
                                            static_dir=str(output / 'exploration'),
                                            dataframe_upstream=source_url,
                                            dataframe_shared_dir=str(shared)))
    with (root / 'core.log').open('w+') as core_log, (root / 'visual.log').open('w+') as visual_log:
        core = subprocess.Popen([str(output / 'flowsql'), '--role', 'all', '--plugins', core_plugins],
                                cwd=root, env=env, stdout=core_log, stderr=core_log)
        visual = subprocess.Popen([str(output / 'flowsql'), '--role', 'visualization', '--plugins',
                                   visualization_plugins, '--option', visualization_options],
                                  cwd=root, env=env, stdout=visual_log, stderr=visual_log)
        try:
            wait_ready(source_url, '/channels/dataframe/exploration/v1/sources', core, core_log, 'core')
            wait_ready(visual_url, '/api/visualization/v1/capabilities', visual, visual_log, 'visualization')
            assert shared.is_dir() and (shared.stat().st_mode & 0o077) == 0, oct(shared.stat().st_mode)
            assert (shared / '.owner.lock').exists()

            status, listing = call(source_url, '/channels/dataframe')
            assert status == 200 and len(listing['channels']) == 4, listing
            assert all(item['rows'] is None and item['schema'] is None and item['load_state'] == 'unloaded'
                       for item in listing['channels']), listing
            status, generic = call(source_url, '/channels/dataframe/query', {})
            assert status == 200, generic
            assert all(item['schema'] == 'null' for item in generic if item['category'] == 'dataframe'), generic
            status, _ = call(source_url, '/scheduler/batch/execute',
                             dict(sql='SELECT * FROM dataframe.restored INTO dataframe.recovered_output'))
            assert status == 200
            status, preview = call(source_url, '/channels/dataframe/preview', dict(name='recovered_output'))
            assert status == 200 and preview['rows'] == 2 and preview['data'][0] == ['alpha', 1], preview
            status, preview = call(source_url, '/channels/dataframe/preview', dict(name='preview_only'))
            assert status == 200 and preview['rows'] == 1 and preview['data'][0] == ['gamma', 3], preview
            status, replaced = call(source_url, '/scheduler/batch/execute',
                                    dict(sql='SELECT * FROM sqlite.local.samples INTO dataframe.overwrite'))
            assert status == 200, replaced
            status, listing = call(source_url, '/channels/dataframe')
            channels = {item['name']: item for item in listing['channels']}
            assert channels['unused']['load_state'] == 'unloaded', channels
            assert all(channels[name]['load_state'] == 'loaded' for name in ('restored', 'preview_only', 'overwrite'))
            restored_id = dataframe_source(visual_url, 'dataframe.recovered_output')
            restored_snapshot = acquire(visual_url, restored_id)
            assert count_rows(visual_url, restored_id, restored_snapshot) == 2
            passed('cold restoration: metadata/SQL/preview/snapshot and unread overwrite isolation')

            # CSV import through the running source service becomes an explorable named DataFrame.
            csv = root / 'import.csv'
            csv.write_text('city,score\nshanghai,7\nbeijing,8\nshenzhen,9\n')
            status, imported = call(source_url, '/channels/dataframe/import',
                                    dict(filename='import.csv', tmp_path=str(csv)))
            assert status == 200, imported
            status, channels = call(source_url, '/channels/dataframe')
            names = [item.get('name') for item in channels.get('channels', [])]
            imported_name = [name for name in names if 'import' in name]
            assert imported_name, channels
            imported_source = dataframe_source(visual_url, 'dataframe.' + imported_name[0])
            assert imported_source, 'imported DataFrame is discoverable'
            snapshot = acquire(visual_url, imported_source)
            assert snapshot['row_count'] == 3 and count_rows(visual_url, imported_source, snapshot) == 3
            first = snapshot['snapshot_id']
            snapshot = acquire(visual_url, imported_source)
            assert snapshot['snapshot_id'] != first, 'a new capture always yields a new version'
            assert count_rows(visual_url, imported_source, snapshot) == 3
            passed('CSV import to exploration with versioned snapshot')

            # SQL named output uses the same path.
            sql = 'SELECT * FROM sqlite.local.samples INTO dataframe.sql_output'
            status, executed = call(source_url, '/scheduler/batch/execute', dict(sql=sql))
            assert status == 200, executed
            source_id = dataframe_source(visual_url, 'dataframe.sql_output')
            assert source_id, 'named SQL output is discoverable'
            snapshot = acquire(visual_url, source_id)
            assert snapshot['row_count'] == 2, snapshot
            assert count_rows(visual_url, source_id, snapshot) == 2
            rows = call(visual_url, '/api/visualization/v1/compute',
                        dict(request_id=str(uuid.uuid4()), source_id=source_id, snapshot_id=snapshot['snapshot_id'],
                             schema_revision=snapshot['schema_revision'], workflow_version='graphic-walker-0.5.3',
                             workflow={'workflow': [{'type': 'view', 'query': [{'op': 'raw',
                                                                                 'fields': ['f0', 'f1']}]}]},
                             detail_mode='complete'))
            assert rows[0] == 200 and [row['f0'] for row in rows[1]['rows']] == ['alpha', 'beta'], rows
            passed('SQL named output to exploration with raw detail')

            # The source process leaving does not invalidate a snapshot already bound to the page.
            status, bound = call(visual_url, '/api/visualization/v1/snapshots/renew',
                                 dict(snapshot_id=snapshot['snapshot_id']))
            assert status == 200, bound
            assert count_rows(visual_url, source_id, snapshot) == 2
            passed('bound snapshot remains computable')

            # Deleting the source keeps the existing snapshot but fails a new capture explicitly.
            call(source_url, '/channels/dataframe/delete', dict(name='sql_output'))
            assert dataframe_source(visual_url, 'dataframe.sql_output') == ''
            assert count_rows(visual_url, source_id, snapshot) == 2
            status, missing = call(visual_url, '/api/visualization/v1/snapshots/acquire',
                                   dict(request_id=str(uuid.uuid4()), source_id=source_id))
            assert status in (404, 503), missing
            passed('source deletion keeps old snapshot and rejects recapture')
        finally:
            stop([visual, core])
        assert not [name for name in shared.iterdir() if name.name != '.owner.lock'], list(shared.iterdir())
        passed('shutdown leaves no published objects')

    # A deployment without a data plane keeps the database path usable.
    database_config = root / 'channels.json'
    database_config.write_text(json.dumps({'channels': {'database_channels': [
        dict(type='sqlite', name='local', path=str(database), readonly='true')]}}))
    database_config.chmod(0o600)
    degraded_url = f'http://127.0.0.1:{port()}'
    with (root / 'degraded.log').open('w+') as degraded_log:
        degraded = subprocess.Popen([str(output / 'flowsql'), '--role', 'visualization', '--plugins',
                                     f'{output}/libflowsql_database.so:config_file={database_config},'
                                     f'{output}/libflowsql_visualization.so', '--option',
                                     json.dumps(dict(port=int(degraded_url.rsplit(':', 1)[1]),
                                                     static_dir=str(output / 'exploration')))],
                                    cwd=root, env=env, stdout=degraded_log, stderr=degraded_log)
        try:
            wait_ready(degraded_url, '/api/visualization/v1/capabilities', degraded, degraded_log, 'degraded')
            status, listing = call(degraded_url, '/api/visualization/v1/sources')
            assert status == 200, listing
            assert [item for item in listing['sources'] if item['label'].endswith('.samples')], listing
            status, failure = call(degraded_url, '/api/visualization/v1/snapshots/acquire',
                                   dict(request_id=str(uuid.uuid4()), source_id='0' * 64))
            assert status == 503 and failure['code'] == 'DATA_PLANE_UNAVAILABLE', failure
            passed('missing data plane keeps database exploration available')

            catalog_only = subprocess.Popen(
                [str(output / 'flowsql'), '--role', 'catalog', '--plugins',
                 f'{output}/libflowsql_builtin.so,{output}/libflowsql_catalog.so:data_dir={dataframes};'
                 f'operator_db_path={root}/meta.db'],
                cwd=root, env=env, stdout=degraded_log, stderr=degraded_log)
            try:
                time.sleep(0.5)
                assert catalog_only.poll() is None
            finally:
                stop([catalog_only])
            # Without exploration_shared_dir the source starts but the data plane stays unconfigured.
            status, failure = call(degraded_url, '/api/visualization/v1/snapshots/acquire',
                                   dict(request_id=str(uuid.uuid4()), source_id='0' * 64))
            assert status == 503, failure
            passed('source without shared directory starts without a data plane')
        finally:
            stop([degraded])
    print(json.dumps({'checks': checks}))


if __name__ == '__main__':
    main()
