# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Real standalone/embedded exploration: reading never runs computations or replaces snapshot evidence."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import uuid
from test_semantics import running, visualization, metadata, metadata_request, meaning, ROOT
from test_entry import port, service


def rich_meaning(control, locator):
    observed=metadata(control,'describe',dict(target=dict(kind='channel',locator=locator)))
    document=observed['description']['document']
    declared=lambda value:dict(state='declared',value=value,origin='manual')
    field=next(f for f in document['fields'] if f['name']=='value')
    field['definition']=declared('<img src=x onerror="window.__unsafe=true"> browser captured meaning '+uuid.uuid4().hex)
    field['null_meaning']=declared('not measured')
    field['validity']=declared(dict(mode='descriptive',description='only valid samples',field_ids=[]))
    field['aggregation']=declared([dict(operation='sum',condition=dict(mode='descriptive',description='only one revision per identity',field_ids=[]))])
    field['value_domain']=declared(dict(kind='range',min=0,max=18446744073709551615,min_inclusive=True,max_inclusive=True))
    document['entity']['row_semantics']=declared('cumulative')
    document['entity']['limitations']=declared(['cumulative revisions must not be summed'])
    reference=observed['basis']['dictionary_ref'];name,revision=reference.removeprefix('dictionary.').rsplit('@',1)
    saved=metadata(control,'save',dict(kind='dictionary',name=name,expected_current_revision=int(revision),document=document))
    binding,revision=observed['basis']['binding_ref'].removeprefix('binding.').rsplit('@',1)
    metadata(control,'channels/apply',dict(locator=locator,name=binding,expected_current_revision=int(revision),
        dictionary_ref=saved['exact_reference'],field_map=observed['context']['binding']['field_map'],
        source_basis={key:observed['basis'][key] for key in ['object_key','source_revision','schema_fingerprint']},
        confirmations=observed['checks']['required_confirmations']))


def main():
    output,node=Path(sys.argv[1]).resolve(),sys.argv[2]
    evidence=ROOT/'evidence/data-semantics-consumption/exploration-browser';evidence.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='flowsql-semantic-reading-ui-') as tmp:
        directory=Path(tmp)
        with running(output,directory) as (control,visual,channels,shared):
            frame=dict(kind='dataframe',name='sales');database=dict(kind='database_table',type='sqlite',name='memory',table='rows')
            status,loaded=metadata_request(control+'/channels/dataframe/preview',dict(category='dataframe',name='sales',page=1,page_size=1))
            assert status==200,loaded
            meaning(control,frame,'value','fixture');meaning(control,database,'value','fixture')
            web='http://127.0.0.1:'+str(port())
            options=f'port={web.rsplit(":",1)[1]};db_path={directory}/web.db;upload_dir={directory}/uploads;visualization_upstream={visual};gateway={control.removeprefix("http://")}'
            with service(output,directory,'web',options,web+'/api/version'):
                for mode,base in [('standalone',visual),('embedded',web)]:
                    rich_meaning(control,frame);rich_meaning(control,database)
                    result=subprocess.run([node,str(Path(__file__).with_name('semantics_browser.mjs')),base,mode,control,str(evidence)],capture_output=True,text=True,timeout=150)
                    (evidence/(mode+'.log')).write_text(result.stdout+result.stderr)
                    print(result.stdout+result.stderr)
                    assert result.returncode==0
            with visualization(output,directory,'',channels,shared) as unavailable:
                result=subprocess.run([node,str(Path(__file__).with_name('semantics_browser.mjs')),unavailable,'error',control,str(evidence)],capture_output=True,text=True,timeout=90)
                (evidence/'error.log').write_text(result.stdout+result.stderr)
                print(result.stdout+result.stderr);assert result.returncode==0
    print('PASS actual standalone/embedded source, selected fields and chart explanations')


if __name__=='__main__':main()
