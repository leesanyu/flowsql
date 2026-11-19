# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Opt-in local performance gates.

Dependencies: built native plugins/hosts, Python Worker dependencies and GDB with
ptrace permission. Temporary SQLite/PCAP fixtures and loopback ports only; no
external database/Kafka, credentials or business data. Run exclusively: no builds
or other benchmarks concurrently. GDB scope timings and uninstrumented data-plane
throughput are reported separately. FLOWSQL_SEMANTICS_BASELINE_ROOT is a frozen
artifact directory supplied at runtime, never an environment captured by CMake.
"""
import concurrent.futures
import json
from pathlib import Path
import statistics
import sys
import tempfile
import time
ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT/'src/tests/test_config_channel'))
from test_operator_contract_api import running_contract_host
from test_npm_semantics_http import write_all_capture
from test_data_semantics_api import request

def read_probe(host, destination):
    host, destination = Path(host).resolve(), Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    report = {}
    with tempfile.TemporaryDirectory(prefix='flowsql-read-perf-') as tmp:
        root = Path(tmp)
        write_all_capture(root)
        started = time.perf_counter()
        with running_contract_host(host, root, npm=True) as (base, process, log):
            report['service_ready_ms'] = (time.perf_counter()-started)*1000
            def post(operation, payload):
                start = time.perf_counter()
                status, body = request(base+'/api/semantics/v1/'+operation, payload)
                ms = (time.perf_counter()-start)*1000
                assert status == 200, (operation, status, body)
                return ms, body['data'], len(json.dumps(body, ensure_ascii=False).encode())
            cold, usage, size = post('describe',dict(target=dict(kind='operator',name='npm.basic')))
            report['first_usage_ms'] = cold
            exact = usage['basis']['contract_ref']
            for kind, name in [('standard','request.duration'),('dictionary','app.requests')]:
                post('save',dict(kind=kind,name=name,expected_current_revision=0,document=json.loads((ROOT/'examples/semantics'/f'{kind}.json').read_text())))
            cases = {
                'standard':('resolve',dict(exact_reference='standard.request.duration@1')),
                'dictionary':('resolve',dict(exact_reference='dictionary.app.requests@1')),
                'usage':('describe',dict(target=dict(kind='operator',name='npm.basic'))),
                'resource':('resolve',dict(exact_reference=exact,view='resource',resource_pointer='/resources/dns_transaction_ipv4/dictionary/fields/0')),
                'fields':('resolve',dict(exact_reference='dictionary.app.requests@1',view='fields',field_ids=['elapsed'])),
            }
            for name, (operation,payload) in cases.items():
                for _ in range(5):post(operation,payload)
                times=[]; sizes=[]
                for _ in range(100):
                    elapsed, data, size=post(operation,payload)
                    times.append(elapsed);sizes.append(size)
                assert max(sizes) <= 65536
                report[name]=dict(samples_ms=times,p95_ms=sorted(times)[94],median_ms=statistics.median(times),max_response_bytes=max(sizes))
                (destination/'reads.json').write_text(json.dumps(report,indent=2))
                print(name,report[name]['p95_ms'],flush=True)
            def concurrent_read(index):
                operation,payload=cases['resource' if index % 2 else 'usage']
                return post(operation,payload)[0]
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                times=list(pool.map(concurrent_read,range(104)))
            report['concurrent']=dict(samples_ms=times,p95_ms=sorted(times)[98],completed=len(times))
            (destination/'reads.json').write_text(json.dumps(report,indent=2))
        (destination/'host.log').write_text((root/'host.log').read_text())
    return report

import contextlib
import csv
import os
import subprocess
import hashlib
import sqlite3
from test_npm_semantics_http import http_client, run_all, check_all
from test_npm_semantics_pipeline import publish_labels, describe, resolve, semantic

def business_rows(root, published, run_id):
    result = {}
    with sqlite3.connect(root/'input.db') as database:
        for table, relation in published.items():
            if relation['view'] == 'runs': continue
            columns = [r[1] for r in database.execute('PRAGMA table_xinfo("'+table+'")')]
            excluded = {'__npm_run_id', '__npm_task_id'}
            if table.startswith('npm_basic_'): excluded.add('observed_at')  # Generation wall clock, reported separately.
            selected = [i for i, name in enumerate(columns) if name not in excluded]
            rows = [tuple({'sqlite_blob_hex':row[i].hex()} if isinstance(row[i],bytes) else row[i] for i in selected)
                    for row in database.execute('SELECT * FROM "'+table+'" WHERE __npm_run_id=?',(run_id,))]
            encoded = sorted(json.dumps(row, separators=(',',':')) for row in rows)
            result[table] = dict(rows=len(rows), sha256=hashlib.sha256('\n'.join(encoded).encode()).hexdigest())
    return result

def events(path,start):
    values=[json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []
    return values[start:]

def summarize(values):
    result={}
    for value in values:
        assert value['status']=='returned',value
        result.setdefault(value['stage'],[]).append((value['end_ns']-value['start_ns'])/1e6)
    # Nested contract checks in RecordUse belong to the use-record phase, once.
    top=[v for v in values if v['stage'] in ['contract_check','publication','use_record'] and
         not any(p['token']==v['parent'] and p['stage'] in ['contract_check','publication','use_record'] for p in values)]
    result['semantic_total_ms']=sum((v['end_ns']-v['start_ns'])/1e6 for v in top)
    assert len(result.get('object_publication',[]))==37,result.keys()
    assert result.get('contract_check') and result.get('use_record')
    return result

def audit_data(values):
    spans=[v for v in values if v['stage'] in ['data_process','data_time','data_flush']]
    assert any(v['stage']=='data_process' for v in spans)
    for span in spans:
        assert span['status']=='returned',span
        assert not any(v['stage'] in ['definition_body','definition_read','definition_resolve','full_parse'] and
                       v['thread']==span['thread'] and span['start_ns']<=v['start_ns']<=span['end_ns'] for v in values)
    return {'processing_calls':len(spans),'definition_reads_or_full_parses_during_processing':0}

def task_probe(baseline, candidate, destination):
    global EVIDENCE
    EVIDENCE = Path(destination)
    EVIDENCE.mkdir(exist_ok=True,parents=True)
    report={label:[] for label in ['baseline','candidate']}
    with tempfile.TemporaryDirectory(prefix='flowsql-task-perf-') as tmp,contextlib.ExitStack() as stack:
        clients={}
        for label,host in [('baseline',Path(baseline)/'test_operator_contract_host'),
                           ('candidate',Path(candidate)/'test_operator_contract_host')]:
            root=Path(tmp)/label;root.mkdir();write_all_capture(root)
            trace=EVIDENCE/f'{label}-task-scopes.jsonl';trace.write_text('')
            Path(str(trace)+'.audit').unlink(missing_ok=True)
            os.environ['FLOWSQL_SEMANTICS_PROFILE_HOST']=str(host)
            os.environ['FLOWSQL_SEMANTICS_PROFILE_OUTPUT']=str(trace)
            base,process,log=stack.enter_context(running_contract_host(Path(__file__).with_name('semantics_profile_host.sh'),root,npm=True,startup_budget=90))
            call=http_client(base,process)
            labels=publish_labels(call,['dns','http1','tls'])
            clients[label]=(root,trace,call,labels,log)
            (EVIDENCE/f'{label}-task-host.log').write_text(log.read_text())
        for round in range(6):
            for label in (['baseline','candidate'] if round%2==0 else ['candidate','baseline']):
                root,trace,call,labels,log=clients[label]
                offset=len(events(trace,0));start=time.perf_counter()
                run=run_all(call,labels)
                elapsed=(time.perf_counter()-start)*1000
                values=events(trace,offset)
                metrics=summarize(values)
                used,published=check_all(call,root,run,'final')
                assert len(published)==37
                metrics.update(round=round,first_creation=round==0,total_task_ms=elapsed,
                               task_id=run['task_id'],use_ref=run['contract_checks'][0]['use_ref'],
                               run_id=used['execution_context']['npm']['run_id'])
                metrics['business'] = business_rows(root, published, metrics['run_id'])
                relation = published['npm_dns_transaction_ipv4_final']
                current = describe(call, relation['locator'])
                if round == 0:
                    field = next(f for f in current['description']['document']['fields'] if f['name']=='latency_ns')
                    semantic(call, 'channels/save-description', dict(locator=relation['locator'], basis=current['basis'],
                        editor_changes={'fields':{field['field_id']:{'definition':'性能验收：保留人工补充'}}},
                        confirmations=current['checks'].get('required_confirmations',[])))
                else:
                    field = next(f for f in current['description']['document']['fields'] if f['name']=='latency_ns')
                    assert field['definition']['value'] == '性能验收：保留人工补充'
                report[label].append(metrics)
                (EVIDENCE/f'{label}-task-host.log').write_text(log.read_text())
                (EVIDENCE/'task-comparison.json').write_text(json.dumps(report,indent=2))
                print(label,round,metrics['semantic_total_ms'],flush=True)
        a=statistics.median(x['semantic_total_ms'] for x in report['baseline'][1:])
        b=statistics.median(x['semantic_total_ms'] for x in report['candidate'][1:])
        for a_row, b_row in zip(report['baseline'], report['candidate']):
            assert a_row['business'] == b_row['business'], (a_row['round'], a_row['business'], b_row['business'])
        report['gate']=dict(baseline_median_ms=a,candidate_median_ms=b,reduction=1-b/a,passed=b<=a*.3)
        (EVIDENCE/'task-comparison.json').write_text(json.dumps(report,indent=2))
        assert report['gate']['passed'],report['gate']
        # Enable deeper probes only after timing; they add variable breakpoint overhead.
        root,trace,call,labels,log=clients['candidate']
        Path(str(trace)+'.audit').write_text('enabled')
        offset=len(events(trace,0))
        run_all(call,labels)
        audit=audit_data(events(trace,offset))
        (EVIDENCE/'task-data-audit.json').write_text(json.dumps(audit,indent=2))

def data_probe(baseline, candidate, destination):
    destination = Path(destination)
    report = {}
    executable = Path(candidate) / 'benchmark_npm_throughput'
    for mode in ['offline', 'realtime']:
        report[mode] = {'baseline': [], 'candidate': []}
        for round in range(5):
            for label in (['baseline', 'candidate'] if round % 2 == 0 else ['candidate', 'baseline']):
                env = dict(os.environ, FLOWSQL_BENCH_PLUGIN_ROOT=str(baseline if label == 'baseline' else candidate),
                           FLOWSQL_BENCH_COMMON_DOMAIN='1')
                env.pop('FLOWSQL_BENCH_REALTIME', None)
                if mode == 'realtime': env['FLOWSQL_BENCH_REALTIME'] = '1'
                run = subprocess.run([str(executable), '2048', '50', 'mixed', 'ack', '8', '128', '10000', 'periodic', '2'],
                                     env=env, cwd=candidate, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
                (destination/f'{mode}-{label}-{round}.log').write_text(run.stdout)
                assert run.returncode == 0, run.stdout
                lines = run.stdout.splitlines()
                selected = [line for line in lines if line.startswith('measurement,')]
                assert len(selected) == 2, run.stdout
                metrics = next(csv.DictReader(selected))
                metrics['latency'] = json.loads(next(line.removeprefix('latency,') for line in lines if line.startswith('latency,')))
                assert metrics['latency']['periodic_samples'] > 0
                report[mode][label].append(metrics)
                print(mode,label,round,metrics['primary_pps'],flush=True)
                (destination/'data-comparison.json').write_text(json.dumps(report, indent=2))
        for a, b in zip(report[mode]['baseline'], report[mode]['candidate']):
            for key in ['business_digest','foreground_rows','wire_fixture_digest','packets']:
                assert a[key] == b[key], (mode, key, a[key], b[key])
        a = statistics.median(float(x['primary_pps']) for x in report[mode]['baseline'])
        b = statistics.median(float(x['primary_pps']) for x in report[mode]['candidate'])
        report[mode]['gate'] = dict(baseline_pps=a, candidate_pps=b, ratio=b/a, passed=b>=a*.95)
    (destination/'data-comparison.json').write_text(json.dumps(report, indent=2))
    assert all(report[mode]['gate']['passed'] for mode in ['offline','realtime']), report
    for mode in ['offline','realtime']:
        trace = destination/f'{mode}-data-audit.jsonl'
        trace.write_text('')
        env = dict(os.environ, FLOWSQL_SEMANTICS_PROFILE_HOST=str(executable),
                   FLOWSQL_SEMANTICS_PROFILE_OUTPUT=str(trace), FLOWSQL_BENCH_PLUGIN_ROOT=str(candidate),
                   FLOWSQL_BENCH_COMMON_DOMAIN='1', FLOWSQL_SEMANTICS_PROFILE_AUDIT='1')
        env.pop('FLOWSQL_BENCH_REALTIME', None)
        if mode == 'realtime': env['FLOWSQL_BENCH_REALTIME']='1'
        audit = subprocess.run([str(Path(__file__).with_name('semantics_profile_host.sh')), '256','5','mixed','ack','8','128','10000','periodic','2'],
                               cwd=candidate,env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=120)
        (destination/f'{mode}-data-audit.log').write_text(audit.stdout)
        assert audit.returncode==0, audit.stdout
        values=events(trace,0)
        assert len([v for v in values if v['stage']=='data_process'])==6
        if mode=='realtime': assert len([v for v in values if v['stage']=='data_time'])==6
        audit_data(values)

def main():
    import argparse
    parser=argparse.ArgumentParser(description='Dedicated static semantics performance gates; no external services')
    parser.add_argument('--candidate', required=True, type=Path)
    parser.add_argument('--destination', required=True, type=Path)
    parser.add_argument('--phase', choices=['all','reads','tasks','data'], default='all')
    args=parser.parse_args()
    baseline=Path(os.environ['FLOWSQL_SEMANTICS_BASELINE_ROOT']).resolve()
    candidate=args.candidate.resolve(); destination=args.destination.resolve()
    assert (baseline/'test_operator_contract_host').is_file(), baseline
    destination.mkdir(parents=True, exist_ok=True)
    def artifact_hashes(folder):
        names=['test_operator_contract_host','benchmark_npm_throughput','libflowsql_config_channel.so',
               'libflowsql_npm_basic.so','libflowsql_scheduler.so','libflowsql_catalog.so','libflowsql_database.so']
        return {name:hashlib.sha256((folder/name).read_bytes()).hexdigest() for name in names if (folder/name).is_file()}
    import platform
    manifest=dict(time_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),execution='native',
                  platform=platform.platform(),cpus=os.cpu_count(),
                  cpu=next(line.split(':',1)[1].strip() for line in Path('/proc/cpuinfo').read_text().splitlines() if line.startswith('model name')),
                  memory=Path('/proc/meminfo').read_text().splitlines()[0],
                  baseline=artifact_hashes(baseline),candidate=artifact_hashes(candidate),
                  timing='HTTP uninstrumented; task GDB inclusive scopes; data-plane uninstrumented; source decoder/validation separate')
    (destination/'manifest.json').write_text(json.dumps(manifest,indent=2))
    if args.phase in ['all','reads']:
        a=read_probe(baseline/'test_operator_contract_host', destination/'baseline')
        b=read_probe(candidate/'test_operator_contract_host', destination/'candidate')
        assert all(b[key]['p95_ms'] <= 100 for key in ['standard','dictionary','usage','resource','fields']), b
        assert all(b[key]['p95_ms'] <= a[key]['p95_ms']*.2 for key in ['usage','resource']), (a,b)
        subprocess.run([str(candidate/'test_npm_data_contract'),'--performance'],check=True,cwd=candidate)
    if args.phase in ['all','tasks']: task_probe(baseline,candidate,destination)
    if args.phase in ['all','data']: data_probe(baseline,candidate,destination)
if __name__=='__main__': main()
