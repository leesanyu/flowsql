# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Real NPM packets, native production databases, public metadata and exact history."""
import json
import os
from pathlib import Path
import sys
import tempfile

from test_data_semantics_api import ROOT
from test_operator_contract_api import running_contract_host
from test_npm_semantics_http import http_client, run_all, write_all_capture, static_existing_case
from test_npm_semantics_pipeline import describe, host_process, publish_labels, resolve, semantic


def rows(call, backend, table, run_id, run_catalog=False):
    # Identifiers come from this run's catalog; run IDs are producer-generated.
    column = 'run_id' if run_catalog else '__npm_run_id'
    projection = 'DISTINCT run_id,task_id,input_namespace,status' if run_catalog else '*'
    terminal = " AND status='completed'" if run_catalog else ''
    result = call('/test/database/query', dict(
        sql=f"SELECT {projection} FROM {table} WHERE {column}='{run_id}'{terminal}"))
    assert result['status'] == 'completed' and result['rows'] > 0, (table, result)
    data = result['data']
    assert len(data['data']) == result['rows'], result
    return [dict(zip(data['columns'], row)) for row in data['data']]


def check(call, backend, result, mode, read_data=True):
    assert result['status'] == 'completed', result
    used = resolve(call, result['contract_checks'][0]['use_ref'])
    context = used['execution_context']['npm']
    assert used['normalized_config']['core']['result_mode'] == mode
    entities = {item['entity_id']: item for item in context['entities']}
    assert len(entities) == 12 and all(e['description_state'] == 'published' for e in entities.values())
    relations = [context['run_catalog']] + [r for e in entities.values() for r in e['relations']]
    assert len(relations) == len({r['locator']['table'] for r in relations}) == 37
    tables = call('/channels/database/tables', dict(type=backend, name='example'))['tables']
    assert all(r['locator']['table'] in tables for r in relations), tables
    native_uint64 = {'mysql': 'bigint unsigned', 'postgres': 'numeric(20,0)', 'clickhouse': 'UInt64'}[backend]
    native_int64 = {'mysql': 'bigint', 'postgres': 'bigint', 'clickhouse': 'Nullable(Int64)'}[backend]
    published = {}
    dictionaries, observations = {}, {}
    field_count = 0
    before = semantic(call, 'list', {})
    for relation in relations:
        locator = relation['locator']
        assert locator['type'] == backend
        value = describe(call, locator)
        dictionary = value['description']['document']
        dictionaries[locator['table']] = dictionary
        assert value['structure']['schema_state'] == 'known'
        assert value['basis']['dictionary_ref'] == relation['dictionary_ref']
        assert value['basis']['binding_ref'] == relation['binding_ref']
        assert dictionary == resolve(call, relation['dictionary_ref'])
        assert resolve(call, relation['binding_ref'])['dictionary_ref'] == relation['dictionary_ref']
        actual = value['structure']['fields']
        assert [f['name'] for f in dictionary['fields']] == [f['name'] for f in actual]
        for field, physical in zip(dictionary['fields'], actual):
            assert field['type'] == physical['type'] and field['nullable'] == physical['nullable']
            assert field['type']['system'] == 'database' and field['type']['parameters']['dialect'] == backend
            # The spec declares four required run-catalog meanings; lifecycle
            # helper columns retain their physical structure and unknown meaning.
            expected = 'declared' if relation['view'] != 'runs' or field['name'] in (
                'run_id', 'task_id', 'input_namespace', 'status') else 'unknown'
            assert field['definition']['state'] == expected, field
        if relation['view'] != 'runs':
            fields = {f['name']: f for f in dictionary['fields']}
            assert fields['revision']['type']['name'] == native_uint64, fields['revision']
            if 'latency_ns' in fields:
                assert fields['latency_ns']['type']['name'] == native_int64 and fields['latency_ns']['nullable']
            links = dictionary['entity']['relations']['value']
            assert links[0]['dictionary_ref'] == context['run_catalog']['dictionary_ref']
            assert links[0]['field_map'] == {'__npm_run_id': 'run_id'}
            if relation['view'] == 'final':
                field_count += len(fields) - 3
        if read_data:
            observed = rows(call, backend, locator['table'], context['run_id'], relation['view'] == 'runs')
            observations[locator['table']] = observed
            if relation['view'] == 'runs':
                assert len(observed) == 1 and observed[0]['status'] == 'completed'
                assert observed[0]['input_namespace'] == context['input_namespace']
            else:
                assert all(r['__npm_run_status'] == 'completed' for r in observed)
                assert all(r['__npm_run_id'] == context['run_id'] for r in observed)
        published[locator['table']] = relation
    assert field_count == 358
    assert semantic(call, 'list', {}) == before, 'description reads must not publish metadata'
    for entity_id, entity in entities.items():
        module, family = entity_id.rsplit('_ipv', 1)
        target = 'basic' if module == 'session' else 'session' if module in ('dns_transaction', 'http1_transaction', 'tls_handshake') else None
        event_sets = []
        for relation in entity['relations']:
            dictionary = dictionaries[relation['locator']['table']]
            links = dictionary['entity']['relations']['value']
            assert len(links) == (2 if target else 1)
            if target:
                peer = next(r for r in entities[f'{target}_ipv{family}']['relations'] if r['view'] == relation['view'])
                assert links[1]['dictionary_ref'] == peer['dictionary_ref']
                assert links[1]['field_map'] == {k: k for k in ['__npm_run_id', 'session_id', 'observation_domain_id']}
            if read_data:
                observed = observations[relation['locator']['table']]
                identity = 'session_id' if module in ('basic', 'session') else 'entity_instance_id'
                event_sets.append({str(r[identity]) for r in observed})
                if relation['view'] == 'final' and module == 'dns_transaction':
                    dns = {int(r['dns_id']): r for r in observed}
                    assert int(dns[1]['latency_ns']) == 10_000_000 and int(dns[1]['response_rcode']) == 3
                    assert dns[2]['outcome'] == 'query_only' and dns[2]['latency_ns'] is None
                if relation['view'] == 'final' and module == 'http1_transaction':
                    assert any(int(r['status_code']) == 404 for r in observed if r['status_code'] is not None)
                if relation['view'] == 'final' and module == 'tls_handshake':
                    assert any(r['incomplete_reason'] == 'encrypted_after_server_hello' for r in observed)
        if read_data:
            assert event_sets[0] == event_sets[1] == event_sets[2], (entity_id, event_sets)
    return used, published


def backend_case(executable, backend, evidence):
    os.environ['FLOWSQL_TEST_NPM_BACKEND'] = backend
    with tempfile.TemporaryDirectory(prefix=f'flowsql-npm-{backend}-') as temporary:
        root = Path(temporary)
        (root / 'frames').mkdir()
        (root / 'frames/lazy.csv').write_text('latency\n1000\n')
        write_all_capture(root)
        with host_process(executable, root) as call:
            labels = publish_labels(call, ['dns', 'http1', 'tls'])
            first = run_all(call, labels, backend=backend)
            used, published = check(call, backend, first, 'final')
            original_use = resolve(call, first['contract_checks'][0]['use_ref'])
            original_contract = resolve(call, used['contract_ref'])
            relation = published['npm_dns_transaction_ipv4_final_v1']
            original_dictionary = resolve(call, relation['dictionary_ref'])
            current = describe(call, relation['locator'])
            semantic(call, 'channels/save-description', dict(
                locator=relation['locator'], basis=current['basis'],
                editor_changes=dict(fields={'latency_ns': dict(definition='人工补充：捕获点 DNS 配对时延。')}),
                confirmations=current['checks'].get('required_confirmations', [])))
            manual = describe(call, relation['locator'])['description']['document']
            second = run_all(call, labels, backend=backend)
            second_use, second_published = check(call, backend, second, 'final', read_data=False)
            assert used['execution_context']['npm']['run_id'] != second_use['execution_context']['npm']['run_id']
            assert describe(call, relation['locator'])['description']['document'] == manual
            assert resolve(call, relation['dictionary_ref']) == original_dictionary
            assert resolve(call, used['contract_ref']) == original_contract
            assert resolve(call, first['contract_checks'][0]['use_ref']) == original_use
        (evidence / f'{backend}-local.log').write_text((root / 'host.log').read_text())
        write_all_capture(root, pcapng=True)
        with running_contract_host(executable, root, npm=True) as (base, process, _):
            call = http_client(base, process)
            for item in second_published.values():
                value = describe(call, item['locator'])
                assert value['basis']['dictionary_ref'] == item['dictionary_ref']
                assert value['description']['document'] == resolve(call, item['dictionary_ref'])
            third = run_all(call, labels, 'periodic_snapshot', backend)
            third_use, third_published = check(call, backend, third, 'periodic_snapshot', read_data=False)
            assert third_published == second_published
            assert resolve(call, first['contract_checks'][0]['use_ref']) == original_use
        (evidence / f'{backend}-http.log').write_text((root / 'host.log').read_text())
        with host_process(executable, root) as call:
            check(call, backend, third, 'periodic_snapshot')
            assert describe(call, relation['locator'])['description']['document'] == manual
            assert resolve(call, first['contract_checks'][0]['use_ref']) == original_use
        (evidence / f'{backend}-restart.log').write_text((root / 'host.log').read_text())
        (evidence / f'{backend}-runs.json').write_text(json.dumps([first, second, third], ensure_ascii=False, indent=2))
        static_existing_case(executable, root, backend, evidence)
    print(f'PASS {backend}: 12 entities, 37 objects, 358 fields, native types/NULL, exact relations, manual/history, HTTP/PCAPNG/restart', flush=True)


if __name__ == '__main__':
    evidence = ROOT / 'evidence/npm-data-semantics/t6-backends'
    evidence.mkdir(parents=True, exist_ok=True)
    assert len(sys.argv) == 3 and sys.argv[2] in ('mysql', 'postgres', 'clickhouse')
    backend_case(Path(sys.argv[1]).resolve(), sys.argv[2], evidence)
