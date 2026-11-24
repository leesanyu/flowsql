# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""NPM managed rows in a fresh metadata store: static meaning never invents historical use."""
import json
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile
from test_data_semantics_api import ROOT, request
from test_operator_contract_api import running_contract_host, call
from test_npm_semantics_http import write_all_capture, http_client, publish_labels, run_all
sys.path.insert(0, str(ROOT / 'src/tests/test_visualization'))
from test_semantics import visualization, get, compute


def main():
    executable, node = Path(sys.argv[1]).resolve(), sys.argv[2]
    evidence = ROOT / 'evidence/data-semantics-consumption/npm-browser'
    evidence.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='flowsql-consumption-npm-seed-') as temporary:
        seed = Path(temporary)
        write_all_capture(seed)
        with running_contract_host(executable, seed, npm=True) as (base, process, _):
            client = http_client(base, process)
            run_all(client, publish_labels(client, ['dns','http1','tls']))
        with tempfile.TemporaryDirectory(prefix='flowsql-consumption-npm-static-') as copied:
            directory = Path(copied)
            shutil.copy2(seed / 'input.db', directory / 'input.db')
            write_all_capture(directory)
            with running_contract_host(executable, directory, npm=True) as (base, process, log):
                gateway = 'http://127.0.0.1:' + str(process.args[2])
                before = call(base, 'list', {})
                assert not call(base, 'list', dict(kind='operator_use'))['items']
                def execute(projection, entity):
                    status, result = request(base+'/api/tasks/batch/execute', dict(sql_text=f'SELECT {projection} FROM sqlite.example.npm_{entity}_ipv4_data',mode='sync'))
                    assert status==200 and result['status']=='completed', result
                    source = result['semantic_context']['sources'][0]
                    assert source['description']['origin']=='producer_default' and source['basis']['dictionary_ref'] is None
                    assert source['basis']['contract_ref'] and source['basis']['component_revision']
                    assert not source['context'].get('operator_use')
                    return result, source['description']['document']
                dns, dns_document = execute('latency_ns','dns_transaction')
                latency = next(f for f in dns_document['fields'] if f['name']=='latency_ns')
                assert dns['semantic_context']['fields'][0]['result_unit']=='ns'
                assert 'outcome' in latency['validity']['value']['description']
                assert '不是零时延' in latency['null_meaning']['value']
                session, session_document = execute('SUM(packets_ab) AS total','session')
                packets = next(f for f in session_document['fields'] if f['name']=='packets_ab')
                assert session['semantic_context']['fields'][0]['result_unit'] is None
                assert '不能跨修订相加' in packets['aggregation']['value'][0]['condition']['description']
                facts = dict(validity=latency['validity']['value']['description'], null_meaning=latency['null_meaning']['value'],
                    aggregation=latency['aggregation']['value'][0]['condition']['description'],
                    cumulative=packets['aggregation']['value'][0]['condition']['description'],
                    session_warning=next(v for v in session_document['entity']['limitations']['value'] if '累计修订不能相加' in v))
                facts_path = evidence / 'facts.json'; facts_path.write_text(json.dumps(facts,ensure_ascii=False))
                shared=directory/'shared';shared.mkdir(mode=0o700)
                with visualization(executable.parent,directory,gateway,directory/'database.yml',shared) as visual:
                    item=next(s for s in get(visual,'sources')['sources'] if s['label'].endswith('.npm_dns_transaction_ipv4_data'))
                    schema=get(visual,'sources/'+item['source_id'])
                    fid=next(f['field_id'] for f in schema['fields'] if f['label']=='latency_ns')
                    chart=compute(visual,schema,dict(op='raw',fields=[fid]))
                    assert chart['semantic_context']['sources'][0]['description']['origin']=='producer_default'
                    result=subprocess.run([node,str(Path(__file__).with_name('consumption_npm_browser.mjs')),base,visual,str(facts_path),str(evidence)],capture_output=True,text=True,timeout=150)
                    (evidence/'browser.log').write_text(result.stdout+result.stderr)
                    print(result.stdout+result.stderr);assert result.returncode==0
                assert call(base,'list',{})==before
                assert not call(base,'list',dict(kind='operator_use'))['items']
                with sqlite3.connect(directory/'input.db') as db:
                    assert db.execute('SELECT count(*) FROM npm_result_runs').fetchone()[0]==1
                (evidence/'host.log').write_text(log.read_text())
    print('PASS NPM DNS validity/NULL/ns, Session cumulative warning, exact producer defaults, no invented historical uses, real workbench and exploration')


if __name__=='__main__':main()
