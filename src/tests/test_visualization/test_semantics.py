# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Configured HTTP descriptions bound to real source schemas, results and immutable IPC snapshots."""
import contextlib
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import time
import uuid
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from test_entry import port, request, service
from test_compute import call

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'src/tests/test_config_channel'))
from test_data_semantics_api import request as metadata_request, validate


def metadata(base, operation, payload):
    status, result = metadata_request(base + '/semantics/' + operation, payload)
    assert status == 200, (operation, status, result)
    return result['data']


def meaning(control, locator, field, definition):
    observed = metadata(control, 'describe', dict(target=dict(kind='channel', locator=locator)))
    return metadata(control, 'channels/save-description', dict(locator=locator, basis=observed['basis'],
        editor_changes=dict(fields={field: dict(definition=definition, unit='ms')}),
        confirmations=observed['checks']['required_confirmations']))


@contextlib.contextmanager
def visualization(output, directory, control, channels, shared, **extra):
    visual = f'http://127.0.0.1:{port()}'
    options = dict(port=int(visual.rsplit(':', 1)[1]), static_dir=str(output / 'exploration'),
        semantics_upstream=control, dataframe_upstream=control, dataframe_shared_dir=str(shared), **extra)
    plugins = f'{output}/libflowsql_database.so:config_file={channels},{output}/libflowsql_visualization.so'
    with (directory / ('visual-' + str(port()) + '.log')).open('w+') as log:
        process = subprocess.Popen([str(output / 'flowsql'), '--role', 'visualization', '--plugins', plugins,
            '--option', json.dumps(options)], cwd=directory, env=dict(os.environ,
            LD_LIBRARY_PATH=str(output) + ':' + os.environ.get('LD_LIBRARY_PATH', '')), stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 15
            while True:
                try:
                    if call(visual, 'capabilities')[0] == 200: break
                except OSError: pass
                assert process.poll() is None and time.monotonic() < deadline, 'visualization startup failed'
                time.sleep(.05)
            yield visual
        finally:
            process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait(); raise AssertionError('visualization shutdown exceeded budget')
            assert process.returncode == 0


@contextlib.contextmanager
def running(output, directory, channels=None):
    shared = directory / 'shared'; shared.mkdir(mode=0o700)
    frames = directory / 'frames'; frames.mkdir()
    (frames / 'sales.csv').write_text('stamp,value,host,cost\n2024-02-29,10,a,1\n2024-03-01,20,b,2\n2024-03-01,5,a,3\n')
    (frames / 'pending.csv').write_text('value\n5\n')
    if channels is None:
        database = directory / 'data.db'
        with sqlite3.connect(database) as db:
            db.execute('CREATE TABLE rows(stamp TEXT,value REAL,host TEXT,cost REAL)')
            db.executemany('INSERT INTO rows VALUES(?,?,?,?)', [('2024-02-29',10,'a',1),('2024-03-01',20,'b',2),('2024-03-01',5,'a',3)])
        channels = directory / 'channels.json'
        channels.write_text(json.dumps(dict(channels=dict(database_channels=[dict(type='sqlite',name='memory',
            path=str(database),readonly='true',dataset_instance_id=uuid.uuid4().hex)]))))
        channels.chmod(0o600)
    control = f'http://127.0.0.1:{port()}'
    plugins = (f'{output}/libflowsql_config_channel.so:db_path={directory}/semantics.db,'
               f'{output}/libflowsql_builtin.so,{output}/libflowsql_catalog.so:data_dir={frames};'
               f'operator_db_path={directory}/operators.db;exploration_shared_dir={shared},'
               f'{output}/libflowsql_database.so:config_file={channels},'
               f'{output}/libflowsql_router.so:host=127.0.0.1;port={control.rsplit(":",1)[1]}')
    with (directory / 'control.log').open('w+') as log:
        process = subprocess.Popen([str(output / 'flowsql'), '--role', 'catalog', '--plugins', plugins], cwd=directory,
            env=dict(os.environ,LD_LIBRARY_PATH=str(output)+':'+os.environ.get('LD_LIBRARY_PATH','')), stdout=log, stderr=log)
        try:
            deadline=time.monotonic()+20
            while True:
                try:
                    with request(control+'/channels/dataframe/exploration/sources') as response:
                        if response.status == 200: break
                except OSError: pass
                assert process.poll() is None and time.monotonic()<deadline, 'control startup failed'
                time.sleep(.05)
            with visualization(output,directory,control,channels,shared) as visual:
                yield control, visual, channels, shared
        finally:
            process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill();process.wait();raise AssertionError('control shutdown exceeded budget')
            assert process.returncode == 0


def get(base,path):
    status,result=call(base,path)
    assert status==200,(path,status,result)
    return result


def compute(base,source,query,**options):
    payload=dict(request_id=str(uuid.uuid4()),source_id=source['source_id'],schema_revision=source['schema_revision'],
        workflow_version='graphic-walker-0.5.3',detail_mode='complete',
        workflow=dict(workflow=[dict(type='view',query=[query])]),**options)
    if source.get('snapshot_id'):payload['snapshot_id']=source['snapshot_id']
    status,result=call(base,'compute',payload)
    assert status==200,(status,result)
    validate('semanticContext',result['semantic_context'])
    assert [f['output_name'] for f in result['semantic_context']['fields']]==[f['field_id'] for f in result['fields']]
    return result


def main():
    output=Path(sys.argv[1]).resolve()
    evidence=ROOT/'evidence/data-semantics-consumption/exploration-server';evidence.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='flowsql-exploration-semantics-') as tmp:
        directory=Path(tmp)
        with running(output,directory) as (control,visual,channels,shared):
            locator=dict(kind='database_table',type='sqlite',name='memory',table='rows')
            unbound=next(s for s in get(visual,'sources')['sources'] if s['kind']=='database')
            undeclared=get(visual,'sources/'+unbound['source_id'])['semantic_context']
            assert undeclared['state']=='unknown' and 'error' not in undeclared
            saved=meaning(control,locator,'value','Captured database latency')
            listing=get(visual,'sources')['sources'];items={s['label']:s for s in listing}
            source=get(visual,'sources/'+items['memory.rows']['source_id'])
            context=source['semantic_context'];validate('semanticContext',context)
            assert context['sources'][0]['basis']['dictionary_ref']==saved['dictionary_ref']
            assert context['fields'][1]['state']=='known'
            raw=compute(visual,source,dict(op='raw',fields=['f1','f0']))
            assert raw['semantic_context']['fields'][0]['result_unit']=='ms' and len(raw['rows'])==3
            for operation in ['count','distinctCount','sum','mean','min','max']:
                result=compute(visual,source,dict(op='aggregate',groupBy=[],measures=[dict(field='f1',agg=operation,asFieldKey='m')]))
                field=result['semantic_context']['fields'][0]
                assert field['operation']['function'].lower()==operation.lower()
                assert field['result_unit']==('个' if operation in ['count','distinctCount'] else None)
            for unit in ['day','month']:
                result=compute(visual,source,dict(op='aggregate',groupBy=[dict(field='f0',unit=unit,asFieldKey='bucket')],
                    measures=[dict(field='*',agg='count',asFieldKey='n')]))
                assert result['semantic_context']['fields'][0]['operation']['kind']=='time_bucket'
                assert result['semantic_context']['fields'][0]['operation']['configuration']['unit']==unit
            histogram=compute(visual,source,dict(op='histogram',field='f1',bins=5))
            assert all(f['operation']['kind']=='histogram' for f in histogram['semantic_context']['fields'])
            assert histogram['semantic_context']['fields'][3]['result_unit']=='个'
            frame_locator=dict(kind='dataframe',name='sales')
            status,loaded=metadata_request(control+'/channels/dataframe/preview',dict(category='dataframe',name='sales',page=1,page_size=1))
            assert status==200,loaded
            first=meaning(control,frame_locator,'value','Old captured frame meaning')
            def acquire():
                status,snapshot=call(visual,'snapshots/acquire',dict(request_id=str(uuid.uuid4()),source_id=items['dataframe.sales']['source_id']))
                assert status==200,snapshot
                validate('semanticContext',snapshot['semantic_context'])
                return snapshot
            old=acquire();old_context=old['semantic_context']
            second=meaning(control,frame_locator,'value','New captured frame meaning')
            old_result=compute(visual,old,dict(op='raw',fields=['f1']))
            assert old_result['semantic_context']['sources']==old_context['sources']
            assert old_result['semantic_context']['sources'][0]['basis']['dictionary_ref']==first['dictionary_ref']
            new=acquire()
            assert new['semantic_context']['sources'][0]['basis']['dictionary_ref']==second['dictionary_ref']
            assert new['snapshot_id']!=old['snapshot_id']
            for query in [dict(op='aggregate',groupBy=['f2'],measures=[dict(field='f1',agg='sum',asFieldKey='total'),dict(field='*',agg='count',asFieldKey='n')]),dict(op='histogram',field='f1',bins=4)]:
                derived=compute(visual,old,query)
                assert derived['semantic_context']['sources']==old_context['sources']
                assert derived['semantic_context']['fields'][-1]['result_unit']=='个'
            # CSV string dates are not promoted to temporal values by reading metadata.
            invalid=dict(request_id=str(uuid.uuid4()),source_id=old['source_id'],schema_revision=old['schema_revision'],snapshot_id=old['snapshot_id'],
                workflow_version='graphic-walker-0.5.3',workflow=dict(workflow=[dict(type='view',query=[dict(op='aggregate',groupBy=[dict(field='f0',unit='day',asFieldKey='d')],measures=[])])]))
            status,rejected=call(visual,'compute',invalid)
            assert status==400 and rejected['code']=='UNSUPPORTED_TYPE'
            # No configured metadata service: successful rows remain available and error is explicit.
            with visualization(output,directory,'',channels,shared) as unavailable:
                item=get(unavailable,'sources')['sources'][0]
                unknown=get(unavailable,'sources/'+item['source_id'])
                result=compute(unavailable,unknown,dict(op='raw',fields=['f1']))
                assert len(result['rows'])==3 and result['semantic_context']['state']=='error'
            class InvalidDescription(BaseHTTPRequestHandler):
                def do_POST(self):
                    self.send_response(200); self.send_header('Content-Type','application/json'); self.end_headers()
                    self.wfile.write(json.dumps(self.payload).encode())
                payload=dict(data={})
                def log_message(self, *args): pass
            server=ThreadingHTTPServer(('127.0.0.1',0),InvalidDescription)
            worker=threading.Thread(target=server.serve_forever,daemon=True);worker.start()
            try:
                with visualization(output,directory,'http://127.0.0.1:'+str(server.server_port),channels,shared) as invalid:
                    item=next(s for s in get(invalid,'sources')['sources'] if s['kind']=='database')
                    schema=get(invalid,'sources/'+item['source_id'])
                    valid=metadata(control,'describe',dict(target=dict(kind='channel',locator=locator)))
                    wrong_fields=json.loads(json.dumps(valid));wrong_fields['description']['document']['fields']=[42]
                    wrong_target=json.loads(json.dumps(valid));wrong_target['subject']['locator']['table']='different_table'
                    for payload in [dict(data={}),dict(data=wrong_fields),dict(data=wrong_target)]:
                        InvalidDescription.payload=payload
                        result=compute(invalid,schema,dict(op='raw',fields=['f1']))
                        assert len(result['rows'])==3 and result['semantic_context']['error']['code']=='INVALID_DOCUMENT'
            finally:
                server.shutdown();server.server_close();worker.join(timeout=2)
            with visualization(output,directory,control,channels,shared,limits=dict(max_bytes=4096)) as bounded:
                item=next(s for s in get(bounded,'sources')['sources'] if s['kind']=='database')
                schema=get(bounded,'sources/'+item['source_id'])
                result=compute(bounded,schema,dict(op='raw',fields=['f1']))
                assert len(result['rows'])==3 and result['semantic_context']['error']['code']=='LIMIT_EXCEEDED'
            (evidence/'contexts.json').write_text(json.dumps(dict(database=raw['semantic_context'],old=old_context,new=new['semantic_context']),ensure_ascii=False,indent=2))
        (evidence/'control.log').write_text((directory/'control.log').read_text())
    print('PASS actual HTTP source/result mapping, aggregate/time/histogram, immutable old/new snapshots, error and metadata budget preserve rows')


if __name__=='__main__':main()
