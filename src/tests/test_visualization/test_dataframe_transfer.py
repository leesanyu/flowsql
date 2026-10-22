# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Two-process acceptance for the DataFrame snapshot data plane.

A real Catalog process publishes Arrow IPC objects; an independent visualization
process maps them read-only. No HTTP body ever carries data rows.
"""

import contextlib
import json
import os
import socket
import stat
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import uuid
from pathlib import Path

output = Path(sys.argv[1]).resolve()
checks = []


def passed(name):
    checks.append(name)
    print('PASS', name, flush=True)


def port():
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


def call(base, path, body=None, timeout=30):
    data = None if body is None else json.dumps(body).encode()
    request = urllib.request.Request(base + path, data=data,
                                     headers={} if body is None else {'Content-Type': 'application/json'},
                                     method='GET' if body is None else 'POST')
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.status, json.loads(response.read() or b'{}')
    except urllib.error.HTTPError as error:
        return error.code, json.loads(error.read() or b'{}')


def wait_ready(base, child, log, label, probe, expected=200):
    deadline = time.monotonic() + 20
    while True:
        try:
            status, _ = call(base, probe, timeout=2)
            if status == expected:
                return
        except (OSError, ValueError):
            pass
        if child.poll() is not None:
            log.seek(0)
            raise AssertionError(f'{label} exited: {log.read()[-2000:]}')
        if time.monotonic() >= deadline:
            log.seek(0)
            raise AssertionError(f'{label} did not start: {log.read()[-2000:]}')
        time.sleep(0.05)


def stop(child, log, label):
    child.terminate()
    try:
        child.wait(timeout=5)
    except subprocess.TimeoutExpired:
        child.kill()
        child.wait()
        raise AssertionError(f'{label} shutdown exceeded 5s')
    assert child.returncode == 0, f'{label} exit code {child.returncode}'
    log.seek(0)
    return log.read()


def main():
    root = Path(tempfile.mkdtemp(prefix='flowsql-dataframe-transfer-'))
    shared = root / 'shared'
    shared.mkdir(mode=0o700)
    os.chmod(shared, 0o700)
    data_dir = root / 'dataframes'
    data_dir.mkdir()
    (data_dir / 'sales.csv').write_text('region,amount\nnorth,10\nsouth,20\nnorth,5\n')
    source_base = f'http://127.0.0.1:{port()}'
    visualization_base = f'http://127.0.0.1:{port()}'
    env = dict(os.environ, LD_LIBRARY_PATH=f'{output}:' + os.environ.get('LD_LIBRARY_PATH', ''))
    source_option = f'data_dir={data_dir};exploration_shared_dir={shared};operator_db_path={root}/meta.db'
    database_config = root / 'channels.json'
    database_config.write_text(json.dumps({'channels': {'database_channels': []}}))
    database_config.chmod(0o600)
    visualization_plugins = (f'{output}/libflowsql_database.so:config_file={database_config},'
                             f'{output}/libflowsql_visualization.so')
    visualization_option = json.dumps(dict(port=int(visualization_base.rsplit(':', 1)[1]),
                                           static_dir=str(output / 'exploration'),
                                           dataframe_upstream=source_base,
                                           dataframe_shared_dir=str(shared)))

    with (root / 'source.log').open('w+') as source_log, (root / 'visualization.log').open('w+') as visualization_log:
        source = subprocess.Popen([str(output / 'flowsql'), '--role', 'catalog', '--plugins',
                                   f'{output}/libflowsql_builtin.so,{output}/libflowsql_catalog.so:{source_option},'
                                   f'{output}/libflowsql_router.so:host=127.0.0.1;port={source_base.rsplit(":", 1)[1]}'],
                                  cwd=root, env=env, stdout=source_log, stderr=source_log)
        try:
            wait_ready(source_base, source, source_log, 'source',
                       '/channels/dataframe/exploration/v1/sources')
            status, listing = call(source_base, '/channels/dataframe/exploration/v1/sources')
            assert status == 200 and [item['label'] for item in listing['sources']] == ['dataframe.sales'], listing
            passed('source lists named DataFrame without data')

            visualization = subprocess.Popen([str(output / 'flowsql'), '--role', 'visualization', '--plugins',
                                              visualization_plugins, '--option', visualization_option],
                                             cwd=root, env=env, stdout=visualization_log, stderr=visualization_log)
            try:
                wait_ready(visualization_base, visualization, visualization_log, 'visualization',
                           '/api/visualization/v1/capabilities')
                status, merged = call(visualization_base, '/api/visualization/v1/sources')
                assert status == 200, merged
                frames = [item for item in merged['sources'] if item.get('kind') == 'dataframe']
                assert len(frames) == 1 and frames[0]['label'] == 'dataframe.sales', merged
                passed('visualization merges DataFrame discovery with database sources')
                source_id = frames[0]['source_id']
                assert len(source_id) == 64, source_id

                status, detail = call(visualization_base, f'/api/visualization/v1/sources/{source_id}')
                assert status == 200 and detail['kind'] == 'dataframe' and detail['status'] == 'available', detail
                assert [field['label'] for field in detail['fields']] == [], detail
                assert not list(shared.glob('*.arrow')), 'describing a source must not create a snapshot'
                passed('source detail is identity only and creates no snapshot')

                request_id = str(uuid.uuid4())
                status, snapshot = call(visualization_base, '/api/visualization/v1/snapshots/acquire',
                                        dict(request_id=request_id, source_id=source_id))
                assert status == 200, snapshot
                assert snapshot['source_id'] == source_id and snapshot['row_count'] == 3, snapshot
                assert snapshot['ipc_bytes'] > 0 and snapshot['expires_at_unix'] > time.time(), snapshot
                assert len(snapshot['snapshot_id']) == 64 and snapshot['captured_at'].endswith('Z'), snapshot
                assert [field['label'] for field in snapshot['fields']] == ['region', 'amount'], snapshot
                assert [field['type'] for field in snapshot['fields']] == ['string', 'number'], snapshot
                # The receiver confirms the release once its mapping exists, so the object is retired
                # by the time acquire returns; nothing under the managed directory is a data row.
                assert not list(shared.glob('*.arrow')), list(shared.glob('*.arrow'))
                assert not list(shared.glob('.*.partial')), list(shared.glob('.*.partial'))
                passed('acquire returns metadata only and retires the transfer after mapping')

                status, repeated = call(visualization_base, '/api/visualization/v1/snapshots/acquire',
                                        dict(request_id=request_id, source_id=source_id))
                assert status == 200 and repeated['snapshot_id'] != snapshot['snapshot_id'], repeated
                status, invalid = call(visualization_base, '/api/visualization/v1/snapshots/acquire',
                                       dict(request_id='not-a-uuid', source_id=source_id))
                assert status == 400 and invalid['code'] == 'INVALID_WORKFLOW', invalid
                passed('repeated request identity reuses one transfer and invalid request is rejected')

                # Compute is bound to one snapshot version and rejects mismatched or expired versions.
                workflow = {"workflow": [{"type": "view", "query": [{"op": "aggregate", "groupBy": [],
                                                                     "measures": [{"field": "*", "agg": "count",
                                                                                   "asFieldKey": "rows"}]}]}]}
                compute = dict(request_id=str(uuid.uuid4()), source_id=source_id,
                               snapshot_id=snapshot['snapshot_id'], schema_revision=snapshot['schema_revision'],
                               workflow_version='graphic-walker-0.5.3', workflow=workflow,
                               detail_mode='complete')
                status, result = call(visualization_base, '/api/visualization/v1/compute', compute)
                assert status == 200 and result['execution'] == 'dataframe', result
                assert result['snapshot_id'] == snapshot['snapshot_id'] and result['returned_rows'] == 1, result
                assert result['rows'][0]['rows'] == 3, result
                mismatch = dict(compute, request_id=str(uuid.uuid4()), schema_revision='0' * 64)
                status, rejected = call(visualization_base, '/api/visualization/v1/compute', mismatch)
                assert status == 409 and rejected['code'] == 'SNAPSHOT_MISMATCH', rejected
                expired_request = dict(compute, request_id=str(uuid.uuid4()), snapshot_id='0' * 64)
                status, expired_compute = call(visualization_base, '/api/visualization/v1/compute', expired_request)
                assert status == 409 and expired_compute['code'] == 'SNAPSHOT_EXPIRED', expired_compute
                cancel = call(visualization_base, '/api/visualization/v1/cancel', dict(request_id=compute['request_id']))
                assert cancel[0] == 200 and cancel[1]['status'] == 'not_running', cancel
                passed('snapshot-bound compute, version mismatch, expiry and cancel')

                status, renewed = call(visualization_base, '/api/visualization/v1/snapshots/renew',
                                       dict(snapshot_id=snapshot['snapshot_id']))
                assert status == 200 and renewed['expires_at_unix'] >= snapshot['expires_at_unix'], renewed
                status, released = call(visualization_base, '/api/visualization/v1/snapshots/release',
                                        dict(snapshot_id=snapshot['snapshot_id']))
                assert status == 200 and released['status'] == 'released', released
                status, expired = call(visualization_base, '/api/visualization/v1/snapshots/renew',
                                       dict(snapshot_id=snapshot['snapshot_id']))
                assert status == 409 and expired['code'] == 'SNAPSHOT_EXPIRED', expired
                status, released = call(visualization_base, '/api/visualization/v1/snapshots/release',
                                        dict(snapshot_id=snapshot['snapshot_id']))
                assert status == 200, released
                passed('renew, idempotent release and expiry rejection')

                # Source-side limits: unknown path, oversize budget, unsafe transfer id, cancel.
                status, unknown = call(source_base, '/channels/dataframe/exploration/v1/snapshots/acquire',
                                       dict(request_id=str(uuid.uuid4()), source_id='0' * 64,
                                            remaining_timeout_ms=5000))
                assert status == 404 and unknown['code'] == 'SOURCE_UNAVAILABLE', unknown
                status, limited = call(source_base, '/channels/dataframe/exploration/v1/snapshots/acquire',
                                       dict(request_id=str(uuid.uuid4()), source_id=source_id, max_bytes=64,
                                            remaining_timeout_ms=5000))
                assert status == 413 and limited['code'] == 'SNAPSHOT_LIMIT', limited
                status, escaped = call(source_base, '/channels/dataframe/exploration/v1/snapshots/release',
                                       dict(transfer_id='../../../../etc/passwd'))
                assert status == 200, escaped
                assert (root / 'etc' / 'passwd').exists() is False
                status, cancelled = call(source_base, '/channels/dataframe/exploration/v1/snapshots/cancel',
                                         dict(request_id='never-started'))
                assert status == 200 and cancelled['status'] == 'not_running', cancelled
                passed('unknown path, byte budget and unsafe identifier are rejected')

                status, second = call(visualization_base, '/api/visualization/v1/snapshots/acquire',
                                      dict(request_id=str(uuid.uuid4()), source_id=source_id))
                assert status == 200, second
            finally:
                visualization_log_text = stop(visualization, visualization_log, 'visualization')

            # The visualization process is gone: no mapping or lease may outlive it.
            assert not list(shared.glob('*.arrow')), list(shared.glob('*.arrow'))
            passed('visualization shutdown releases client state and mapped objects')

            status, snapshot_after = call(source_base, '/channels/dataframe/exploration/v1/snapshots/acquire',
                                          dict(request_id=str(uuid.uuid4()), source_id=source_id,
                                               remaining_timeout_ms=5000))
            assert status == 200, snapshot_after
            transfer_object = shared / (snapshot_after['transfer_id'] + '.arrow')
            assert transfer_object.exists() and stat.S_IMODE(transfer_object.stat().st_mode) == 0o600
            call(source_base, '/channels/dataframe/exploration/v1/snapshots/release',
                 dict(transfer_id=snapshot_after['transfer_id']))
            assert not transfer_object.exists(), 'release must remove the published object'
            passed('source release removes the published object')
        finally:
            source_log_text = stop(source, source_log, 'source')

    # A visualization without a configured data plane must keep the database path usable.
    unavailable_base = f'http://127.0.0.1:{port()}'
    with (root / 'standalone.log').open('w+') as standalone_log:
        standalone = subprocess.Popen([str(output / 'flowsql'), '--role', 'visualization', '--plugins',
                                       visualization_plugins, '--option',
                                       json.dumps(dict(port=int(unavailable_base.rsplit(':', 1)[1]),
                                                       static_dir=str(output / 'exploration')))],
                                      cwd=root, env=env, stdout=standalone_log, stderr=standalone_log)
        try:
            wait_ready(unavailable_base, standalone, standalone_log, 'standalone',
                       '/api/visualization/v1/capabilities')
            status, listing = call(unavailable_base, '/api/visualization/v1/sources')
            assert status == 200 and listing['sources'] == [], listing
            status, unknown = call(unavailable_base, '/api/visualization/v1/snapshots/acquire',
                                   dict(request_id=str(uuid.uuid4()), source_id='0' * 64))
            assert status == 503 and unknown['code'] in ('DATA_PLANE_UNAVAILABLE', 'SOURCE_UNAVAILABLE'), unknown
            passed('missing data plane fails explicitly while database path stays available')
        finally:
            stop(standalone, standalone_log, 'standalone')

    leftovers = [item.name for item in shared.iterdir() if item.name != '.owner.lock']
    assert not leftovers, leftovers
    passed('no leftover objects or mappings in the managed directory')
    print(json.dumps({'checks': checks}))


if __name__ == '__main__':
    main()
