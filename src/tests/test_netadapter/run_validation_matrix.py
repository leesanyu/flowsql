#!/usr/bin/env python3
# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Run isolated real inputs sequentially and retain every attempted result."""
import argparse
import json
import os
import pathlib
import subprocess
import signal
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--executable', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--group', choices=['capture', 'sql', 'all', 'recovery', 'functional', 'periods', 'sustained'],
                        default='all')
    parser.add_argument('--profile', help='select one named profile within the chosen group')
    parser.add_argument('--ticks-per-s', type=int, help='explicitly override the selected controlled send load')
    parser.add_argument('--keep-going', action='store_true', help='record failures and finish the selected sweep')
    parser.add_argument('--resume', action='store_true', help='retain existing attempted cases without overwriting artifacts')
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[3]
    output = pathlib.Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    executable = pathlib.Path(args.executable).resolve()
    script = root / 'src/tests/test_netadapter/run_isolated_validation.sh'
    cases = []
    if args.group in ['capture', 'all']:
        for backend in ['af_packet', 'pfring_classic', 'af_xdp_copy_skb']:
            for frame in [64, 1500]:
                for ticks in [100, 500, 1000]:
                    for profile in ['single', 'dual']:
                        cases.append(('capture', backend, 1, 5, ticks, frame, 'stop', profile))
                for profile in ['busy_idle', 'reject']:
                    cases.append(('capture', backend, 1, 5, 1000, frame, 'cancel', profile))
    if args.group in ['sql', 'all']:
        cases.extend([
            ('sql_all', 'af_packet', 15, 47, 100, 64, 'stop', 'dual'),
            ('sql_all', 'pfring_classic', 15, 47, 100, 1500, 'cancel', 'dual'),
            ('sql_all', 'af_xdp_copy_skb', 15, 47, 100, 64, 'stop', 'dual'),
            ('sql_all', 'af_xdp_copy_skb', 30, 92, 100, 1500, 'cancel', 'dual'),
            ('sql_all', 'af_xdp_copy_skb', 60, 182, 100, 64, 'stop', 'dual'),
        ])
    if args.group == 'recovery':
        for backend in ['af_packet', 'pfring_classic', 'af_xdp_copy_skb']:
            for profile in ['slow_analysis', 'slow_output', 'overload_recovery']:
                cases.append(('sql_probe', backend, 15, 47, 100, 64 if profile != 'slow_output' else 1500,
                              'cancel' if profile == 'slow_output' else 'stop', profile))
    if args.group == 'functional':
        for backend in ['af_packet', 'pfring_classic', 'af_xdp_copy_skb']:
            for profile in ['single', 'busy_idle', 'reject', 'domain_isolation', 'input_failure']:
                cases.append(('sql_probe', backend, 1, 5, 100, 64, 'stop', profile))
    if args.group == 'periods':
        for backend in ['af_packet', 'pfring_classic']:
            for interval, frame, finish in [(30, 1500, 'cancel'), (60, 64, 'stop')]:
                cases.append(('sql_probe', backend, interval, interval * 3 + 2, 100, frame, finish, 'dual'))
    if args.group == 'sustained':
        for backend in ['af_packet', 'pfring_classic', 'af_xdp_copy_skb']:
            for frame in [64, 1500]:
                for profile in ['single', 'dual', 'busy_idle', 'high_reject']:
                    cases.append(('sql_probe', backend, 15, 47, 100, frame,
                                  'cancel' if profile in ['single', 'high_reject'] else 'stop', profile))
    if args.profile:
        cases = [case for case in cases if case[-1] == args.profile]
        if not cases: parser.error('no selected cases match the profile')
    if args.ticks_per_s is not None:
        if args.ticks_per_s <= 0: parser.error('ticks-per-s must be positive')
        cases = [(mode, backend, interval, duration, args.ticks_per_s, frame, finish, profile)
                 for mode, backend, interval, duration, ticks, frame, finish, profile in cases]
    summary = {
        'scope': 'T5 selected real capture/SQL cases; each result retains its frozen criteria and measurement scope',
        'group': args.group,
        'environment': {},
        'cases': [],
        'passed': True,
    }
    summary_path = output / 'summary.json'
    if args.resume and summary_path.exists():
        summary = json.loads(summary_path.read_text())
        if summary['group'] != args.group:
            parser.error('resume requires the same group and output directory')
    attempted = {case['name'] for case in summary['cases']}
    summary['keep_going'] = args.keep_going
    for name, command in [
        ('kernel', ['uname', '-a']),
        ('libxdp', ['pkg-config', '--modversion', 'libxdp']),
        ('libbpf', ['pkg-config', '--modversion', 'libbpf']),
        ('pfring', ['cat', '/proc/net/pf_ring/info']),
        ('plugin_libraries', ['ldd', str(executable.parent / 'libflowsql_capture_pfring.so')]),
    ]:
        result = subprocess.run(command, text=True, capture_output=True, check=False)
        summary['environment'][name] = {'returncode': result.returncode, 'output': result.stdout.strip()}
    for mode, backend, interval, duration, ticks, frame, finish, profile in cases:
        if (output / 'STOP').exists():
            summary['interrupted_at_case_boundary'] = True
            summary_path.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n')
            print('STOP requested; unfinished cases remain pending', flush=True)
            return 130
        name = f'{backend}-{mode}-{profile}-{frame}-{ticks}-{interval}-{finish}'
        if name in attempted:
            print('RETAIN ' + name, flush=True)
            continue
        artifact = output / (name + '.json')
        command = ['bash', str(script), str(executable), mode, backend, str(interval), str(duration),
                   str(ticks), str(frame), finish, str(artifact), profile]
        print('RUN ' + name, flush=True)
        start = time.monotonic()
        with (output / (name + '.log')).open('w') as log:
            process = subprocess.Popen(command, cwd=root, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                returncode = process.wait(timeout=duration + 30)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
                returncode = 124
        record = {'name': name, 'command': command, 'returncode': returncode,
                  'elapsed_s': time.monotonic() - start, 'artifact': str(artifact)}
        if artifact.exists():
            data = json.loads(artifact.read_text())
            record['passed_observed_checks'] = data.get('passed_observed_checks', False)
            for field in ['sent_packets', 'delivered_packets', 'capture_allocated_peak_bytes',
                          'common_watermark_lag_ms', 'per_input_service_max_ms', 'window_close_observation_ms',
                          'complete_rows_before_stop', 'session_count', 'final_rows']:
                if field in data:
                    record[field] = data[field]
        record['passed'] = returncode == 0 and record.get('passed_observed_checks', False)
        summary['cases'].append(record)
        summary['passed'] = summary['passed'] and record['passed']
        summary_path.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n')
        print(('PASS ' if record['passed'] else 'FAIL ') + name, flush=True)
        if not record['passed'] and not args.keep_going:
            return 1
    return 0 if summary['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
