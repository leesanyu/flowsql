"""Build and hot-upgrade a real compatible UI while the original Web process stays alive."""
import contextlib
import gzip
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time

from test_entry import port, request, service
from test_compute import call

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools'))
from release import check_exploration


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def rss(process):
    return int(next(line.split()[1] for line in Path(f'/proc/{process.pid}/status').read_text().splitlines()
                    if line.startswith('VmRSS:')))


def service_rss(guardian):
    children = Path(f'/proc/{guardian.pid}/task/{guardian.pid}/children').read_text().split()
    assert len(children) == 1, children
    return rss(type('Process', (), {'pid': int(children[0])})())


def main():
    output, node = Path(sys.argv[1]).resolve(), sys.argv[2]
    evidence = ROOT / 'evidence/visualization-database/t4'
    evidence.mkdir(parents=True, exist_ok=True)
    checks = []
    def passed(name):
        checks.append(name)
        print('PASS', name, flush=True)
    with tempfile.TemporaryDirectory(prefix='flowsql-exploration-delivery-') as temporary:
        directory = Path(temporary)
        static = directory / 'exploration'
        shutil.copytree(output / 'exploration', static)
        original = check_exploration(static)
        project = directory / 'project'
        project.mkdir()
        for name in ('package.json', 'index.html', 'vite.config.js'):
            shutil.copy2(ROOT / 'src/exploration' / name, project / name)
        shutil.copytree(ROOT / 'src/exploration/src', project / 'src')
        main_source = project / 'src/main.jsx'
        main_source.write_text(main_source.read_text().replace('FlowSQL · 数据探索', 'FlowSQL · 数据探索 0.1.1'))
        (project / 'node_modules').symlink_to(ROOT / 'src/exploration/node_modules', target_is_directory=True)
        package = json.loads((project / 'package.json').read_text())
        package['version'] = '0.1.1'
        (project / 'package.json').write_text(json.dumps(package))
        candidate = directory / 'candidate'
        environment = dict(os.environ, FLOWSQL_BUILD_INFO_FILE=str(output / 'build-info.json'),
                           PATH=str(Path(node).parent) + ':' + os.environ['PATH'])
        with (evidence / 'upgrade-build.log').open('w') as log:
            subprocess.run(['npm', 'run', 'build', '--', '--outDir', str(candidate)], cwd=project,
                           env=environment, stdout=log, stderr=log, check=True, timeout=120)
        upgraded = check_exploration(candidate)
        assert original['ui_version'] != upgraded['ui_version'] == '0.1.1'
        assert {name for name in original['files'] if name.endswith('.js')} != {name for name in upgraded['files'] if name.endswith('.js')}
        passed('actually_rebuilt_compatible_independent_package')
        db = directory / 'data.db'
        with sqlite3.connect(db) as connection:
            connection.execute('CREATE TABLE samples(host TEXT,time TEXT,value REAL,x REAL,allnull REAL,mixed INTEGER)')
            connection.executemany('INSERT INTO samples VALUES(?,?,?,?,?,?)', [
                ('alpha', '2026-01-01T00:00:00Z', 10, 1, None, 1),
                ('alpha', '2026-01-01T00:01:00Z', None, 2, None, 'text'),
                ('beta', '2026-01-01T00:02:00Z', 20, 3, None, 3)])
        channels = directory / 'channels.yml'
        channels.write_text('channels:\n  database_channels:\n    - type: sqlite\n      name: example\n      path: ' + str(db) + '\n      readonly: "true"\n')
        config_before, db_before = digest(channels), digest(db)
        deployment = directory / 'deploy.yaml'
        visual = port()
        base = f'http://127.0.0.1:{visual}'
        deploy = (output / 'config/deploy-visualization.yaml').read_text()
        deploy = deploy.replace('config_file=config/flowsql.yml', 'config_file=' + str(channels))
        deploy = deploy.replace('"port":18910', '"port":' + str(visual)).replace('"static_dir":"exploration"', '"static_dir":' + json.dumps(str(static)))
        deployment.write_text(deploy)
        env = dict(os.environ, LD_LIBRARY_PATH=str(output) + ':' + os.environ.get('LD_LIBRARY_PATH', ''))
        fixed = [output / 'flowsql', output / 'libflowsql_web.so', *sorted((output / 'static').rglob('*'))]
        fixed = {str(path): digest(path) for path in fixed if path.is_file()}
        with (evidence / 'deployment.log').open('w+') as log:
            started = time.monotonic()
            process = subprocess.Popen([str(output / 'flowsql'), '--config', str(deployment)], cwd=output,
                                       env=env, stdout=log, stderr=log)
            try:
                deadline = started + 15
                while True:
                    try:
                        if call(base, 'capabilities')[0] == 200:
                            break
                    except OSError:
                        pass
                    if process.poll() is not None or time.monotonic() > deadline:
                        log.seek(0)
                        raise AssertionError(log.read())
                    time.sleep(.01)
                startup_ms = (time.monotonic() - started) * 1000
                idle_rss = service_rss(process)
                assert len(call(base, 'sources')[1]['sources']) == 1
                passed('staged_native_deployment_without_scheduler_gateway_or_worker')
                web_port = port()
                host = f'http://127.0.0.1:{web_port}'
                options = f'port={web_port};db_path={directory}/web.db;upload_dir={directory}/uploads;visualization_upstream={base}'
                with service(output, directory, 'web', options, host + '/api/version') as web:
                    web_pid = web.pid
                    with request(host + '/exploration/manifest.json') as response:
                        assert json.load(response)['ui_version'] == original['ui_version']
                    # New files first, manifest last. Keep old hashed chunks for open pages.
                    for path in candidate.rglob('*'):
                        if path.is_file() and path.name != 'manifest.json':
                            destination = static / path.relative_to(candidate)
                            destination.parent.mkdir(parents=True, exist_ok=True)
                            shutil.copy2(path, destination)
                    shutil.copy2(candidate / 'manifest.json', static / 'manifest.next')
                    (static / 'manifest.next').replace(static / 'manifest.json')
                    for address in (base, host):
                        with request(address + '/exploration/manifest.json') as response:
                            assert json.load(response)['ui_version'] == '0.1.1'
                    for name in original['files']:
                        if name.startswith('assets/'):
                            with request(base + '/exploration/' + name) as response:
                                assert response.status == 200
                    passed('hot_upgrade_preserves_old_assets_and_both_entries')
                    for address, mode in ((host, 'embedded'), (base, 'standalone')):
                        subprocess.run([node, str(Path(__file__).with_name('compute_browser.mjs')), address,
                                        str(evidence / (mode + '-browser.json')), mode], check=True, timeout=120)
                        report = json.loads((evidence / (mode + '-browser.json')).read_text())
                        assert report['selected'] == report['passed'] == 7 and not report['failed']
                        passed(mode + '_upgraded_package_three_charts_and_errors')
                    chart_rss = service_rss(process)
                    assert web.pid == web_pid and web.poll() is None
                    assert fixed == {name: digest(Path(name)) for name in fixed}
                    passed('main_web_pid_and_artifact_digests_unchanged')
                    incompatible = dict(upgraded, api_version=2)
                    (static / 'manifest.json').write_text(json.dumps(incompatible))
                    for address in (base, host):
                        with request(address + '/exploration/') as response:
                            assert response.status == 409
                    passed('incompatible_upgrade_rejected_in_both_entries')
                assert digest(channels) == config_before and digest(db) == db_before
                passed('configuration_and_database_unchanged')
                raw = sum((output / 'exploration' / name).stat().st_size for name in original['files'])
                compressed = sum(len(gzip.compress((output / 'exploration' / name).read_bytes(), mtime=0)) for name in original['files'])
                measurements = dict(date=time.strftime('%Y-%m-%dT%H:%M:%S%z'), system=platform.platform(),
                    cpus=os.cpu_count(), startup_ms=startup_ms, idle_rss_kib=idle_rss, after_charts_rss_kib=chart_rss,
                    current_package_bytes=raw, current_package_gzip_bytes=compressed, rows=3,
                    startup_definition='Popen to first successful capabilities HTTP response, 10ms polling',
                    rss_definition='Linux /proc/worker-PID/status VmRSS, excluding Guardian; idle and after both browser chart suites; one sample',
                    package_definition='Sum of files named in current manifest; each gzip member compressed independently; old chunks excluded')
                (evidence / 'measurements.json').write_text(json.dumps(measurements, indent=2))
            finally:
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                    raise AssertionError('deployment did not stop within 2s')
    (evidence / 'delivery.json').write_text(json.dumps(dict(selected=8, executed=len(checks), passed=len(checks),
        failed=0, skipped=0, not_executed=8-len(checks), checks=checks), indent=2))


if __name__ == '__main__':
    main()
