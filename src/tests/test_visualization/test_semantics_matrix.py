# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Four real database drivers bind source/result explanations to their native schemas."""
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import uuid
from test_semantics import ROOT, running, get, compute, meaning, metadata
sys.path.insert(0, str(ROOT / 'tools'))
from test_environment.config import Config


def main():
    output = Path(sys.argv[1]).resolve()
    fixture = output / 'test_visualization_connectors_fixture'
    table = 'vis_e2e_' + uuid.uuid4().hex[:12]
    backends = ['mysql', 'postgres', 'clickhouse']
    prepared = []
    def prepare(action, backend):
        subprocess.run([str(fixture), action, backend, table], check=True, capture_output=True, timeout=30)
    try:
        for backend in backends:
            prepare('seed', backend)
            prepared.append(backend)
        with tempfile.TemporaryDirectory(prefix='flowsql-exploration-semantics-matrix-') as tmp:
            directory = Path(tmp)
            database = directory / 'sqlite.db'
            with sqlite3.connect(database) as db:
                db.execute(f'CREATE TABLE {table}(host TEXT,time TEXT,value REAL)')
                db.executemany(f'INSERT INTO {table} VALUES(?,?,?)', [('a','2024-02-29',10),('a','2024-02-29',None),('b','2024-03-01',20)])
                db.execute(f'CREATE TABLE {table}_statistics(stamp DATE,identity TEXT)')
                db.executemany(f'INSERT INTO {table}_statistics VALUES(?,?)', [('2024-02-29','x'),('2024-03-01','y')])
            entries = [dict(type='sqlite', name='sqlite', path=str(database), readonly='true', dataset_instance_id=uuid.uuid4().hex)]
            entries += [dict(type=backend, name=backend, dataset_instance_id=uuid.uuid4().hex,
                **Config().database(backend, 'driver', os.environ.get('FLOWSQL_TEST_LOCATION', 'native'))) for backend in backends]
            channels = directory / 'channels.json'
            channels.write_text(json.dumps(dict(channels=dict(database_channels=entries))))
            channels.chmod(0o600)
            with running(output, directory, channels) as (control, visual, _, _):
                listing = get(visual, 'sources')['sources']
                for backend in ['sqlite', *backends]:
                    locator = dict(kind='database_table', type=backend, name=backend, table=table)
                    saved = meaning(control, locator, 'value', backend + ' captured delay')
                    def source(relation):
                        item = next(s for s in listing if s['kind']=='database' and s['label'].startswith(backend+'.') and s['label'].endswith('.'+relation) and table+'.schema.' not in s['label'])
                        return get(visual, 'sources/' + item['source_id'])
                    schema = source(table)
                    assert schema['semantic_context']['sources'][0]['basis']['dictionary_ref'] == saved['dictionary_ref']
                    raw = compute(visual, schema, dict(op='raw', fields=['f2','f0']))
                    field = raw['semantic_context']['fields'][0]
                    assert field['state']=='known' and field['result_unit']=='ms' and len(raw['rows'])==3, raw
                    document = raw['semantic_context']['sources'][0]['description']['document']
                    assert next(f for f in document['fields'] if f['field_id']==field['source_field_id'])['name']=='value'
                    for operation in ['count','distinctCount','sum']:
                        result = compute(visual, schema, dict(op='aggregate', groupBy=['f0'], measures=[dict(field='f2',agg=operation,asFieldKey='m')]))
                        assert result['semantic_context']['fields'][1]['operation']['function']==operation.lower()
                        assert result['semantic_context']['fields'][1]['result_unit']==('个' if operation!='sum' else None)
                    histogram = compute(visual, schema, dict(op='histogram',field='f2',bins=4))
                    assert histogram['semantic_context']['fields'][3]['result_unit']=='个'
                    for unit in ['day','month']:
                        result = compute(visual, source(table+'_statistics'), dict(op='aggregate', groupBy=[dict(field='f0',unit=unit,asFieldKey='bucket')], measures=[dict(field='*',agg='count',asFieldKey='n')]))
                        assert result['semantic_context']['fields'][0]['operation']['configuration']['calendar']=='source_calendar'
                        assert result['semantic_context']['fields'][0]['operation']['configuration']['unit']==unit
                    meaning(control, locator, 'value', backend + ' updated delay')
                    assert raw['semantic_context']['sources'][0]['basis']['dictionary_ref']==saved['dictionary_ref']
                    refreshed = compute(visual, schema, dict(op='raw',fields=['f2']))
                    assert refreshed['semantic_context']['sources'][0]['basis']['dictionary_ref']!=saved['dictionary_ref']
                    print('PASS', backend, 'native source IDs, raw/count/distinct/sum, day/month/histogram and captured versions', flush=True)
    finally:
        for backend in reversed(prepared):
            prepare('cleanup', backend)


if __name__=='__main__':main()
