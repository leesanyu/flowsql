"""Actual database plugin/HTTP and Graphic Walker acceptance for all new connectors."""
import contextlib
from collections import Counter
from decimal import Decimal
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import time
import uuid

from test_entry import port
from test_compute import call

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools'))
from test_environment.config import Config


def main():
    output, node = Path(sys.argv[1]).resolve(), sys.argv[2]
    config = Config()
    table = 'vis_e2e_' + uuid.uuid4().hex[:12]
    evidence = ROOT / 'evidence/visualization-statistics/connectors'
    evidence.mkdir(parents=True, exist_ok=True)
    fixture = output / 'test_visualization_connectors_fixture'
    checks = []
    def passed(name):
        checks.append(name)
        print('PASS', name, flush=True)
    def prepare(mode, backend):
        result = subprocess.run([str(fixture), mode, backend, table], check=True, capture_output=True, text=True, timeout=30)
        if mode in ('baseline', 'numeric_baseline', 'statistics_baseline'):
            return json.loads(next(line[9:] for line in result.stdout.splitlines() if line.startswith('BASELINE ')))
    backends = ['mysql', 'postgres', 'clickhouse']
    try:
        for backend in backends:
            prepare('seed', backend)
        baselines = {backend: prepare('baseline', backend) for backend in backends}
        numeric_baselines = {backend: prepare('numeric_baseline', backend) for backend in backends}
        statistics_baselines = {backend: prepare('statistics_baseline', backend) for backend in backends}
        (evidence/'statistics-native-baselines.json').write_text(json.dumps(statistics_baselines, indent=2))
        with tempfile.TemporaryDirectory(prefix='flowsql-connectors-e2e-') as temporary:
            directory = Path(temporary)
            database = directory / 'data.db'
            with sqlite3.connect(database) as connection:
                connection.execute('CREATE TABLE example(host TEXT,value REAL)')
                connection.execute("INSERT INTO example VALUES('sqlite',1)")
            channels = directory / 'channels.json'
            data = [dict(type='sqlite', name='local', path=str(database), readonly='true')]
            for backend in backends:
                values = config.database(backend, 'driver', os.environ.get('FLOWSQL_TEST_LOCATION', 'native'))
                data.append(dict(type=backend, name=backend, **values))
            channels.write_text(json.dumps(dict(channels=dict(database_channels=data))))
            channels.chmod(0o600)
            original = hashlib.sha256(channels.read_bytes()).hexdigest()
            plugins = f'{output}/libflowsql_database.so:config_file={channels},{output}/libflowsql_visualization.so'
            base = f'http://127.0.0.1:{port()}'
            options = json.dumps(dict(port=int(base.rsplit(':', 1)[1]), static_dir=str(output / 'exploration')))
            env = dict(os.environ, LD_LIBRARY_PATH=str(output) + ':' + os.environ.get('LD_LIBRARY_PATH', ''))
            @contextlib.contextmanager
            def bounded_service(base, options):
                with (directory/'budget.log').open('w+') as log:
                    child = subprocess.Popen([str(output/'flowsql'), '--role','visualization','--plugins',plugins,
                        '--option',options], cwd=directory, env=env, stdout=log, stderr=log)
                    try:
                        deadline = time.monotonic() + 15
                        while True:
                            try:
                                if call(base,'capabilities')[0] == 200: break
                            except OSError: pass
                            if child.poll() is not None or time.monotonic() >= deadline:
                                raise AssertionError('budget service did not start')
                            time.sleep(.05)
                        yield
                    finally:
                        child.terminate()
                        try: child.wait(timeout=2)
                        except subprocess.TimeoutExpired:
                            child.kill(); child.wait(); raise AssertionError('budget service shutdown exceeded 2s')
                        assert child.returncode == 0
            with (directory / 'service.log').open('w+') as log:
                process = subprocess.Popen([str(output / 'flowsql'), '--role', 'visualization', '--plugins', plugins,
                    '--option', options], cwd=directory, env=env, stdout=log, stderr=log)
                try:
                    deadline = time.monotonic() + 15
                    while True:
                        try:
                            status, capabilities = call(base, 'capabilities')
                            if status == 200:
                                break
                        except OSError:
                            pass
                        if process.poll() is not None or time.monotonic() >= deadline:
                            raise AssertionError('isolated connector service failed to start')
                        time.sleep(.05)
                    assert capabilities['dialects'] == ['sqlite', *backends]
                    status, listing = call(base, 'sources')
                    assert status == 200, listing
                    all_sources = {}
                    for item in listing['sources']:
                        if item['dialect'] in backends and (item['label'].endswith('.' + table) or
                                                           item['label'].endswith('.' + table + '_statistics')):
                            status, source = call(base, 'sources/' + item['source_id'])
                            assert status == 200, source
                            all_sources[item['label']] = source
                    sources = {label: source for label, source in all_sources.items() if label.endswith('.' + table)}
                    assert len(sources) == 4, sources  # PG has an additional schema with the same table name.
                    assert len({source['source_id'] for source in sources.values()}) == 4
                    pg_other = next(source for label, source in sources.items() if table + '.schema.' in label)
                    assert pg_other['fields'][0]['label'] == 'host' and len(pg_other['fields']) == 1
                    passed('four_dialects_and_postgres_schema_identity')
                    for backend in backends:
                        label, source = next((label, source) for label, source in sources.items()
                            if source['dialect'] == backend and len(source['fields']) == 9)
                        assert source['fields'][7]['type'] == 'boolean', source
                        assert source['fields'][8]['type'] == 'unsupported', source
                        def compute(query):
                            return call(base, 'compute', dict(request_id=str(uuid.uuid4()), source_id=source['source_id'],
                                schema_revision=source['schema_revision'], workflow_version='graphic-walker-0.5.3',
                                workflow=dict(workflow=[dict(type='view', query=[query])]), detail_mode='complete'))
                        status, result = compute(dict(op='aggregate', groupBy=[], measures=[dict(field='f2', agg='mean', asFieldKey='avg')]))
                        assert status == 200 and result['rows'] == [{'avg': 15}], result
                        status, histogram = compute(dict(op='histogram', field='f2', bins=2))
                        assert status == 200 and [r['bin_count'] for r in histogram['rows']] == [1, 1], histogram
                        assert histogram['rows'][0]['bin_lower'] == 10 and histogram['rows'][1]['bin_upper'] == 20, histogram
                        assert compute(dict(op='histogram', field='f4', bins=10))[1]['rows'] == []
                        assert compute(dict(op='histogram', field='f5', bins=10))[1]['code'] == 'UNSUPPORTED_TYPE'
                        assert compute(dict(op='histogram', field='f0', bins=10))[1]['code'] == 'UNSUPPORTED_TYPE'
                        assert compute(dict(op='histogram', field='f2', bins=101))[1]['code'] == 'INVALID_WORKFLOW'
                        passed(backend + '_histogram_api_and_numeric_guard')
                        for field in ('f5', 'f8'):
                            assert compute(dict(op='raw', fields=[field]))[1]['code'] == 'UNSUPPORTED_TYPE'
                        status, decimal_result = compute(dict(op='raw', fields=['f6']))
                        assert status == 200 and decimal_result['numeric_display'][0]['mode'] == 'approximate', decimal_result
                        assert Decimal(decimal_result['numeric_display'][0]['exact_values'][0]) == Decimal('0.10000000000000000001')
                        status, decimal_mean = compute(dict(op='aggregate', groupBy=[], measures=[dict(field='f6', agg='mean', asFieldKey='avg')]))
                        assert status == 200 and decimal_mean['numeric_display'], decimal_mean
                        status, result = compute(dict(op='raw', fields=['f7']))
                        assert status == 200 and sorted(row['f7'] for row in result['rows']) == [False, True, True], result
                        passed(backend + '_native_api_and_type_protection')
                        report = evidence / (backend + '-browser.json')
                        subprocess.run([node, str(Path(__file__).with_name('compute_browser.mjs')), base,
                                        str(report), 'connector', label], check=True, timeout=120)
                        browser = json.loads(report.read_text())
                        assert browser['passed'] == browser['selected'] == 7 and not browser['failed']
                        assert len(browser['results']) == len(browser['resultWorkflows'])
                        def normalized(rows):
                            return Counter(tuple(sorted(row.items())) for row in rows)
                        seen = set()
                        for workflow, actual in zip(browser['resultWorkflows'], browser['results']):
                            query = next(step['query'][0] for step in workflow['workflow'] if step['type'] == 'view')
                            if query['op'] == 'raw':
                                index, fields = 2, ['f3', 'f2']
                            elif query['groupBy'] == ['f1']:
                                index, fields = 1, ['f1', 'f2_mean']
                            elif any(step['type'] == 'filter' for step in workflow['workflow']):
                                index, fields = 3, ['f0', 'f2_mean']
                            else:
                                index, fields = 0, ['f0', 'f2_mean']
                            expected = [dict(zip(fields, row)) for row in baselines[backend][index]]
                            assert normalized(actual['rows']) == normalized(expected), (backend, workflow, actual, expected)
                            seen.add(index)
                        assert seen == {0, 1, 2, 3}, seen
                        passed(backend + '_three_charts_filter_and_native_baselines')
                        statistics_label = label + '_statistics'
                        statistics_source = all_sources[statistics_label]
                        assert status == 200 and statistics_source['time_bucket_fields'] == ['f0','f3','f4'], statistics_source
                        assert statistics_source['semantics']['timezone'] == 'source_calendar'
                        def statistics(query, filters=None, **options):
                            steps = ([dict(type='filter',filters=filters)] if filters else []) + [dict(type='view',query=[query])]
                            return call(base,'compute',dict(request_id=str(uuid.uuid4()),source_id=statistics_source['source_id'],
                                schema_revision=statistics_source['schema_revision'],workflow_version='graphic-walker-0.5.3',
                                workflow=dict(workflow=steps,**options),detail_mode='complete'))
                        def stat_query(field='f0', unit='day'):
                            return dict(op='aggregate',groupBy=[dict(field=field,unit=unit,asFieldKey='bucket')],
                                measures=[dict(field='f1',agg='distinctCount',asFieldKey='n')])
                        for index, (field,unit) in enumerate((f,u) for f in ('f0','f3','f4') for u in ('day','month')):
                            status, actual = statistics(stat_query(field,unit))
                            expected = [dict(zip(['bucket','n'], row)) for row in statistics_baselines[backend][index]]
                            assert status == 200 and normalized(actual['rows']) == normalized(expected), (backend,field,unit,actual,expected)
                        # Exact counts operate on source values, including adjacent integers and decimals
                        # that would collapse to one value if converted to JavaScript numbers first.
                        for field, expected in [('f1',3),('f2',5),('f5',0),('f6',2),('f7',2)]:
                            status, actual = statistics(dict(op='aggregate',groupBy=[],measures=[dict(field=field,agg='distinctCount',asFieldKey='n')]))
                            assert status == 200 and actual['rows'] == [{'n':expected}], (backend,field,actual)
                        selected = [dict(fid='f1',rule=dict(type='one of',value=['x']))]
                        assert statistics(stat_query(unit='month'),selected)[1]['rows'] == [{'bucket':'2024-02-01','n':1}]
                        empty = [dict(fid='f1',rule=dict(type='one of',value=['missing']))]
                        assert statistics(stat_query(),empty)[1]['rows'] == []
                        total = dict(op='aggregate',groupBy=[],measures=[dict(field='f1',agg='distinctCount',asFieldKey='n')])
                        assert statistics(total,empty)[1]['rows'] == [{'n':0}]
                        status, top = statistics(stat_query(),limit=1)
                        assert status == 200 and top['scope'] == 'top_n' and len(top['rows']) == 1
                        assert statistics(stat_query(unit='week'))[1]['code'] == 'UNSUPPORTED_OPERATION'
                        assert statistics(stat_query(field='f1'))[1]['code'] == 'UNSUPPORTED_TYPE'
                        passed(backend + '_statistics_native_calendar_null_exact_distinct_and_filters')
                        report = evidence/(backend+'-statistics-browser.json')
                        subprocess.run([node,str(Path(__file__).with_name('statistics_browser.mjs')),base,'-',statistics_label,str(report)],
                            check=True,timeout=100)
                        statistics_browser = json.loads(report.read_text())
                        assert statistics_browser['selected'] == statistics_browser['executed'] == statistics_browser['passed'] == 6
                        assert statistics_browser['failed'] == statistics_browser['skipped'] == statistics_browser['not_executed'] == 0
                        passed(backend + '_statistics_browser_and_error_recovery')
                        numeric_label = label + '_numeric'
                        item = next(s for s in listing['sources'] if s['label'] == numeric_label)
                        status, numeric_source = call(base, 'sources/' + item['source_id'])
                        assert status == 200, numeric_source
                        def numeric(query, filters=None, endpoint=base):
                            steps = ([dict(type='filter', filters=filters)] if filters else []) + [dict(type='view', query=[query])]
                            return call(endpoint, 'compute', dict(request_id=str(uuid.uuid4()), source_id=numeric_source['source_id'],
                                schema_revision=numeric_source['schema_revision'], workflow_version='graphic-walker-0.5.3',
                                workflow=dict(workflow=steps), detail_mode='complete'))
                        status, projection = numeric(dict(op='raw', fields=['f0','f1']))
                        assert status == 200, projection
                        display = projection['numeric_display'][0]
                        assert display['mode'] == 'approximate' and display['origin'] == 'decimal'
                        assert len(display['exact_values']) == len(projection['rows']) == 5
                        actual = {row['f0']: (row['f1'], exact) for row, exact in zip(projection['rows'], display['exact_values'])}
                        expected = dict(numeric_baselines[backend][0])
                        for name, exact in expected.items():
                            assert actual[name][1] == exact, (backend, name, actual, expected)
                            assert actual[name][0] == (None if exact is None else float(exact))
                        assert Decimal(actual['tenth'][1]) == Decimal('0.1')
                        assert Decimal(actual['precision'][1]) == Decimal('0.10000000000000000001')
                        only_exact = [dict(fid='f0',rule=dict(type='one of', value=['exact']))]
                        status, exact = numeric(dict(op='raw', fields=['f1']), only_exact)
                        assert status == 200 and exact['numeric_display'][0]['mode'] == 'exact', exact
                        for index, aggregate in enumerate(('sum','mean','min','max'), 1):
                            status, value = numeric(dict(op='aggregate',groupBy=[],measures=[dict(field='f1',agg=aggregate,asFieldKey='n')]))
                            assert status == 200, value
                            native = numeric_baselines[backend][index][0][0]
                            assert value['rows'][0]['n'] == float(native), (backend,aggregate,value,native)
                            metadata = value['numeric_display'][0]
                            if backend == 'clickhouse' and aggregate == 'mean':
                                assert metadata['origin'] == 'native_decimal_mean' and metadata['mode'] == 'approximate'
                                assert 'exact_values' not in metadata
                            else:
                                assert metadata['origin'] == 'decimal' and Decimal(metadata['exact_values'][0]) == Decimal(native)
                        for aggregate in ('mean','sum','min','max'):
                            assert numeric(dict(op='aggregate',groupBy=[],measures=[dict(field='f2',agg=aggregate,asFieldKey='n')]))[1]['code'] == 'UNSUPPORTED_TYPE'
                        # A wide integer outside the filtered scope must not poison a safe aggregate.
                        safe = [dict(fid='f0',rule=dict(type='not in', value=['unsafe']))]
                        status, safe_mean = numeric(dict(op='aggregate',groupBy=[],measures=[dict(field='f2',agg='mean',asFieldKey='n')]), safe)
                        assert status == 200 and safe_mean['rows'] == [{'n':2}], safe_mean
                        passed(backend + '_decimal_native_precision_null_and_filtered_integer_protection')
                        # Select the exact same Decimal rows with a byte budget that admits ordinary rows,
                        # but cannot admit the complete response with precision metadata.
                        reduced = dict(projection); reduced.pop('numeric_display'); reduced['elapsed_ms'] = 0
                        budget = len(json.dumps(reduced, separators=(',',':'), ensure_ascii=False).encode()) + 96
                        budget_base = f'http://127.0.0.1:{port()}'
                        budget_options = json.dumps(dict(port=int(budget_base.rsplit(':',1)[1]), static_dir=str(output/'exploration'), limits=dict(max_bytes=budget)))
                        with bounded_service(budget_base, budget_options):
                            assert call(budget_base,'sources')[0] == 200
                            status, bounded = numeric(dict(op='raw', fields=['f0','f1']), endpoint=budget_base)
                            assert status == 413 and bounded['code'] == 'RESULT_LIMIT' and '精度元信息' in bounded['message'], bounded
                        passed(backend + '_precision_metadata_byte_budget')
                        subprocess.run([node, str(Path(__file__).with_name('numeric_browser.mjs')), base,
                                        numeric_label, str(evidence/(backend+'-numeric-browser.json'))], check=True, timeout=100)
                        passed(backend + '_decimal_browser_and_error_recovery')

                    assert hashlib.sha256(channels.read_bytes()).hexdigest() == original
                    passed('shared_configuration_unchanged')
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        process.kill(); process.wait(); raise AssertionError('connector service shutdown exceeded 2s')
                    assert process.returncode == 0
    finally:
        cleanup_errors = []
        for backend in backends:
            try:
                prepare('cleanup', backend)
            except subprocess.CalledProcessError:
                cleanup_errors.append(backend)
        assert not cleanup_errors, ('fixture cleanup failed', cleanup_errors)
    passed('isolated_tables_schema_and_process_reclaimed')
    assert len(checks) == 27
    (evidence / 'e2e.json').write_text(json.dumps(dict(selected=27, executed=27, passed=27, failed=0, skipped=0,
        not_executed=0, checks=checks), indent=2))


if __name__ == '__main__':
    main()
