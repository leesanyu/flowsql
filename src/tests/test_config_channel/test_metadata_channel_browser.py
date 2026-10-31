# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Object channel workflow: real providers, persistent restart and Chromium mouse/layout assertions."""
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
from test_channel_dictionary_api import running
from test_data_semantics_api import ROOT, request


def main():
    evidence = ROOT / 'evidence/channel-description-refinement/t2-browser'
    evidence.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='flowsql-object-channels-') as temporary:
        directory = Path(temporary)
        (directory / 'frames').mkdir()
        csv = 'id,value,value_extra\n' + ''.join(f'{i},{i * 2.5},{i}\n' for i in range(1, 46))
        (directory / 'frames/sample.csv').write_text(csv)
        (directory / 'frames/disposable.csv').write_text(csv)
        (directory / ('frames/' + 'long_channel_name_' * 5 + '.csv')).write_text(csv)
        with sqlite3.connect(directory / 'source.db') as db:
            db.execute('CREATE TABLE populated(id INTEGER, value REAL, created_at TIMESTAMP)')
            db.execute('INSERT INTO populated VALUES(1,2.5,NULL)')
        with running(Path(sys.argv[1]).resolve(), directory) as (base, _, log, _):
            result = subprocess.run([sys.argv[2], str(Path(__file__).with_name('metadata_channel_browser.mjs')), base, str(evidence)], capture_output=True, text=True, timeout=260)
            (evidence / 'browser.log').write_text(result.stdout + result.stderr)
            (evidence / 'host.log').write_text(log.read_text())
            print(result.stdout + result.stderr)
            assert result.returncode == 0
            references = {}
            for locator in [dict(kind='dataframe',name='sample'), dict(kind='database_table',type='sqlite',name='sample',table='empty_table'),dict(kind='kafka_source',name='events')]:
                status, d = request(base+'/api/semantics/v1/describe', dict(target=dict(kind='channel',locator=locator)))
                assert status == 200, d
                references[json.dumps(locator)] = d['data']['basis']['dictionary_ref']
                assert d['data']['description']['state'] == 'present', d
        with running(Path(sys.argv[1]).resolve(), directory) as (base, _, log, _):
            for encoded, ref in references.items():
                status, d = request(base+'/api/semantics/v1/describe', dict(target=dict(kind='channel',locator=json.loads(encoded))))
                assert status == 200 and d['data']['basis']['dictionary_ref'] == ref, d
                assert d['data']['description']['document'], d
            (evidence / 'restart.log').write_text(log.read_text())
    print('PASS: channel object workflow and precise descriptions survive actual plugin-host restart')


if __name__ == '__main__':
    main()
