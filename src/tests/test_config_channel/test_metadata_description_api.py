# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Object description protocol over actual Config/Web/Gateway/Catalog/Worker plugins."""
import json
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from test_data_semantics_api import ROOT, validate, request
from test_operator_contract_api import running_contract_host, api, bind_input

def call(base, op, payload, expected=200):
    key=op.replace('/','_')
    if expected==200:validate(key+'Request',payload)
    status,value=request(base+'/api/semantics/v1/'+op,payload)
    assert status==expected,(op,status,expected,value)
    validate(key+'Response' if status==200 else 'error',value)
    return value.get('data',value.get('error'))

def describe(base,locator=None,name=None):
    return call(base,'describe',dict(target=dict(kind='operator',name=name) if name else dict(kind='channel',locator=locator)))

def main():
    evidence=ROOT/'evidence/metadata-description-query/implementation/t1';evidence.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='flowsql-description-http-') as tmp:
        directory=Path(tmp)
        with running_contract_host(Path(sys.argv[1]).resolve(),directory) as (base,process,log):
            for filename in ('api.schema.json','openapi.json'):
                assert request(base+'/api/semantics/v1/'+filename)[1]==json.loads((ROOT/'config/semantics'/filename).read_text())
            locator=dict(kind='dataframe',name='input')
            d=describe(base,locator);assert d['structure']['fields'] and d['description']['state']=='missing' and d['checks']['state']=='unbound'
            assert d['checks']['data_validation']=='not_run'
            pending=describe(base,dict(kind='dataframe',name='lazy'));assert pending['structure']['state']=='pending'
            ref=bind_input(base);d=describe(base,locator);assert d['basis']['dictionary_ref']==ref and d['description']['state']=='present'
            for name in ('builtin.passthrough','explore.baseliner','contractdemo.output'):
                d=describe(base,name=name);assert d['contract']['state']=='declared' and d['description']['origin']=='component_author'
                assert d['basis']['component_revision'] and d['contract']['document']['inputs']
                if name=='explore.baseliner':assert len(d['contract']['document']['outputs'])==4 and d['contract']['document']['resources']
            for name in ('builtin.passthrough','contractdemo.output'):
                before=describe(base,name=name)
                notes=dict(purpose='HTTP purpose',usage_notes='Read the actual contract',examples=['SELECT * FROM dataframe.input USING '+name])
                payload=dict(name=name,expected_component_revision=before['basis']['component_revision'],expected_description_ref=before['basis']['description_ref'],notes=notes)
                saved=call(base,'operators/save-description',payload)
                assert saved['created_revision'] and saved['description_ref']
                assert not call(base,'operators/save-description',payload)['created_revision']
                after=describe(base,name=name)
                assert after['description']['notes']==notes and after['contract']==before['contract']
                payload['notes']=dict(notes,purpose='stale purpose')
                assert call(base,'operators/save-description',payload,409)['code']=='REVISION_CONFLICT'
                payload['expected_component_revision']='changed'
                assert call(base,'operators/save-description',payload,409)['code']=='SOURCE_CHANGED'
            table=describe(base,dict(kind='database_table',type='sqlite',name='example',table='one'));assert table['structure']['state']=='known'
            call(base,'describe',dict(target=dict(kind='operator',name='absent.absent')),404)
            call(base,'describe',dict(target=dict(kind='channel',locator=dict(kind='dataframe',name='absent'))),404)
            assert describe(base,dict(kind='dataframe',name='lazy'))['structure']['state']=='pending'
            # Object save needs no client-generated name, field ID or mapping.
            for locator in (dict(kind='dataframe',name='input'),dict(kind='database_table',type='sqlite',name='example',table='one')):
                before=describe(base,locator)
                field=before['structure']['fields'][0]['name']
                meaning=dict(state='declared',origin='manual',value='HTTP object meaning')
                payload=dict(locator=locator,basis=before['basis'],changes=dict(fields={field:dict(definition=meaning)}),confirmations=before['checks'].get('required_confirmations',[]))
                saved=call(base,'channels/save-description',payload)
                assert saved['status']=='applied' and saved['dictionary_ref'] and saved['binding_ref']
                assert call(base,'channels/save-description',payload)['binding_ref']==saved['binding_ref']
                current=describe(base,locator)
                assert current['basis']==saved['basis'] and current['description']['document']['fields'][0]['definition']==meaning
                if locator['kind']=='dataframe':assert current['description']['document']['fields'][0]['unit']['value']['code']=='ms'
                payload['changes']['fields'][field]['definition']['value']='stale write'
                assert call(base,'channels/save-description',payload,409)['code']=='REVISION_CONFLICT'
            assert describe(base,dict(kind='dataframe',name='lazy'))['structure']['state']=='pending'
            # The simplified client sends no SemanticValue wrappers or field identities.
            for locator in (dict(kind='dataframe',name='input'),dict(kind='database_table',type='sqlite',name='example',table='one')):
                before=describe(base,locator)
                field=before['structure']['fields'][0]['name']
                payload=dict(locator=locator,basis=before['basis'],editor_changes=dict(fields={field:dict(definition='Editor meaning')}),confirmations=before['checks'].get('required_confirmations',[]))
                saved=call(base,'channels/save-description',payload)
                current=describe(base,locator)
                expected=json.loads(json.dumps(before['description']['document']))
                expected['fields'][0]['definition']=dict(state='declared',origin='manual',value='Editor meaning')
                assert current['description']['document']==expected
                assert call(base,'channels/save-description',payload)['binding_ref']==saved['binding_ref']
                for bad in (dict(changes={}),dict(editor_changes={'fields':{field:{'type':{}}}})):
                    assert call(base,'channels/save-description',dict(payload,**bad),400)['code']=='INVALID_DOCUMENT'
                cleared=call(base,'channels/save-description',dict(locator=locator,basis=current['basis'],editor_changes={'fields':{field:{'definition':None}}},confirmations=current['checks'].get('required_confirmations',[])))
                assert describe(base,locator)['description']['document']['fields'][0]['definition']==dict(state='unknown',origin='manual',value=None)
                assert cleared['status']=='applied'
            # New time information is consumed by the existing F3 preflight/output derivation.
            locator=dict(kind='dataframe',name='input');before=describe(base,locator)
            field=before['structure']['fields'][0]['name']
            saved=call(base,'channels/save-description',dict(locator=locator,basis=before['basis'],editor_changes={'fields':{field:{'time':{'mode':'duration','unit':'ms'}}}},confirmations=before['checks'].get('required_confirmations',[])))
            from test_operator_contract_api import preflight
            checked=preflight(base,'builtin.passthrough')
            assert checked['check']['state']=='compatible',checked
            output=checked['check']['output_previews'][0]['dictionary']
            assert output['fields'][0]['time_semantics']['value']['kind']=='duration',output
            assert output['fields'][0]['unit']['value']['code']=='ms'
            # First source save and exact historical target relations use the same public HTTP contract.
            target=describe(base,locator);target_ref=target['basis']['dictionary_ref'];target_field=target['description']['document']['fields'][0]['field_id']
            source_locator=dict(kind='database_table',type='sqlite',name='example',table='one');source=describe(base,source_locator)
            source_field=source['structure']['fields'][0]['name']
            saved=call(base,'channels/save-description',dict(locator=source_locator,basis=source['basis'],editor_changes={'relations':[{'dictionary_ref':target_ref,'field_map':{source_field:target_field}}]},confirmations=source['checks'].get('required_confirmations',[])))
            linked=describe(base,source_locator);relation=linked['description']['document']['entity']['relations']['value'][0]
            assert relation['dictionary_ref']==target_ref and list(relation['field_map'].values())==[target_field]
            assert describe(base,locator)['basis']['dictionary_ref']==target_ref
            for mapping in ({source_field:'absent'}, {'absent':target_field}):
                invalid=call(base,'channels/save-description',dict(locator=source_locator,basis=linked['basis'],editor_changes={'relations':[{'dictionary_ref':target_ref,'field_map':mapping}]}),400)
                assert invalid['field_path'].startswith('/editor_changes/relations/')
            # The complete Web -> Gateway -> Router budget includes forwarding time.
            observed=[]
            class SilentPeer(BaseHTTPRequestHandler):
                def do_POST(self):
                    body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                    observed.append((body['timeout_ms'],int(self.headers['X-FlowSQL-Metadata-Deadline-Ms'])))
                    time.sleep(.3)
                    try:
                        self.send_response(200);self.end_headers();self.wfile.write(b'{}')
                    except (BrokenPipeError,ConnectionResetError):pass
                def log_message(self,*args):pass
            # Host options record the dynamically selected Gateway port in its local log.
            gateway=int(process.args[2])
            with ThreadingHTTPServer(('127.0.0.1',0),SilentPeer) as peer:
                thread=threading.Thread(target=peer.serve_forever,kwargs={'poll_interval':.01});thread.start()
                try:
                    assert request('http://127.0.0.1:'+str(gateway)+'/gateway/register',dict(prefix='/semantics/v1/describe',address='127.0.0.1:'+str(peer.server_port)))[0]==200
                    wall=int(time.time()*1000);started=time.monotonic()
                    err=call(base,'describe',dict(target=dict(kind='operator',name='builtin.passthrough'),timeout_ms=80),504)
                    assert err['code']=='SOURCE_TIMEOUT' and time.monotonic()-started<.35
                    assert observed and 0<observed[0][0]<=80 and observed[0][1]<=wall+90
                finally:peer.shutdown();thread.join()
            (evidence/'description.json').write_text(json.dumps(d,ensure_ascii=False,indent=2));(evidence/'host.log').write_text(log.read_text())
    print('PASS: actual HTTP published schema, object discovery, known/unbound/pending, current operator contract/context, unknown 404')
if __name__=='__main__':main()
