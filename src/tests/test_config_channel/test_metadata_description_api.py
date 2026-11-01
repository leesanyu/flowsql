# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Object description protocol over actual Config/Web/Gateway/Catalog/Worker plugins."""
import json
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
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

def describe(base,locator=None,name=None,view=None):
    payload=dict(target=dict(kind='operator',name=name) if name else dict(kind='channel',locator=locator))
    if view is not None:payload['view']=view
    return call(base,'describe',payload)

def empty_notes(**changes):
    return dict(dict(purpose='',usage_notes='',examples=[],inputs=[],parameters=[],outputs=[]),**changes)


def structured_notes():
    return empty_notes(
        inputs=[dict(name='latency',type='double',requirement='required_non_null',description='Primary input: latency in milliseconds; nonempty rows')],
        parameters=[dict(name='column',type='string',required=None,default='""',description='Pass an existing column name as a string')],
        outputs=[dict(name='result',type='',nullable=False,description='Results output: produced on success; preserves the declared units')])


def check_usage(base,evidence):
    source=Path(__file__).with_name('fixtures').joinpath('description_chisquare.py').read_text()
    api(base,'operators/upload',dict(type='python',filename='explore_chisquare.py',content=source))
    contract=json.loads((ROOT/'config/semantics/examples/operator-contract-passthrough.json').read_text())
    contract['operator']=dict(category='usage',name='simple',version='1')
    simple=('from flowsql import OperatorBase, register_operator\n'
            '@register_operator(category="usage",name="simple",data_contract='+repr(contract)+')\n'
            'class Simple(OperatorBase):\n    def work(self,df): return df\n')
    api(base,'operators/upload',dict(type='python',filename='usage_simple.py',content=simple))
    sizes={}
    registry_before=call(base,'list',{})
    for name in ('builtin.passthrough','contractdemo.output','usage.simple','explore.chisquare','explore.baseliner','npm.basic'):
        data=describe(base,name=name)
        explicit=describe(base,name=name,view='usage')
        full=describe(base,name=name,view='full')
        assert data==explicit and data['view']=='usage' and data['contract_state']=='declared'
        assert data['usage']['annotations']==dict(inputs=[],parameters=[],outputs=[])
        assert set(data['usage'])=={'identity','inputs','parameters','outputs','limitations','examples','annotations'}
        assert not {'contract','context','resources','normalized_config_schema'} & set(data)
        assert data['basis']==full['basis']
        doc=full['contract']['document']
        assert data['usage']['identity']['name']==name
        assert data['usage']['identity']['title']==doc['title']
        def pointer(path):
            v=doc
            for token in path.lstrip('/').split('/'):
                key=token.replace('~1','/').replace('~0','~')
                v=v[int(key)] if isinstance(v,list) else v[key]
            return v
        seen=set()
        def expand(path):
            if path in seen:return
            seen.add(path)
            resource=call(base,'resolve',dict(exact_reference=data['basis']['contract_ref'],view='resource',resource_pointer=path))
            assert resource['resource']==pointer(path)
            assert resource['exact_reference']==data['basis']['contract_ref']
            def visit(value):
                if isinstance(value,dict):
                    for key,item in value.items():
                        if key=='$ref' and isinstance(item,str) and item.startswith('#/'):expand(item[1:])
                        else:visit(item)
                elif isinstance(value,list):
                    for item in value:visit(item)
            visit(resource['resource'])
        for item in data['details']:
            assert item['exact_reference']==data['basis']['contract_ref']
            expand(item['resource_pointer'])
        for p in data['usage']['parameters']['items']:
            assert p['schema']==pointer(p['source_pointer'])
        for o in data['usage']['outputs']:
            for f in o['fields']:assert f['declaration']==pointer(f['source_pointer'])
        for original, projected in zip(doc['inputs'], data['usage']['inputs']):
            assert projected.get('description') == original.get('description')
        assert data['usage']['parameters'].get('description') == doc['configuration'].get('description')
        assert [p['text'] for p in data['usage']['identity']['purposes'] if p['origin']=='author'] == ([doc['description']] if 'description' in doc else [])
        for original, projected in zip(doc['outputs'], data['usage']['outputs']):
            assert projected.get('description') == original.get('description')
        if name=='npm.basic':
            assert len(doc['outputs'])==2 and len(doc['outputs'][0]['variants'])==13
            assert len(doc['resources']['managed_results']['entities'])==12
        if name=='explore.baseliner':
            params={p['name']:p for p in data['usage']['parameters']['items']}
            assert params['config']['required'] is None and params['parameters']['required'] is None
            assert params['parameters']['wire_encoding']=='json' and params['parameters']['control']=='json'
            assert data['usage']['parameters']['constraints']
            outputs=data['usage']['outputs'];assert len(outputs)==4
            assert len(outputs[0]['row_cases'][1]['field_rules'])==9
            assert outputs[0]['row_cases'][1]['field_rules']['observed']['must_be_null']
            assert outputs[0]['context_semantics'] and outputs[1]['availability']
        if name=='explore.chisquare':
            assert all(p['control']=='field' and p['input_roles'] for p in data['usage']['parameters']['items'])
            assert data['usage']['outputs'][0]['fields_state']=='dynamic'
            assert 'NULL' in data['usage']['outputs'][0]['summary']
            assert data['usage']['inputs'][0]['roles'][0]['types']==[]
            assert data['usage']['inputs'][0]['roles'][0]['nullable'] is None
        def wire_size(view):
            payload=dict(target=dict(kind='operator',name=name),view=view)
            req=urllib.request.Request(base+'/api/semantics/v1/describe',data=json.dumps(payload).encode(),
                                       headers={'Content-Type':'application/json'})
            with urllib.request.urlopen(req,timeout=15) as response:raw=response.read()
            validate('describeResponse',json.loads(raw))
            return len(raw)
        sizes[name]=dict(usage_bytes=wire_size('usage'),full_bytes=wire_size('full'),details=len(seen))
    assert call(base,'list',{})==registry_before  # Reading does not publish registry revisions.
    # A regular client constructs a string-only check after reading usage and its exact rules.
    supplied={'force_fail':'','delay_ms':'0'}
    checked=call(base,'operators/check',dict(operator=dict(category='builtin',name='passthrough'),
                 parameters=supplied,inputs=[dict(input_index=0,locator=dict(kind='dataframe',name='input'))]))
    assert checked['resolved_invocation']['effective_parameters']==supplied
    assert checked['check']['data_validation']=='not_run' and checked['check']['output_check']=='not_observed'
    legacy=simple.replace(',data_contract='+repr(contract),'').replace('name="simple"','name="missing"').replace('class Simple','class Missing')
    api(base,'operators/upload',dict(type='python',filename='usage_missing.py',content=legacy))
    missing=describe(base,name='usage.missing')
    assert missing['contract_state']=='missing' and missing['basis']['contract_ref'] is None and missing['gaps']
    for payload in (dict(target=dict(kind='operator',name='builtin.passthrough'),view='bogus'),
                    dict(target=dict(kind='channel',locator=dict(kind='dataframe',name='input')),view='full')):
        assert call(base,'describe',payload,400)['code']=='INVALID_DOCUMENT'
    call(base,'operators/save-description',dict(name='usage.missing',expected_component_revision=missing['basis']['component_revision'],
         expected_description_ref=None,notes=structured_notes()))
    annotated=describe(base,name='usage.missing')
    assert annotated['contract_state']=='missing' and annotated['basis']['contract_ref'] is None
    assert annotated['usage']['annotations']=={key:structured_notes()[key] for key in ('inputs','parameters','outputs')}
    unknown=call(base,'operators/check',dict(operator=dict(category='usage',name='missing'),parameters={},inputs=[dict(input_index=0,locator=dict(kind='dataframe',name='input'))]))
    assert unknown['check']['state']=='undetermined' and unknown['check']['contract_state']=='missing'
    input_row=structured_notes()['inputs'][0]
    output_row=structured_notes()['outputs'][0]
    parameter=structured_notes()['parameters'][0]
    invalid_rows = [dict(inputs='old text'), dict(parameters=[{'name':'missing required properties'}]),
                    dict(outputs=[dict(output_row,nullable='yes')]),
                    dict(inputs=[dict(input_row,description='x'*30001)]),
                    dict(inputs=[dict(input_row,requirement='required')]),
                    dict(inputs=[dict(name='primary',description='',fields=[input_row])]),
                    dict(outputs=[dict(name='results',description='',fields=[output_row])]),
                    dict(inputs=[dict(input_row,port='input')]),
                    dict(parameters=[dict(parameter,default=None)]),dict(parameters=[dict(parameter,encoding='json')]),
                    dict(parameters=[dict(parameter,type='object')]),dict(parameters=[dict(parameter,default=42)]),
                    dict(inputs=[input_row]*101),dict(outputs=[output_row]*101),dict(parameters=[parameter]*101),
                    dict(input_description=42),dict(parameter_description=None),dict(output_description='x'*30001),
                    dict(input_requirements='removed legacy field')]
    for invalid in invalid_rows:
        call(base,'operators/save-description',dict(name='usage.missing',expected_component_revision=annotated['basis']['component_revision'],
             expected_description_ref=annotated['basis']['description_ref'],notes=dict(structured_notes(),**invalid)),400)
    # Field order, duplicate names, textual ownership and defaults survive exactly.
    edited=structured_notes()
    edited['parameters'].append(dict(edited['parameters'][0],name='optional',default='',required=False))
    edited['parameters'].append(dict(name='settings',type='json',required=True,default='{}',description='JSON object'))
    edited['inputs'].extend([dict(name='note',type='string',requirement='required_nullable',description=''),dict(name='extra',type='',requirement='optional',description=''),dict(name='unknown',type='',requirement='',description='')])
    edited['inputs'].append(dict(input_row,requirement='optional',description='Reference input: latency in milliseconds'))
    edited['outputs'].append(dict(output_row,nullable=None,description='Model output: only when enabled; fields depend on configuration'))
    edited['usage_notes']='All inputs must be ordered by time'
    edited.update(input_description='Input selection is defined by configuration',
                  parameter_description='',output_description='Results and conditional model output')
    call(base,'operators/save-description',dict(name='usage.missing',expected_component_revision=annotated['basis']['component_revision'],
         expected_description_ref=annotated['basis']['description_ref'],notes=edited))
    current=describe(base,name='usage.missing',view='full')
    assert current['description']['notes']==edited and current['contract']['state']=='missing'
    assert describe(base,name='usage.missing')['usage']['annotations']=={key:edited[key] for key in ('inputs','parameters','outputs','input_description','parameter_description','output_description')}
    client=ROOT/'examples/metadata_description/client.py'
    read=subprocess.run([sys.executable,str(client),'--web-url',base],capture_output=True,text=True,timeout=40)
    assert read.returncode==0,read.stderr
    report=json.loads(read.stdout)
    assert report['operator']['view']=='usage' and not report['executed']
    before=describe(base,name='usage.simple')
    saved=call(base,'operators/save-description',dict(name='usage.simple',expected_component_revision=before['basis']['component_revision'],
               expected_description_ref=None,notes=empty_notes(purpose='Local simple purpose')))
    current=describe(base,name='usage.simple')
    assert current['basis']['description_ref']==saved['description_ref'] and current['usage']['identity']['purposes']
    api(base,'operators/update',dict(name='usage.simple',content=simple+'\n# new component\n',
        expected_component_revision=current['basis']['component_revision']))
    updated=describe(base,name='usage.simple')
    assert updated['basis']['component_revision']!=current['basis']['component_revision']
    assert updated['basis']['description_ref'] is None and updated['usage']['identity']['purposes']==[]
    (evidence/'response-sizes.json').write_text(json.dumps(sizes,ensure_ascii=False,indent=2))

def main():
    evidence=ROOT/'evidence/operator-description-refinement/flat-fields/http';evidence.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='flowsql-description-http-') as tmp:
        directory=Path(tmp)
        with running_contract_host(Path(sys.argv[1]).resolve(),directory) as (base,process,log):
            for filename in ('api.schema.json','openapi.json'):
                assert request(base+'/api/semantics/v1/'+filename)[1]==json.loads((ROOT/'config/semantics'/filename).read_text())
            check_usage(base,evidence)
            locator=dict(kind='dataframe',name='input')
            d=describe(base,locator);assert d['structure']['fields'] and d['description']['state']=='missing' and d['checks']['state']=='unbound'
            assert d['checks']['data_validation']=='not_run'
            pending=describe(base,dict(kind='dataframe',name='lazy'));assert pending['structure']['state']=='pending'
            ref=bind_input(base);d=describe(base,locator);assert d['basis']['dictionary_ref']==ref and d['description']['state']=='present'
            for name in ('builtin.passthrough','explore.baseliner','contractdemo.output'):
                d=describe(base,name=name,view='full');assert d['contract']['state']=='declared' and d['description']['origin']=='component_author'
                assert d['basis']['component_revision'] and d['contract']['document']['inputs']
                if name=='explore.baseliner':assert len(d['contract']['document']['outputs'])==4 and d['contract']['document']['resources']
            for name in ('builtin.passthrough','contractdemo.output'):
                before=describe(base,name=name,view='full')
                notes=dict(structured_notes(),purpose='HTTP purpose',usage_notes='Read the actual contract',examples=['SELECT * FROM dataframe.input USING '+name])
                payload=dict(name=name,expected_component_revision=before['basis']['component_revision'],expected_description_ref=before['basis']['description_ref'],notes=notes)
                saved=call(base,'operators/save-description',payload)
                assert saved['created_revision'] and saved['description_ref']
                assert not call(base,'operators/save-description',payload)['created_revision']
                after=describe(base,name=name,view='full')
                assert after['description']['notes']==notes and after['contract']==before['contract']
                usage=describe(base,name=name)
                assert usage['basis']==after['basis']
                assert usage['usage']['annotations']=={key:notes[key] for key in ('inputs','parameters','outputs')}
                assert [p for p in usage['usage']['identity']['purposes'] if p['origin']=='local']==[dict(text=notes['purpose'],origin='local',source_pointer='/description/notes/purpose')]
                assert any(x['origin']=='local' and x['text']==notes['usage_notes'] for x in usage['usage']['limitations'])
                assert usage['usage']['examples'][0]['text']==notes['examples'][0]
                assert usage['usage']['examples'][0]['origin']=='local'
                assert [x['text'] for x in usage['usage']['limitations'] if x['origin']=='author']==before['contract']['document']['limitations']
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
