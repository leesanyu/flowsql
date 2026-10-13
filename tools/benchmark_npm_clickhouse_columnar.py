#!/usr/bin/env python3
"""Compare the frozen NPM sink against the budgeted columnar sink; preserve raw runs."""
import argparse
import csv
import hashlib
import io
import json
import statistics
import subprocess
import time
import urllib.parse
import urllib.request
from pathlib import Path


def summary(values):
    return dict(median=statistics.median(values), min=min(values), max=max(values))


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--optimized', type=Path, required=True)
    parser.add_argument('--options', type=Path, required=True)
    parser.add_argument('--channel', default='t5clickhouse')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--repeats', type=int, default=7)
    parser.add_argument('--cpu', type=int, default=2)
    args = parser.parse_args()
    assert args.repeats >= 7
    result = dict(cpu=args.cpu, repeats=args.repeats, utc=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                  runs=[], summaries=[], failures=[], blocked_cases=[])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    raw_directory = args.output.with_suffix('') / 'raw'
    raw_directory.mkdir(parents=True, exist_ok=True)

    def save():
        args.output.write_text(json.dumps(result, indent=2))

    parameters = dict(part.split('=', 1) for part in args.options.read_text().strip().split(';'))

    def query(sql):
        url = 'http://' + parameters['host'] + ':' + parameters['port'] + '/?' + urllib.parse.urlencode(
            {'database': parameters['database']})
        headers = {'X-ClickHouse-User': parameters.get('user', 'default'),
                   'X-ClickHouse-Key': parameters.get('password', '')}
        request = urllib.request.Request(url, data=(sql + ' FORMAT JSONEachRow').encode(), headers=headers)
        with urllib.request.urlopen(request, timeout=20) as response:
            return [json.loads(line) for line in response.read().decode().splitlines() if line]

    def server_state():
        try:
            return dict(
                version=query('SELECT version() AS version'),
                settings=query("SELECT name,value FROM system.settings WHERE name IN "
                               "('async_insert','wait_for_async_insert','input_format_values_interpret_expressions')"),
                parts=query("SELECT table,count() AS active_parts,sum(rows) AS rows,sum(bytes_on_disk) AS bytes "
                            "FROM system.parts WHERE active AND database=currentDatabase() "
                            "AND startsWith(table,'npm_') GROUP BY table ORDER BY table"),
                merges=query("SELECT count() AS active_merges FROM system.merges WHERE database=currentDatabase()"))
        except Exception as error:
            return dict(observation_error=str(error))

    result['server_before'] = server_state()

    def run(binary, params, kind, label, repeat):
        command = ['taskset', '-c', str(args.cpu), str(binary), *params]
        completed = subprocess.run(command, capture_output=True, text=True)
        item = dict(kind=kind, label=label, repeat=repeat, command=command,
                    binary_sha256=digest(binary), exit_code=completed.returncode)
        prefix = raw_directory / f'{kind}-{label}-{repeat}'
        prefix.with_suffix('.stdout').write_text(completed.stdout)
        prefix.with_suffix('.stderr').write_text(completed.stderr)
        item['raw_prefix'] = str(prefix)
        if completed.returncode:
            item['stderr'] = completed.stderr[-6000:]
            item['stdout_tail'] = completed.stdout[-6000:]
            result['failures'].append(item)
            # The unchanged legacy URL adapter failed this size during the diagnostic run;
            # a separate request with the same parameter count received HTTP 400 with no body.
            if kind == 'bulk-16384' and label == 'values' and completed.returncode == 1 and \
                    completed.stderr.strip() == 'columnar probe failed:':
                item['blocked_reason'] = 'Legacy URL-parameter batch rejected; HTTP 400 empty-body diagnostic retained'
                result['blocked_cases'].append(item)
                save()
                return item
            save()
            raise RuntimeError('measurement failed; see saved raw failure')
        fields = None
        for line in completed.stdout.splitlines():
            if line.startswith('measurement,family,'):
                fields = next(csv.reader(io.StringIO(line)))
            elif line.startswith('measurement,') and fields:
                item['measurement'] = dict(zip(fields, next(csv.reader(io.StringIO(line)))))
            elif line.startswith(('managed_result,', 'database_cost,', 'columnar_probe,')):
                key, payload = line.split(',', 1)
                item[key] = json.loads(payload)
            elif line.startswith('managed_readback,'):
                item.setdefault('managed_readback', []).append(next(csv.reader(io.StringIO(line)))[1:])
        if kind.startswith('managed'):
            assert item['measurement']['packets'] == '512'
            assert item['managed_result']['rows_written'] == 2048
            item['result_rows_per_second'] = 2 * float(item['measurement']['primary_pps'])
        result['runs'].append(item)
        save()
        return item

    fixture = ['512', '1', 'mixed', 'rst', '16', '512', '0', 'session', '1',
               str(args.options), 'clickhouse', args.channel]
    for mode in ['throughput', 'cost']:
        for repeat in range(args.repeats):
            labels = ['baseline', 'optimized'] if repeat % 2 == 0 else ['optimized', 'baseline']
            for label in labels:
                directory = getattr(args, label)
                run(directory / 'benchmark_npm_throughput_managed', fixture + (['cost'] if mode == 'cost' else []),
                    'managed-' + mode, label, repeat)
    for size in [256, 4096, 16384]:
        for repeat in range(args.repeats):
            modes = ['values', 'arrow'] if repeat % 2 == 0 else ['arrow', 'values']
            for mode in modes:
                run(args.optimized / 'benchmark_clickhouse_columnar',
                    [str(args.options), args.channel, mode, str(size), '4'], 'bulk-' + str(size), mode, repeat)

    for kind in sorted({r['kind'] for r in result['runs']}):
        for label in sorted({r['label'] for r in result['runs'] if r['kind'] == kind}):
            runs = [r for r in result['runs'] if r['kind'] == kind and r['label'] == label]
            item = dict(kind=kind, label=label)
            if kind.startswith('managed'):
                for key in ['primary_pps', 'process_ms', 'flush_ms', 'cpu_ms', 'rss_peak_bytes']:
                    item[key] = summary([float(r['measurement'][key]) for r in runs])
                item['result_rows_per_second'] = summary([r['result_rows_per_second'] for r in runs])
                item['pending_output_peak_bytes'] = summary([
                    r['managed_result']['diagnostics']['pending_output_peak_bytes'] for r in runs])
                if kind.endswith('cost'):
                    for key in runs[0]['database_cost']:
                        item[key] = summary([r['database_cost'][key] for r in runs])
                fingerprints = [[row for row in r['managed_readback']] for r in runs]
                assert all(value == fingerprints[0] for value in fingerprints), 'result equivalence failed'
                item['business_fingerprint'] = fingerprints[0]
            else:
                for key in ['rows_per_second', 'logical_bytes_per_second', 'write_ms', 'cpu_ms', 'ipc_pool_peak',
                            'rss_bytes', 'requests', 'logical_bytes']:
                    item[key] = summary([r['columnar_probe'][key] for r in runs])
                assert len({r['columnar_probe']['digest'] for r in runs}) == 1
            result['summaries'].append(item)
    throughput = [s for s in result['summaries'] if s['kind'] == 'managed-throughput']
    baseline = next(s for s in throughput if s['label'] == 'baseline')
    optimized = next(s for s in throughput if s['label'] == 'optimized')
    result['equivalence_passed'] = baseline['business_fingerprint'] == optimized['business_fingerprint']
    result['managed_speedup'] = optimized['primary_pps']['median'] / baseline['primary_pps']['median']
    result['performance_passed'] = result['managed_speedup'] >= 2 and result['equivalence_passed']
    cost = next(s for s in result['summaries'] if s['kind'] == 'managed-cost' and s['label'] == 'optimized')
    assert cost['arrow_write_calls']['median'] == 4 and cost['arrow_write_rows']['median'] == 1024
    assert cost['prepared_batch_calls']['median'] == 0
    # Same result bytes for both sides; use the measured Arrow result payload, excluding transport protocol bytes.
    logical_bytes = cost['arrow_logical_bytes']['median']
    for item in throughput:
        item['arrow_logical_bytes_per_second'] = item['result_rows_per_second']['median'] * logical_bytes / 1024
    result['server_after'] = server_state()
    save()
    assert result['performance_passed'], 'managed speedup or equivalence gate failed'
    print(json.dumps(dict(speedup=result['managed_speedup'], equivalence=result['equivalence_passed'],
                          runs=len(result['runs']), output=str(args.output))))


if __name__ == '__main__':
    main()
