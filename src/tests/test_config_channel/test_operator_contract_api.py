# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Ordinary HTTP client through actual Web/Gateway/Router with real Worker IPC."""
import contextlib
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from test_data_semantics_api import ROOT, request as http_request, port, validate

LOCAL_PROCESS = None
LOCAL_TRACE = []

def request(url, payload=None, raw=None):
    if LOCAL_PROCESS is None:
        return http_request(url, payload, raw)
    path = url.removeprefix("http://local/api")
    if path == "/tasks/batch/execute":
        path = "/scheduler/batch/execute"
        payload = dict(sql=payload["sql_text"])
    LOCAL_TRACE.append(path)
    LOCAL_PROCESS.stdin.write(json.dumps(dict(path=path,payload=payload)) + "\n")
    LOCAL_PROCESS.stdin.flush()
    while True:
        line = LOCAL_PROCESS.stdout.readline()
        assert line, "local host exited"
        if line.startswith("CONTRACT_LOCAL "):
            result = json.loads(line.removeprefix("CONTRACT_LOCAL "))
            return {0:200,-1:400,-2:404,-3:409,-4:500,-5:503,-6:413,-8:504,-9:422}.get(result["rc"],500), result["result"]


def call(base, operation, payload, expected=200):
    if operation.startswith('channels/'):
        key = operation.replace('/', '_')
    elif operation == 'operators/check':
        key = 'operators_check'
    else:
        key = operation
    if expected == 200:
        validate(key + 'Request', payload)
    status, result = request(base + '/api/semantics/v1/' + operation, payload)
    assert status == expected, (operation, status, expected, result)
    validate(key + 'Response' if status == 200 else 'error', result)
    return result.get('data', result.get('error'))


@contextlib.contextmanager
def running_contract_host(host, directory, provider=True):
    used = set()
    ports = [port(used) for _ in range(4)]
    base = 'http://127.0.0.1:' + str(ports[2])
    (directory / 'operators').mkdir(exist_ok=True)
    (directory / 'frames').mkdir(exist_ok=True)
    (directory / 'frames/lazy.csv').write_text('latency\n1000\n')
    with (directory / 'worker.log').open('w') as log:
        worker = subprocess.Popen([sys.executable, '-m', 'flowsql.worker', '--port', str(ports[3]),
                                   '--operators-dir', str(directory / 'operators')],
                                  env=dict(os.environ, PYTHONPATH=str(ROOT / 'src/python'), FLOWSQL_GATEWAY_ADDR=''),
                                  stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 20
            while True:
                try:
                    if request('http://127.0.0.1:' + str(ports[3]) + '/operators/python/health')[0] == 200:
                        break
                except OSError:
                    pass
                assert worker.poll() is None and time.monotonic() < deadline, (directory / 'worker.log').read_text()
                time.sleep(.05)
            host_log = directory / 'host.log'
            with host_log.open('w') as log:
                process = subprocess.Popen([str(host), str(directory), *map(str, ports),
                                            *([] if provider else ['without-provider'])],
                                           cwd=host.parent, env=dict(os.environ, FLOWSQL_GATEWAY_ADDR=''), stdin=subprocess.PIPE, stdout=log, stderr=log, text=True)
                try:
                    deadline = time.monotonic() + 25
                    while 'CONTRACT_HOST_READY' not in host_log.read_text():
                        assert process.poll() is None and time.monotonic() < deadline, host_log.read_text()
                        time.sleep(.05)
                    yield base, process, host_log
                finally:
                    if process.poll() is None:
                        try:
                            process.communicate('quit\n', timeout=15)
                        except subprocess.TimeoutExpired:
                            process.kill(); process.wait(timeout=5)
                            raise AssertionError(host_log.read_text())
                    assert process.returncode == 0, host_log.read_text()
        finally:
            evidence = ROOT / 'evidence/operator-data-contract/t5-protocol'
            evidence.mkdir(parents=True, exist_ok=True)
            for filename in ('host.log', 'worker.log'):
                if (directory / filename).exists():
                    (evidence / filename).write_text((directory / filename).read_text())
            worker.terminate(); worker.wait(timeout=10)


def api(base, path, payload, status=200):
    if path == "tasks/batch/execute":
        payload = dict(sql_text=payload["sql"], mode="sync")
    actual, value = request(base + '/api/' + path, payload)
    assert actual == status, (path, actual, status, value)
    return value


def preflight(base, name, parameters=None, locator=None, **extra):
    category, name = name.split('.', 1)
    payload = dict(operator=dict(category=category, name=name), parameters=parameters or {},
                   inputs=[dict(input_index=0, locator=locator or dict(kind='dataframe', name='input'))], **extra)
    return call(base, 'operators/check', payload)


def bind_input(base):
    locator = dict(kind='dataframe', name='input')
    preview = call(base, 'channels/preview', dict(locator=locator))
    preview['dictionary']['fields'][0]['unit'] = dict(state='declared', value=dict(code='ms', label='milliseconds'))
    saved = call(base, 'save', dict(kind='dictionary', name='http.input', expected_current_revision=0, document=preview['dictionary']))
    call(base, 'channels/apply', dict(locator=locator, name='http.input', dictionary_ref=saved['exact_reference'],
         field_map=preview['field_map'], expected_current_revision=0, confirmations=preview['required_confirmations'],
         source_basis={k: preview['source_snapshot'][k] for k in ('object_key', 'source_revision', 'schema_fingerprint')}))
    return saved['exact_reference']


def baseliner_checks(base):
    configs = json.loads((ROOT / 'src/tests/test_config_channel/fixtures/operator-baseliner-configs.json').read_text())
    first = dict(kind='database_table',type='sqlite',name='example',table='one')
    cfg = configs['multi']['config']
    before = preflight(base,'explore.baseliner',dict(parameters=json.dumps(cfg)),first)
    assert before['check']['state'] == 'undetermined', before
    assert before['check']['output_previews'][0]['context_state'] == 'incomplete'
    # Each dictionary has an independent physical unit and exact binding.
    for table, code in [('one','ms'),('two','B')]:
        locator = dict(first,table=table)
        preview = call(base,'channels/preview',dict(locator=locator))
        for field in preview['dictionary']['fields']:
            if field['name'] in ('value','total'):
                field['unit'] = dict(state='declared',value=dict(code=code if field['name']=='value' else '1',label=code))
        saved = call(base,'save',dict(kind='dictionary',name='http.'+table,expected_current_revision=0,document=preview['dictionary']))
        call(base,'channels/apply',dict(locator=locator,name='http.'+table,dictionary_ref=saved['exact_reference'],field_map=preview['field_map'],
             expected_current_revision=0,confirmations=preview['required_confirmations'],
             source_basis={k:preview['source_snapshot'][k] for k in ('object_key','source_revision','schema_fingerprint')}))
    for label in ('multi','relation'):
        cfg = configs[label]['config']
        checked = preflight(base,'explore.baseliner',dict(parameters=json.dumps(cfg)),first)
        assert checked['check']['state']=='undetermined',checked
        outputs = checked['check']['output_previews']
        assert len(outputs)==4 and outputs[0]['context_state']=='complete',outputs
        assert outputs[3]['availability']=='undetermined',outputs
        if label=='relation':
            assert outputs[2]['availability']=='undetermined',outputs
        finite = preflight(base,'explore.baseliner',dict(parameters=json.dumps(cfg)),first,
                           output_locator=dict(kind='dataframe',name='planned'))
        assert finite['check']['output_previews'][3]['availability']=='absent'
        table_target = preflight(base,'explore.baseliner',dict(parameters=json.dumps(cfg)),first,output_locator=dict(first,table='new_result'))
        assert table_target['check']['output_previews'][3]['availability']=='absent'
        wrong = preflight(base,'explore.baseliner',dict(parameters=json.dumps(cfg)),dict(first,table='two'))
        assert wrong['check']['state']=='incompatible'
        result = api(base,'tasks/batch/execute',dict(sql="SELECT * FROM sqlite.example USING explore.baseliner WITH parameters='"+
                     json.dumps(cfg,separators=(',',':'))+"' INTO dataframe.http_"+label))
        check = result['contract_checks'][0]
        assert check['output_check']=='matched' and check['data_validation']=='not_run',check
        use = call(base,'resolve',dict(exact_reference=check['use_ref']))['document']
        assert len(use['outputs'])==4 and use['outputs'][0]['row_cases']
        assert use['outputs'][0]['context_state']=='complete'
        assert all(o['state']=='absent' for o in use['outputs'][1:])
        for context in use['outputs'][0]['semantic_contexts']:
            assert context['state']=='complete' and context['required_context_refs']
            assert context['unit']['state']=='unknown'
            expected = 'B' if context['when']['dataset_id']=='ratio' else 'ms'
            for role in context['roles']:
                if role['column']=='value':
                    assert role['source_semantics']['unit']['value']['code']==expected
            for ref in context['required_context_refs']:
                assert call(base,'resolve',dict(exact_reference=ref))['document']
    pending = preflight(base,'builtin.passthrough',locator=dict(kind='dataframe',name='lazy'))
    assert pending['check']['state']=='undetermined'
    assert pending['resolved_invocation']['inputs'][0]['snapshot']['schema_state']=='pending'
    assert call(base,'channels/describe',dict(locator=dict(kind='dataframe',name='lazy')))['source_snapshot']['schema_state']=='pending'


def local_checks(host):
    global LOCAL_PROCESS
    with tempfile.TemporaryDirectory(prefix='flowsql-contract-preflight-') as temporary:
        directory = Path(temporary)
        (directory/'frames').mkdir()
        (directory/'frames/lazy.csv').write_text('latency\n1000\n')
        with (directory/'local.log').open('w') as log:
            process = subprocess.Popen([str(host),str(directory),'0','0','0','0','--local'],stdin=subprocess.PIPE,
                                       stdout=subprocess.PIPE,stderr=log,text=True,cwd=host.parent,env=dict(os.environ,FLOWSQL_GATEWAY_ADDR=''))
            try:
                while True:
                    line=process.stdout.readline()
                    assert line, (directory/'local.log').read_text()
                    if line.startswith('CONTRACT_HOST_READY'):break
                LOCAL_PROCESS=process
                base='http://local'
                baseliner_checks(base)
                bind_input(base)
                checked=preflight(base,'contractdemo.output',dict(operation='dual'))
                assert checked['check']['state']=='compatible' and checked['check']['role_map'],checked
                assert checked['resolved_invocation']['effective_parameters']['operation']=='dual'
                assert preflight(base,'contractdemo.output',dict(column='missing'))['check']['state']=='incompatible'
                bad=dict(operator=dict(category='contractdemo',name='output'),parameters={},inputs=[dict(input_index=0,locator=dict(kind='dataframe',name='input'),schema={})])
                assert call(base,'operators/check',bad,400)['code']=='CONTRACT_INVALID'
                result=api(base,'tasks/batch/execute',dict(sql="SELECT * FROM dataframe.input USING contractdemo.output WITH column='missing' INTO dataframe.rejected"),422)
                assert result['error']['code']=='CONTRACT_INCOMPATIBLE'
            finally:
                LOCAL_PROCESS=None
                if process.poll() is None:
                    remaining,_=process.communicate('quit\n',timeout=15)
                else:
                    remaining=process.stdout.read()
                evidence=ROOT/'evidence/operator-data-contract/t5-protocol'
                evidence.mkdir(parents=True,exist_ok=True)
                (evidence/'local-host.log').write_text(remaining+(directory/'local.log').read_text())
                assert process.returncode==0,(process.returncode,LOCAL_TRACE,remaining,(directory/'local.log').read_text())
    print('PASS: local server handler preflight (HTTP/Worker/browser not executed)')


def main():
    host = Path(sys.argv[1]).resolve()
    evidence = ROOT / 'evidence/operator-data-contract/t5-protocol'
    evidence.mkdir(parents=True, exist_ok=True)
    if '--local' in sys.argv:
        local_checks(host)
        return
    with tempfile.TemporaryDirectory(prefix='flowsql-contract-api-') as temporary:
        directory = Path(temporary)
        with running_contract_host(host, directory) as (base, _, log):
            baseliner_checks(base)
            input_ref = bind_input(base)
            detail = api(base, 'operators/detail', dict(name='contractdemo.output'))
            assert detail['contract_state'] == 'declared' and detail['component_revision']
            ref = detail['contract_ref']
            summary = call(base, 'resolve', dict(exact_reference=ref, view='summary'))
            assert summary['exact_reference'] == ref
            full = call(base, 'resolve', dict(exact_reference=ref))
            assert full['document']['operator']['name'] == 'output'
            projected = call(base, 'resolve', dict(exact_reference=ref, view='resource', resource_pointer='/inputs'))
            assert projected['exact_reference'] == ref
            checked = preflight(base, 'contractdemo.output', dict(operation='dual'))
            assert checked['check']['state'] == 'compatible', checked
            assert checked['check']['role_map'] and checked['check']['output_check'] == 'not_observed'
            assert checked['resolved_invocation']['inputs'][0]['binding_ref']
            assert checked['resolved_invocation']['effective_parameters']['operation'] == 'dual'
            payload = dict(operator=dict(category='contractdemo',name='output'), parameters={}, inputs=[dict(input_index=0,locator=dict(kind='dataframe',name='input'),schema={})])
            assert call(base, 'operators/check', payload, 400)['code'] == 'CONTRACT_INVALID'
            incompatible = preflight(base, 'contractdemo.output', dict(column='missing'))
            assert incompatible['check']['state'] == 'incompatible'
            error = api(base, 'tasks/batch/execute', dict(sql="SELECT * FROM dataframe.input USING contractdemo.output WITH column='missing' INTO dataframe.rejected"), 422)
            assert error['error']['code'] == 'CONTRACT_INCOMPATIBLE'
            result = api(base, 'tasks/batch/execute', dict(sql="SELECT * FROM dataframe.input USING contractdemo.output WITH operation='dual' INTO dataframe.dual"))
            checked = result['contract_checks'][0]
            assert checked['output_check'] == 'matched' and checked['metadata_state'] == 'persisted'
            use = call(base, 'resolve', dict(exact_reference=checked['use_ref']))['document']
            assert use['contract_ref'] == ref and len(use['outputs']) == 2
            assert {o['port_id'] for o in use['outputs']} == {'main','secondary'}
            for output in use['outputs']:
                dictionary = call(base, 'resolve', dict(exact_reference=output['dictionary_ref']))['document']
                assert dictionary['fields']
            # Real Python source publication and Arrow IPC execution with unchanged single DataFrame API.
            contract = json.loads((ROOT / 'config/semantics/examples/operator-contract-passthrough.json').read_text())
            contract['operator'] = dict(category='http', name='python', version='1')
            source = ('from flowsql import register_operator, OperatorBase\nimport pyarrow as pa\n'
                      '@register_operator(category="http", name="python", data_contract=' + repr(contract) + ')\n'
                      'class HttpOperator(OperatorBase):\n'
                      '    def work(self, df):\n'
                      '        return df.to_arrow().cast(pa.schema([pa.field("latency", pa.float64(), nullable=False)]))\n')
            api(base, 'operators/upload', dict(type='python', filename='http_python.py', content=source))
            api(base, 'operators/activate', dict(name='http.python'))
            py_detail = api(base, 'operators/detail', dict(name='http.python'))
            assert py_detail['contract_state'] == 'declared'
            py_check = preflight(base, 'http.python')
            assert py_check['check']['state'] in ('compatible','undetermined')
            py_result = api(base, 'tasks/batch/execute', dict(sql='SELECT * FROM dataframe.input USING http.python INTO dataframe.python_result'))
            assert py_result['contract_checks'][0]['output_check'] == 'matched'
            # Polars' default Arrow export makes fields nullable. A copy declaration
            # of this non-nullable input must detect that real structural drift.
            roundtrip_contract = copy.deepcopy(contract)
            roundtrip_contract['operator']['name'] = 'roundtrip'
            roundtrip = ('from flowsql import register_operator, OperatorBase\n'
                         '@register_operator(category="http", name="roundtrip", data_contract=' + repr(roundtrip_contract) + ')\n'
                         'class Roundtrip(OperatorBase):\n    def work(self, df):\n        return df\n')
            api(base, 'operators/upload', dict(type='python', filename='http_roundtrip.py', content=roundtrip))
            api(base, 'operators/activate', dict(name='http.roundtrip'))
            mismatch = api(base, 'tasks/batch/execute', dict(sql='SELECT * FROM dataframe.input USING http.roundtrip INTO dataframe.roundtrip_result'), 500)
            assert mismatch['error']['code'] == 'OUTPUT_SCHEMA_MISMATCH'
            assert api(base, 'operators/update', dict(name='http.python', content=source+'\n', expected_component_revision='stale'),409)['error']
            # An unchanged historical operator still runs with an explicit missing-contract result.
            legacy = source.replace(', data_contract=' + repr(contract), '').replace('name="python"','name="legacy"').replace('HttpOperator','LegacyOperator')
            api(base,'operators/upload',dict(type='python',filename='http_legacy.py',content=legacy))
            api(base,'operators/activate',dict(name='http.legacy'))
            old = api(base,'tasks/batch/execute',dict(sql='SELECT * FROM dataframe.input USING http.legacy INTO dataframe.legacy_result'))
            assert old['contract_checks'][0]['contract_state'] == 'missing'
            (evidence / 'host.log').write_text(log.read_text())
            (evidence / 'http-results.json').write_text(json.dumps(dict(preflight=checked, use=use, input_ref=input_ref),ensure_ascii=False,indent=2))
    with tempfile.TemporaryDirectory(prefix='flowsql-contract-unavailable-') as temporary:
        with running_contract_host(host,Path(temporary),provider=False) as (base,_,_):
            payload=dict(operator=dict(category='builtin',name='passthrough'),parameters={},
                         inputs=[dict(input_index=0,locator=dict(kind='dataframe',name='input'))])
            assert call(base,'operators/check',payload,503)['code']=='UNAVAILABLE'
    print('ordinary HTTP + Python Worker contract closure passed')


if __name__ == '__main__':
    main()
