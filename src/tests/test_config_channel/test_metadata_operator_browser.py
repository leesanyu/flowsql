# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Actual operator object notes, author declarations and Python/C++ component replacement."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from test_data_semantics_api import ROOT, request
from test_operator_contract_api import running_contract_host, api, bind_input
from test_metadata_description_api import describe


def main():
    evidence = ROOT / 'evidence/operator-description-refinement/inline-editors/operator-browser'
    evidence.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='flowsql-object-operators-') as temporary:
        directory = Path(temporary)
        with running_contract_host(Path(sys.argv[1]).resolve(), directory) as (base, _, log):
            bind_input(base)
            multi = Path(sys.argv[1]).resolve().parent / 'libfixture_cpp_operator_v2.so'
            source = Path(__file__).with_name('fixtures').joinpath('description_chisquare.py').read_text()
            api(base,'operators/upload',dict(type='python',filename='explore_chisquare.py',content=source))
            for name in ['legacy', 'long_name_'*8]:
                legacy='from flowsql import OperatorBase, register_operator\n@register_operator(category="browser",name='+repr(name)+')\nclass Legacy(OperatorBase):\n def work(self,df): return df\n'
                api(base,'operators/upload',dict(type='python',filename=name+'.py',content=legacy))
            history_contract = json.loads((ROOT/'config/semantics/examples/operator-contract-passthrough.json').read_text())
            for revision in range(1,13):
                history_contract['operator'] = dict(category='browser',name='history',version=str(revision))
                history_contract['title'] = 'History fixture release '+str(revision)
                history_source = ('from flowsql import OperatorBase, register_operator\n'
                                  '@register_operator(category="browser",name="history",data_contract='+repr(history_contract)+')\n'
                                  'class History(OperatorBase):\n def work(self,df): return df\n')
                if revision == 1:
                    api(base,'operators/upload',dict(type='python',filename='browser_history.py',content=history_source))
                else:
                    current = api(base,'operators/detail',dict(name='browser.history'))
                    api(base,'operators/update',dict(name='browser.history',content=history_source,expected_component_revision=current['component_revision']))
            replacement=directory/'description_cpp.so'
            replacement.write_bytes(Path(sys.argv[3]).read_bytes())
            bad=directory/'broken_description.so';bad.write_bytes(b'not a shared library')
            # Reproduce an existing record whose old editor saved empty text beside author prose.
            original=describe(base,name='explore.baseliner')
            api(base,'semantics/v1/operators/save-description',dict(name='explore.baseliner',
                expected_component_revision=original['basis']['component_revision'],expected_description_ref=None,
                notes=dict(purpose='',usage_notes='',examples=[],inputs=[],parameters=[],outputs=[])))
            result=subprocess.run([sys.argv[2],str(Path(__file__).with_name('metadata_operator_browser.mjs')),base,str(evidence),str(replacement),str(bad),str(multi)],capture_output=True,text=True,timeout=260)
            (evidence/'browser.log').write_text(result.stdout+result.stderr)
            (evidence/'host.log').write_text(log.read_text())
            print(result.stdout+result.stderr)
            assert result.returncode==0
            refs={name:describe(base,name=name)['basis'] for name in ['explore.chisquare','builtin.passthrough']}
            for basis in refs.values():assert basis['description_ref']
            inline_notes={name:describe(base,name=name,view='full')['description']['notes'] for name in ['npm.basic','explore.baseliner']}
            for notes in inline_notes.values():
                assert notes['purpose']=='' and notes['parameter_description']==''
                assert notes['input_description'].endswith('已维护')
                assert notes['output_description'].endswith('补充结果说明')
        with running_contract_host(Path(sys.argv[1]).resolve(),directory) as (base,_,log):
            for name,notes in inline_notes.items():
                assert describe(base,name=name,view='full')['description']['notes']==notes
                current_usage=describe(base,name=name)['usage']
                for key in ['input_description','parameter_description','output_description']:
                    assert current_usage['annotations'][key]==notes[key]
            for name,basis in refs.items():
                current=describe(base,name=name,view='full')
                assert current['basis']['description_ref']==basis['description_ref'],current
                assert current['basis']['component_revision']==basis['component_revision']
                assert current['description']['notes']['purpose']
                assert current['description']['notes']['inputs'][0]['description']=='Input latency is measured in milliseconds'
                assert current['description']['notes']['parameters'][0]['description']=='Pass column names as strings'
                assert current['description']['notes']['outputs'][0]['description']=='Interpret rows according to the output conditions'
            cpp_notes=describe(base,name='contractdemo.output',view='full')['description']['notes']
            assert cpp_notes['purpose']=='Original C++ notes'
            assert cpp_notes['inputs'][0]['description']=='Input latency is measured in milliseconds'
            (evidence/'restart.log').write_text(log.read_text())
    print('PASS: actual Python/C++ declarations, component isolation and precise notes survive plugin/Worker restart')


if __name__=='__main__':main()
