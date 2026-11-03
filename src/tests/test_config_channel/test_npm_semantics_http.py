# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Twelve real entities: HTTP, existing browser, precise history and actual restart."""
import json
import shutil
from pathlib import Path
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time

from test_data_semantics_api import ROOT, request
from test_operator_contract_api import running_contract_host
from test_npm_semantics_pipeline import (describe, host_process, publish_labels, resolve, semantic,
                                         write_dns_capture, write_protocol_capture)


def capture_records(path):
    raw = path.read_bytes()
    offset = 24
    while offset < len(raw):
        seconds, micros, captured, wire = struct.unpack_from('<IIII', raw, offset)
        offset += 16
        yield seconds * 1_000_000 + micros, raw[offset:offset + captured], wire
        offset += captured
    assert offset == len(raw)


def write_all_capture(root, pcapng=False):
    write_dns_capture(root / 'dns.pcap')
    write_protocol_capture(root / 'protocols.pcap')
    records = sorted([*capture_records(root / 'dns.pcap'), *capture_records(root / 'protocols.pcap')], key=lambda r: r[0])
    with (root / 'npm.pcap').open('wb') as output:
        if pcapng:
            def block(kind, body):
                body += bytes((-len(body)) % 4)
                size = len(body) + 12
                output.write(struct.pack('<II', kind, size) + body + struct.pack('<I', size))
            block(0x0a0d0d0a, struct.pack('<IHHq', 0x1a2b3c4d, 1, 0, -1))
            block(1, struct.pack('<HHI', 1, 0, 65535))
            for micros, raw, wire in records:
                block(6, struct.pack('<IIIII', 0, micros >> 32, micros & 0xffffffff, len(raw), wire) + raw)
        else:
            output.write(struct.pack('<IHHIIII', 0xa1b2c3d4, 2, 4, 0, 0, 65535, 1))
            for micros, raw, wire in records:
                seconds, fraction = divmod(micros, 1_000_000)
                output.write(struct.pack('<IIII', seconds, fraction, len(raw), wire) + raw)


def http_client(base, process):
    gateway = 'http://127.0.0.1:' + str(process.args[2])
    def call(path, payload, expected=0):
        if path == '/scheduler/batch/execute':
            # A full managed publication can outlive one HTTP request. Submit
            # once, then observe that exact task through the existing APIs.
            status, submitted = request(base + '/api/tasks/batch/execute',
                                        dict(sql_text=payload['sql'], mode='async'))
            assert status == 200 and submitted['status'] == 'pending', (status, submitted)
            task = dict(task_id=submitted['task_id'])
            deadline = time.monotonic() + 30
            while True:
                status, result = request(base + '/api/tasks/result', task)
                assert status == 200, (status, result)
                if result['status'] == 'completed':
                    break
                assert result['status'] in ('pending', 'running'), result
                assert time.monotonic() < deadline, (task, result)
                time.sleep(.1)
            # Web's result projection omits contract_checks. Gateway exposes
            # the existing Task detail route with the persisted exact record.
            status, detail = request(gateway + '/tasks/detail', task)
            assert status == 200 and detail['status'] == 'completed', (status, detail)
            assert detail['task_id'] == task['task_id']
            assert detail['runtime_task_id'] == submitted['runtime_task_id']
            assert detail['result_row_count'] == result['rows_affected']
            assert len(detail['contract_checks']) == 1 and detail['contract_checks'][0]['use_ref']
            return detail
        status, value = request(base + '/api' + path, payload)
        assert status == {0: 200, -1: 400, -3: 409, -9: 422}[expected], (path, status, value)
        return value
    return call


def run_all(call, reference, mode='final', backend='sqlite'):
    config = dict(schema_version=1, core=dict(labeling=reference, result_mode=mode, output_interval_ns=1_000_000_000),
                  dns=dict(primary_label_ids=[1002]), http1=dict(primary_label_ids=[1003]), tls=dict(primary_label_ids=[1004]))
    sql = ("SELECT * FROM pcapfile.fixture USING npm.basic WITH features='basic,session,labeling,dns,http1,tls,icmp', "
           "observing='dns_transaction_ipv4', parameters='" + json.dumps(config, separators=(',', ':')) +
           "' INTO " + backend + ".example")
    return call('/scheduler/batch/execute', dict(sql=sql))


def check_all(call, root, result, mode):
    assert result['status'] == 'completed', result
    used = resolve(call, result['contract_checks'][0]['use_ref'])
    context = used['execution_context']['npm']
    assert context['run_id'] != used['run_id']
    assert used['normalized_config']['core']['result_mode'] == mode
    assert used['execution_context']['label_selection_state'] == 'checked'
    assert next(o for o in used['outputs'] if o['port_id'] == 'managed_results')['state'] == 'not_observed'
    entities = {item['entity_id']: item for item in context['entities']}
    assert len(entities) == 12 and all(item['description_state'] == 'published' for item in entities.values())
    relations = [context['run_catalog']] + [r for item in entities.values() for r in item['relations']]
    assert len(relations) == len({r['locator']['table'] for r in relations}) == 37
    published = {}
    with sqlite3.connect(root / 'input.db') as database:
        database.row_factory = sqlite3.Row
        actual_run = database.execute('SELECT * FROM npm_result_runs WHERE run_id=?', (context['run_id'],)).fetchone()
        assert actual_run and actual_run['status'] == 'completed'
        assert actual_run['input_namespace'] == context['input_namespace']
        for relation in relations:
            table = relation['locator']['table']
            value = describe(call, relation['locator'])
            assert value['basis']['dictionary_ref'] == relation['dictionary_ref']
            assert value['basis']['binding_ref'] == relation['binding_ref']
            dictionary = value['description']['document']
            assert dictionary == resolve(call, relation['dictionary_ref'])
            binding = resolve(call, relation['binding_ref'])
            assert binding['dictionary_ref'] == relation['dictionary_ref']
            physical = list(database.execute('PRAGMA table_xinfo("' + table + '")'))
            assert [f['name'] for f in dictionary['fields']] == [r['name'] for r in physical]
            for field, actual in zip(dictionary['fields'], physical):
                assert field['type']['system'] == 'database' and field['type']['name'] == actual['type']
                assert field['nullable'] == (not bool(actual['notnull']))
            if relation['view'] == 'runs':
                for name in ['run_id', 'task_id', 'input_namespace', 'status']:
                    assert next(f for f in dictionary['fields'] if f['name'] == name)['definition']['state'] == 'declared'
            else:
                assert all(f['definition']['state'] == 'declared' for f in dictionary['fields'])
                count = database.execute('SELECT COUNT(*) FROM "' + table + '" WHERE __npm_run_id=?', (context['run_id'],)).fetchone()[0]
                assert count > 0, (table, context)
            published[table] = relation
        assert sum(len(describe(call, item['relations'][0]['locator'])['description']['document']['fields']) - 3
                   for item in entities.values()) == 358
    # The existing database browser must expose every public NPM view, using the
    # exact table catalog returned by the data producer rather than naming inference.
    tables = call('/channels/database/tables', dict(type='sqlite', name='example'))['tables']
    assert all(table in tables for table in published), tables
    before = semantic(call, 'list', {})
    for relation in relations:
        describe(call, relation['locator'])
    assert semantic(call, 'list', {}) == before
    return used, published


def static_existing_case(executable, source_root, backend, evidence, node=None):
    """A fresh metadata service explains an existing database without a new NPM run."""
    with tempfile.TemporaryDirectory(prefix='flowsql-npm-static-') as temporary:
        root = Path(temporary)
        (root / 'frames').mkdir()
        write_all_capture(root)
        if backend == 'sqlite':
            shutil.copy2(source_root / 'input.db', root / 'input.db')
        with host_process(executable, root) as call:
            before = semantic(call, 'list', {})
            run_count = call('/test/database/query', dict(sql='SELECT count(*) AS n FROM npm_result_runs'))['data']
            objects = []
            for module in ['basic', 'session', 'dns_transaction', 'http1_transaction', 'tls_handshake', 'icmp_event']:
                for family in [4, 6]:
                    entity = f'{module}_ipv{family}'
                    objects.extend([f'npm_{entity}_{view}_v1' for view in ['history', 'latest', 'final']])
                    objects.append(f'npm_{entity}_v1_data')
            objects.extend(['npm_result_runs', 'npm_result_entities'])
            for table in objects:
                value = describe(call, dict(kind='database_table', type=backend, name='example', table=table))
                assert value['description']['state'] == 'present' and value['description']['origin'] == 'producer_default', value
                assert value['basis']['dictionary_ref'] is None and value['basis']['binding_ref'] is None
                assert value['basis']['contract_ref'] and value['basis']['component_revision']
                dictionary = value['description']['document']
                assert [f['name'] for f in dictionary['fields']] == [f['name'] for f in value['structure']['fields']]
                for field, physical in zip(dictionary['fields'], value['structure']['fields']):
                    assert field['type'] == physical['type'] and field['nullable'] == physical['nullable']
                    assert field['definition']['state'] == 'declared'
            assert len(objects) == 50 and semantic(call, 'list', {}) == before
            assert call('/test/database/query', dict(sql='SELECT count(*) AS n FROM npm_result_runs'))['data'] == run_count
            assert not semantic(call, 'list', dict(kind='operator_use'))['items']
        if node:
            with running_contract_host(executable, root, npm=True) as (base, _, log):
                result = subprocess.run([node, str(Path(__file__).with_name('npm_semantics_browser.mjs')), base, str(evidence)],
                                        capture_output=True, text=True, timeout=100)
                (evidence / 'static-browser.log').write_text(result.stdout + result.stderr)
                assert result.returncode == 0, result.stdout + result.stderr + log.read_text()[-3000:]
        print(f'PASS {backend}: existing 50 objects expose native static dictionaries without new tasks, runs or metadata writes', flush=True)


def main():
    executable = Path(sys.argv[1]).resolve()
    evidence = ROOT / 'evidence/npm-data-semantics/t5-completion'
    evidence.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='flowsql-npm-http-') as temporary:
        root = Path(temporary)
        write_all_capture(root)
        with running_contract_host(executable, root, npm=True) as (base, process, log):
            call = http_client(base, process)
            labels = publish_labels(call, ['dns', 'http1', 'tls'])
            first = run_all(call, labels)
            used, published = check_all(call, root, first, 'final')
            print('PASS: HTTP structure and exact references for 37 objects', flush=True)
            original_use = resolve(call, first['contract_checks'][0]['use_ref'])
            original_contract = resolve(call, used['contract_ref'])
            relation = published['npm_dns_transaction_ipv4_final_v1']
            original_dictionary = resolve(call, relation['dictionary_ref'])
            node = subprocess.run([sys.argv[2], str(Path(__file__).with_name('npm_semantics_browser.mjs')), base, str(evidence)],
                                  capture_output=True, text=True, timeout=100)
            (evidence / 'browser.log').write_text(node.stdout + node.stderr)
            assert node.returncode == 0, node.stdout + node.stderr + log.read_text()[-3000:]
            print('PASS: existing Chromium page read and local edit', flush=True)
            manual = describe(call, relation['locator'])
            definition = lambda d: next(f for f in d['fields'] if f['name'] == 'latency_ns')['definition']['value']
            assert definition(manual['description']['document']) == '人工补充：捕获点可配对 DNS 时延，结合运行与有效性解释。'
            assert definition(resolve(call, relation['dictionary_ref'])) == definition(original_dictionary)
            assert resolve(call, used['contract_ref']) == original_contract
            second = run_all(call, labels)
            second_use, second_published = check_all(call, root, second, 'final')
            print('PASS: repeated run with manual description preserved', flush=True)
            assert second_use['execution_context']['npm']['run_id'] != used['execution_context']['npm']['run_id']
            assert resolve(call, first['contract_checks'][0]['use_ref']) == original_use
            assert definition(describe(call, relation['locator'])['description']['document']) == definition(manual['description']['document'])
            assert second_published['npm_dns_transaction_ipv4_final_v1']['dictionary_ref'] != relation['dictionary_ref']
            # Object-local edits keep the two other views and the other family unchanged.
            for table in ['npm_dns_transaction_ipv4_history_v1', 'npm_dns_transaction_ipv6_final_v1']:
                assert resolve(call, published[table]['dictionary_ref']) == describe(call, published[table]['locator'])['description']['document']
            (evidence / 'http-runs.json').write_text(json.dumps([first, second], ensure_ascii=False, indent=2))
        (evidence / 'http.log').write_text((root / 'host.log').read_text())
        # The same persisted object is read via IID routes after an actual host restart.
        with host_process(executable, root) as call:
            for relation in second_published.values():
                value = describe(call, relation['locator'])
                assert value['basis']['dictionary_ref'] == relation['dictionary_ref']
                assert value['description']['document'] == resolve(call, relation['dictionary_ref'])
            assert resolve(call, first['contract_checks'][0]['use_ref']) == original_use
        (evidence / 'iid-restart.log').write_text((root / 'host.log').read_text())
        print('PASS: precise IID reread after host restart', flush=True)
        write_all_capture(root, pcapng=True)
        with running_contract_host(executable, root, npm=True) as (base, process, log):
            call = http_client(base, process)
            third = run_all(call, labels, 'periodic_snapshot')
            third_use, third_published = check_all(call, root, third, 'periodic_snapshot')
            assert third_use['execution_context']['npm']['run_id'] not in [used['execution_context']['npm']['run_id'],
                                                                          second_use['execution_context']['npm']['run_id']]
            assert resolve(call, first['contract_checks'][0]['use_ref']) == original_use
            assert third_published == second_published
            (evidence / 'pcapng-run.json').write_text(json.dumps(third, ensure_ascii=False, indent=2))
        (evidence / 'http-restart.log').write_text((root / 'host.log').read_text())
        static_existing_case(executable, root, 'sqlite', evidence, sys.argv[2])
    print('PASS: 12 entities/358 fields/37 objects, HTTP/IID/Chromium, manual preservation, exact history, PCAPNG and restart')


if __name__ == '__main__':
    main()
