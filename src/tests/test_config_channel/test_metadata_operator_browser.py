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
    evidence = ROOT / 'evidence/metadata-description-query/implementation/t3-browser'
    evidence.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='flowsql-object-operators-') as temporary:
        directory = Path(temporary)
        with running_contract_host(Path(sys.argv[1]).resolve(), directory) as (base, _, log):
            bind_input(base)
            source = Path(__file__).with_name('fixtures').joinpath('description_chisquare.py').read_text()
            api(base,'operators/upload',dict(type='python',filename='explore_chisquare.py',content=source))
            for name in ['legacy', 'long_name_'*8]:
                legacy='from flowsql import OperatorBase, register_operator\n@register_operator(category="browser",name='+repr(name)+')\nclass Legacy(OperatorBase):\n def work(self,df): return df\n'
                api(base,'operators/upload',dict(type='python',filename=name+'.py',content=legacy))
            replacement=directory/'description_cpp.so'
            replacement.write_bytes(Path(sys.argv[3]).read_bytes())
            bad=directory/'broken_description.so';bad.write_bytes(b'not a shared library')
            result=subprocess.run([sys.argv[2],str(Path(__file__).with_name('metadata_operator_browser.mjs')),base,str(evidence),str(replacement),str(bad)],capture_output=True,text=True,timeout=260)
            (evidence/'browser.log').write_text(result.stdout+result.stderr)
            (evidence/'host.log').write_text(log.read_text())
            print(result.stdout+result.stderr)
            assert result.returncode==0
            refs={name:describe(base,name=name)['basis'] for name in ['explore.chisquare','builtin.passthrough']}
            for basis in refs.values():assert basis['description_ref']
        with running_contract_host(Path(sys.argv[1]).resolve(),directory) as (base,_,log):
            for name,basis in refs.items():
                current=describe(base,name=name)
                assert current['basis']['description_ref']==basis['description_ref'],current
                assert current['basis']['component_revision']==basis['component_revision']
                assert current['description']['notes']['purpose']
            assert describe(base,name='contractdemo.output')['description']['notes']['purpose']=='Original C++ notes'
            (evidence/'restart.log').write_text(log.read_text())
    print('PASS: actual Python/C++ declarations, component isolation and precise notes survive plugin/Worker restart')


if __name__=='__main__':main()
