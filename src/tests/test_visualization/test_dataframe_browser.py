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
from pathlib import Path
from test_entry import port, service
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

        source_port, visual_port = port(), port()
        source = f'http://127.0.0.1:{source_port}'
        visual = f'http://127.0.0.1:{visual_port}'
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
                subprocess.run([node, str(Path(__file__).with_name('dataframe_browser.mjs')), visual, str(evidence)],
                               check=True, timeout=240)
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
