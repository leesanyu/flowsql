# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Four actual database drivers: result identity, operations and immutable explanations."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import uuid
from test_metadata_description_api import call, describe, request, validate, ROOT
from test_operator_contract_api import running_contract_host
sys.path.insert(0, str(ROOT / 'tools'))
from test_environment.config import Config


def main():
    host = Path(sys.argv[1]).resolve()
    fixture = host.parent / 'test_visualization_connectors_fixture'
    table = 'vis_e2e_' + uuid.uuid4().hex[:12]
    backends = ['mysql', 'postgres', 'clickhouse']
    prepared = []
    def prepare(mode, backend):
        subprocess.run([str(fixture), mode, backend, table], check=True, capture_output=True, timeout=30)
    try:
        for backend in backends:
            prepare('seed', backend)
            prepared.append(backend)
        with tempfile.TemporaryDirectory(prefix='flowsql-semantic-queries-') as temporary:
            directory = Path(temporary)
            channels = [dict(type=backend, name=backend, dataset_instance_id=uuid.uuid4().hex,
                **Config().database(backend, 'driver', os.environ.get('FLOWSQL_TEST_LOCATION', 'native')))
                for backend in backends]
            config = directory / 'database.yml'
            config.write_text(json.dumps(dict(channels=dict(database_channels=channels))))
            config.chmod(0o600)
            with running_contract_host(host, directory) as (base, _, log):
                def execute(sql):
                    status, result = request(base + '/api/tasks/batch/execute', dict(sql_text=sql, mode='sync'))
                    assert status == 200 and result['status'] == 'completed', (sql, result)
                    validate('semanticContext', result['semantic_context'])
                    return result
                for backend in ['sqlite', *backends]:
                    name, relation = ('example', 'one') if backend == 'sqlite' else (backend, table)
                    locator = dict(kind='database_table', type=backend, name=name, table=relation)
                    source = f'{backend}.{name}.{relation}'
                    observed = call(base, 'describe-sql', dict(sql=f'DESC CHANNEL {backend}.{name} TABLE {relation}'))
                    saved = call(base, 'channels/save-description', dict(locator=locator, basis=observed['basis'],
                        editor_changes=dict(fields=dict(value=dict(definition='Captured delay', unit='ms'))),
                        confirmations=observed['checks'].get('required_confirmations', [])))
                    originals = []
                    for projection, condition, operation in [('*', ' WHERE 1=0', 'identity'),
                            ('value AS renamed', '', 'alias'), ('count(value) AS n', '', 'aggregate'),
                            ('sum(value) AS total', '', 'aggregate'), ('value+1 AS value', '', 'unknown')]:
                        result = execute(f'SELECT {projection} FROM {source}{condition}')
                        context = result['semantic_context']
                        assert context['fields'] and context['fields'][0]['operation']['kind'] == operation, result
                        if condition: assert result['result_values'] == [] and len(result['result_columns']) > 1
                        if projection.startswith('count'): assert context['fields'][0]['result_unit'] == '个'
                        if projection.startswith('sum'): assert context['fields'][0]['result_unit'] is None
                        if operation == 'alias':
                            assert context['fields'][0]['state'] == 'known', context
                            assert context['sources'][0]['basis']['dictionary_ref'] == saved['dictionary_ref']
                        if operation == 'unknown': assert context['fields'][0]['source_field_id'] is None
                        originals.append(result)
                    observed = describe(base, locator)
                    call(base, 'channels/save-description', dict(locator=locator, basis=observed['basis'],
                        editor_changes=dict(fields=dict(value=dict(definition='New meaning'))),
                        confirmations=observed['checks'].get('required_confirmations', [])))
                    for original in originals:
                        status, historical = request(base + '/api/tasks/result', dict(task_id=original['task_id']))
                        assert status == 200 and historical['semantic_context'] == original['semantic_context']
                    print('PASS', backend, 'zero rows, alias, count, sum, unknown and historical identity', flush=True)
    finally:
        for backend in reversed(prepared): prepare('cleanup', backend)


if __name__ == '__main__': main()
