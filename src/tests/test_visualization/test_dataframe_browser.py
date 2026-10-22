# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Browser acceptance: a DataFrame work page bound to one snapshot version."""
import json
import os
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.request
import uuid
from pathlib import Path
from test_entry import port, request, service
from test_compute import call


def main():
    output, node = Path(sys.argv[1]).resolve(), sys.argv[2]
    evidence = Path(__file__).resolve().parents[3] / 'evidence/visualization-dataframe/ui'
    evidence.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='flowsql-dataframe-browser-') as temporary:
        directory = Path(temporary)
        shared = directory / 'shared'
        shared.mkdir(mode=0o700)
        os.chmod(shared, 0o700)
        dataframes = directory / 'dataframes'
        dataframes.mkdir()
        (dataframes / 'sales.csv').write_text(
            'region,channel,amount,cost\nnorth,web,10,1.5\nsouth,app,20,2.5\nnorth,app,5,3.5\n')
        database = directory / 'data.db'
        with sqlite3.connect(database) as connection:
            connection.execute('CREATE TABLE rows(region TEXT, amount INTEGER)')
            connection.executemany('INSERT INTO rows VALUES(?,?)', [('north', 10), ('south', 20)])

        source_port, visual_port, web_port = port(), port(), port()
        source = f'http://127.0.0.1:{source_port}'
        visual = f'http://127.0.0.1:{visual_port}'
        web = f'http://127.0.0.1:{web_port}'
        env = dict(os.environ, LD_LIBRARY_PATH=str(output) + ':' + os.environ.get('LD_LIBRARY_PATH', ''))
        source_option = f'data_dir={dataframes};exploration_shared_dir={shared};operator_db_path={directory}/meta.db'
        plugins = f'{output}/libflowsql_database.so,{output}/libflowsql_visualization.so'
        database_plugins = f'{output}/libflowsql_database.so:type=sqlite;name=memory;path={database};readonly=true,{output}/libflowsql_visualization.so'
        options = json.dumps(dict(port=visual_port, static_dir=str(output / 'exploration'),
                                  dataframe_upstream=source, dataframe_shared_dir=str(shared)))
        with (evidence / 'source.log').open('w+') as source_log, (evidence / 'service.log').open('w+') as log:
            source_process = subprocess.Popen(
                [str(output / 'flowsql'), '--role', 'catalog', '--plugins',
                 f'{output}/libflowsql_builtin.so,{output}/libflowsql_catalog.so:{source_option},'
                 f'{output}/libflowsql_router.so:host=127.0.0.1;port={source_port}'],
                cwd=directory, env=env, stdout=source_log, stderr=source_log)
            process = subprocess.Popen([str(output / 'flowsql'), '--role', 'visualization', '--plugins',
                                        database_plugins, '--option', options],
                                       cwd=directory, env=env, stdout=log, stderr=log)
            try:
                for base, label in ((source, 'catalog'), (visual, 'visualization')):
                    deadline = time.monotonic() + 20
                    while True:
                        try:
                            probe = '/api/visualization/v1/capabilities' if label == 'visualization' else \
                                '/channels/dataframe/exploration/v1/sources'
                            with urllib.request.urlopen(base + probe, timeout=5) as response:
                                if response.status == 200:
                                    break
                        except OSError:
                            pass
                        if time.monotonic() >= deadline or (source_process if label == 'catalog' else process).poll() is not None:
                            raise AssertionError(f'{label} did not start')
                        time.sleep(.05)
                status, listing = call(visual, 'sources')
                assert status == 200, listing
                labels = [item['label'] for item in listing['sources']]
                assert 'dataframe.sales' in labels and 'memory.rows' in labels, labels
                web_options = (f'port={web_port};db_path={directory}/web.db;'
                               f'upload_dir={directory}/uploads;visualization_upstream={visual}')
                with service(output, directory, 'web', web_options, web + '/api/version'):
                    # Missing/invalid bodies must reach the exploration service as JSON errors.
                    acquire_url = web + '/api/visualization/v1/snapshots/acquire'
                    with request(urllib.request.Request(acquire_url, data=b'{}',
                                 headers={'Content-Type': 'application/json'})) as response:
                        assert response.status == 400, (response.status, response.read().decode())
                        assert response.headers.get_content_type() == 'application/json'
                        assert json.load(response)['code'] == 'INVALID_WORKFLOW'
                    for operation in ('acquire', 'renew', 'release'):
                        url = web + '/api/visualization/v1/snapshots/' + operation
                        for method in ('GET', 'HEAD', 'DELETE'):
                            with request(urllib.request.Request(url, method=method)) as response:
                                assert response.status == 405, (operation, method, response.status)
                    with request(urllib.request.Request(web + '/api/visualization/v1/snapshots/unknown',
                                                       data=b'{}')) as response:
                        assert response.status == 405, response.status
                    status, proxied = call(web, 'sources')
                    assert status == 200, proxied
                    dataframe = next(item for item in proxied['sources'] if item['label'] == 'dataframe.sales')
                    status, snapshot = call(web, 'snapshots/acquire',
                                            {'request_id': str(uuid.uuid4()), 'source_id': dataframe['source_id']})
                    assert status == 200 and snapshot['row_count'] == 3, snapshot
                    binding = {'snapshot_id': snapshot['snapshot_id']}
                    try:
                        status, renewed = call(web, 'snapshots/renew', binding)
                        assert status == 200 and renewed['expires_at_unix'] > 0, renewed
                    finally:
                        status, released = call(web, 'snapshots/release', binding)
                        assert status == 200, released
                    status, expired = call(web, 'snapshots/renew', binding)
                    assert status == 409 and expired['code'] == 'SNAPSHOT_EXPIRED', expired
                    (evidence / 'proxy-http.json').write_text(json.dumps({
                        'invalid_body': 400, 'wrong_methods': 405, 'unknown_operation': 405,
                        'acquire': snapshot, 'renew': renewed, 'release': released, 'expired': expired,
                    }, indent=2))
                    for mode, base in (('standalone', visual), ('embedded', web)):
                        browser_evidence = evidence / mode
                        browser_evidence.mkdir(exist_ok=True)
                        subprocess.run([node, str(Path(__file__).with_name('dataframe_browser.mjs')),
                                        base, str(browser_evidence), mode], check=True, timeout=240)
            finally:
                for child in (process, source_process):
                    child.terminate()
                    try:
                        child.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        child.kill()
                        child.wait()
        # The work page never leaves a published object behind in this fixture.
        assert not [name for name in os.listdir(shared) if name != '.owner.lock'], os.listdir(shared)


if __name__ == '__main__':
    main()
