# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Execute the user guide against an isolated native deployment and real plugins."""
import ast
import json
import base64
import os
from pathlib import Path
import re
import shutil
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from urllib.parse import unquote, urlsplit

REPO = Path(__file__).resolve().parents[3]
GUIDE = REPO / 'docs/sql-workflow.md'


def documentation():
    text = GUIDE.read_text()
    identifiers = re.findall(r'<!-- workflow:([\w-]+) -->', text)
    assert len(identifiers) == len(set(identifiers)), identifiers
    records = dict(re.findall(r'<!-- workflow:([\w-]+) -->\s*```json\n(.*?)\n```', text, re.S))
    tree = ast.parse(Path(__file__).read_text())
    calls = [node for node in ast.walk(tree) if isinstance(node, ast.Call)
             and isinstance(node.func, ast.Name) and node.func.id == 'request']
    invoked = {node.args[1].value for node in calls}
    assert set(records) == invoked, (set(records) - invoked, invoked - set(records))
    assert set(identifiers) == invoked | {'SETUP', 'DNS-LABELS'}, identifiers
    declared = {'EXAMPLE_DIR', 'LABELS_B64', 'LABEL_REF', 'RUN_ID', 'STREAM_TASK_ID'}
    for node in calls:
        identifier = node.args[1].value
        record = json.loads(records[identifier])
        assert record['method'] == 'POST' and record['path'].startswith('/api/'), record
        assert isinstance(record['body'], dict), record
        required = set(re.findall(r'\$\{(\w+)\}', records[identifier]))
        assert required == {keyword.arg for keyword in node.keywords}, (identifier, required)
        assert required <= declared, (identifier, required)
        assert not re.search(r'"(?:password|secret|token|private_key)"\s*:', records[identifier], re.I), identifier
    compile(block('SETUP', 'python'), str(GUIDE), 'exec')
    assert set(re.findall(r'^### (E\d{2})[：:]', text, re.M)) == {f'E{i:02}' for i in range(1, 7)}
    assert re.findall(r'^\| (C\d{2}) ', text, re.M) == [f'C{i:02}' for i in range(1, 6)]
    assert 'conditional_template' in text and '未执行真实网卡或 Broker' in text
    assert '结构示意' in text and '尖括号和花括号不是可复制的 SQL' in text

    # Validate the guide's local destinations and fragments, plus the new entry links.
    links = [(GUIDE, target) for target in re.findall(r'\]\(([^)]+)\)', text)]
    for relative in ['README.md', 'docs/vision.md', 'docs/npm.md', 'docs/baseliner.md',
                     'docs/kafka.md', 'docs/netadapter.md']:
        source = REPO / relative
        targets = [target for target in re.findall(r'\]\(([^)]+)\)', source.read_text())
                   if 'sql-workflow.md' in target]
        assert targets, relative
        links.extend((source, target) for target in targets)
    for source, target in links:
        parsed = urlsplit(target)
        assert not parsed.scheme and not parsed.netloc, (source, target)
        destination = (source.parent / unquote(parsed.path)).resolve() if parsed.path else source
        assert destination.is_file(), (source, target)
        if parsed.fragment:
            anchors, counts = set(), {}
            for heading in re.findall(r'^#{1,6}\s+(.+?)\s*#*$', destination.read_text(), re.M):
                slug = re.sub(r'[^\w\- ]', '', heading.lower()).replace(' ', '-')
                occurrence = counts.get(slug, 0)
                counts[slug] = occurrence + 1
                anchors.add(slug + (f'-{occurrence}' if occurrence else ''))
            anchors.update(re.findall(r'(?:id|name)=[\'\"]([^\'\"]+)', destination.read_text()))
            assert unquote(parsed.fragment) in anchors, (source, target)

    vision = (REPO / 'docs/vision.md').read_text()
    for match in re.finditer(r'^```(?:sql|C\+\+|Python)\n', vision, re.M):
        assert '规划' in vision[max(0, match.start() - 150):match.start()], match.start()
    for heading in ['### 基于模型的数据探索', '### 任务生命周期管理']:
        section = vision[vision.index(heading):]
        assert '规划' in section.split('```', 1)[0], heading
    assert 'SELECT <projection> FROM <source>' in vision
    print(f'PASS G5: {len(records)} document requests match execution calls; '
          f'{len(links)} local links/fragments; executable and planning boundaries', flush=True)


def block(identifier, language='json'):
    text = GUIDE.read_text()
    matches = re.findall(r'<!-- workflow:' + re.escape(identifier) + r' -->\s*```' + language + r'\n(.*?)\n```',
                         text, re.S)
    assert len(matches) == 1, (identifier, len(matches))
    return matches[0]


def request(rt, identifier, **variables):
    record = json.loads(block(identifier))
    def expand(value):
        if isinstance(value, str):
            for key, replacement in variables.items():
                value = value.replace('${' + key + '}', str(replacement))
            assert '${' not in value, (identifier, value)
        elif isinstance(value, dict):
            return {key: expand(item) for key, item in value.items()}
        elif isinstance(value, list):
            return [expand(item) for item in value]
        return value
    record = expand(record)
    assert record['method'] == 'POST', record
    result = rt.ok(record['path'], record['body'])
    print(identifier, json.dumps(result)[:1600], flush=True)
    return result


def preview(rt, name):
    rows, page = [], 1
    while True:
        result = rt.ok('/api/channels/dataframe/preview', dict(name=name, page=page, page_size=100))
        rows.extend(dict(zip(result['columns'], row)) for row in result['data'])
        if len(rows) >= result['rows']:
            assert len(rows) == result['rows'], result
            return rows
        assert result['data'], result
        page += 1


def call(base, path, body=None):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(base + path, data=data,
                                 headers={'Content-Type': 'application/json'} if data else {})
    try:
        with urllib.request.urlopen(req, timeout=40) as response:
            return response.status, json.loads(response.read() or b'{}')
    except urllib.error.HTTPError as error:
        return error.code, json.loads(error.read() or b'{}')


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


class Runtime:
    def __init__(self, output, root):
        self.output, self.root = output, root
        self.web_port, self.router_port = free_port(), free_port()
        self.web = f'http://127.0.0.1:{self.web_port}'
        self.router = f'http://127.0.0.1:{self.router_port}'

    def ok(self, path, body=None, internal=False):
        status, result = call(self.router if internal else self.web, path, body)
        assert status == 200, (path, status, result)
        return result

    def start(self, phase='discovery'):
        root, out = self.root, self.output
        previous = os.environ.get('FLOWSQL_EXAMPLE_DIR')
        os.environ['FLOWSQL_EXAMPLE_DIR'] = str(root)
        try:
            exec(compile(block('SETUP', 'python'), str(GUIDE), 'exec'), {})
        finally:
            if previous is None:
                os.environ.pop('FLOWSQL_EXAMPLE_DIR')
            else:
                os.environ['FLOWSQL_EXAMPLE_DIR'] = previous
        plugins = [
            ('builtin', ''),
            ('catalog', f'data_dir={root}/dataframes;operator_db_path={root}/meta.db'),
            ('binaddon', f'upload_dir={root}/binaddon;operator_db_path={root}/meta.db'),
            ('database', f'type=sqlite;name=work;path={root}/work.db'),
            ('stream', f'db_path={root}/meta.db'),
            ('npi', json.dumps({'ldfile': str(out / 'config/protocols.yml')})),
            ('pcapfile', f'db_path={root}/meta.db'),
            ('netadapter', json.dumps({'db_path': str(root / 'netadapter.db')})),
            ('capture_af_packet', ''),
            ('config_channel', f'db_path={root}/meta.db'),
            ('scheduler', ''), ('task', f'db_path={root}/tasks.db'),
            ('web', f'host=127.0.0.1;port={self.web_port};db_path={root}/web.db;'
                    f'upload_dir={root}/uploads;gateway=127.0.0.1:{self.router_port}'),
            ('router', f'host=127.0.0.1;port={self.router_port}'),
        ]
        if phase == 'finite':
            plugins.extend([('baseline', ''), ('flow_labeling', 'eal_memory_mib=512')])
        text = ','.join(str(out / f'libflowsql_{name}.so') + (':' + option if option else '')
                        for name, option in plugins)
        self.log = (root / 'runtime.log').open('w+')
        self.child = subprocess.Popen([str(out / 'flowsql'), '--role', 'all', '--plugins', text], cwd=root,
                                      env=dict(os.environ, LD_LIBRARY_PATH=str(out)),
                                      stdout=self.log, stderr=self.log)
        deadline = time.monotonic() + 25
        while time.monotonic() < deadline and self.child.poll() is None:
            try:
                self.ok('/api/health')
                self.ok('/api/operators/list?type=builtin')
                return
            except (OSError, AssertionError):
                time.sleep(0.05)
        self.log.seek(0)
        raise AssertionError(self.log.read()[-6000:])

    def stop(self):
        self.child.terminate()
        try:
            self.child.wait(timeout=12)
        except subprocess.TimeoutExpired:
            self.child.kill()
            self.child.wait()
        self.log.close()

    def upload(self, library):
        copy = self.root / library
        shutil.copy2(self.output / library, copy)
        uploaded = self.ok('/api/operators/upload', dict(type='cpp', filename=library, tmp_path=str(copy)))
        self.ok('/api/operators/activate', dict(type='cpp', plugin_id=uploaded['plugin_id']))


def discovery(rt):
    listing = rt.ok('/api/channels/list')
    rows = listing if isinstance(listing, list) else listing['channels']
    sample = next(row for row in rows if row['name'] == 'samples')
    assert sample['type'] == 'dataframe', sample
    cold = rt.ok('/channels/dataframe', internal=True)['channels']
    sample = next(row for row in cold if row['name'] == 'samples')
    assert sample['load_state'] == 'unloaded' and sample['rows'] is None and sample['schema'] is None, sample
    dbs = rt.ok('/api/channels/database/query', {})
    assert 'work' in json.dumps(dbs), dbs
    defs = rt.ok('/api/channels/stream/definitions/query', {})['definitions']
    assert {'ring', 'netadapter', 'tcp_session_mock'}.issubset({d['channel_type'] for d in defs}), defs
    rt.ok('/api/channels/stream/add', dict(type='tcp_session_mock', name='finite',
          option=json.dumps(dict(mode='none', total_records=4, batch_rows=2, emit_interval_ms=0,
                                 partition_count=1, ring_size=16, overflow='block', ring_mode='spsc'))))
    rt.ok('/api/channels/stream/add', dict(type='pcapfile', name='capture', role='source',
          option=json.dumps(dict(path=str(REPO / 'src/tests/data/packets/Dns.pcap'), format='pcap'))))
    channels = rt.ok('/api/channels/stream/query', {})['channels']
    assert any(c['type'] == 'pcapfile' and c['name'] == 'capture' for c in channels), channels
    rt.ok('/api/channels/stream/add', dict(type='netadapter', name='unused', role='source',
          option=json.dumps(dict(backend='af_packet', interfaces=['wf_unused'], snaplen=65535,
                                 buffer_mib=4, promiscuous=False))))
    live = 'SELECT * FROM netadapter.unused USING npm.basic INTO sqlite.work'
    assert rt.ok('/api/tasks/sql/classify', {'sql': live}) == {'task_kind': 'batch', 'requires_async': True}
    status, error = call(rt.web, '/api/tasks/batch/execute', dict(sql_text=live, mode='sync'))
    assert status == 400 and error['error_code'] == 'ASYNC_EXECUTION_REQUIRED', (status, error)
    operators = rt.ok('/api/operators/list?type=builtin')['operators']
    assert any(op['full_name'] == 'builtin.passthrough' for op in operators), operators
    rt.ok('/api/operators/activate', {'name': 'builtin.passthrough'})
    rt.ok('/api/operators/deactivate', {'name': 'builtin.passthrough'})
    operators = rt.ok('/api/operators/list?type=builtin')['operators']
    assert next(op for op in operators if op['full_name'] == 'builtin.passthrough')['active'] == 0
    rt.ok('/api/operators/activate', {'name': 'builtin.passthrough'})
    rt.upload('libflowsql_npm_basic.so')
    cpp = rt.ok('/api/operators/list?type=cpp')
    assert 'npm.basic' in json.dumps(cpp), cpp
    rt.ok('/api/operators/list?type=python')
    sql = 'SELECT * FROM dataframe.samples USING builtin.passthrough INTO dataframe.copy'
    assert rt.ok('/api/tasks/sql/classify', {'sql': sql}) == {'task_kind': 'batch', 'requires_async': False}
    analysis = rt.ok('/api/tasks/sql/analyze', {'sql_text': sql + ';' + sql + ';'})
    assert analysis['statement_count'] == 2 and analysis['task_kind'] == 'batch', analysis
    stream_sql = 'SELECT * FROM tcp_session_mock.finite USING builtin.passthrough_stream INTO dataframe.stream_out'
    assert rt.ok('/api/tasks/sql/classify', {'sql': stream_sql})['task_kind'] == 'stream'
    assert rt.ok('/api/tasks/sql/analyze', {'sql_text': sql + ';' + stream_sql})['task_kind'] == 'mixed'
    for path, body in [('/api/tasks/sql/classify', {'sql_text': sql}),
                       ('/api/tasks/batch/execute', {'sql': sql}),
                       ('/api/tasks/batch/execute', {'sql_text': stream_sql}),
                       ('/api/tasks/stream/execute', {'execution_kind': 'single', 'sql_text': sql}),
                       ('/api/tasks/sql/classify', {'sql': 'SELECT * FROM dataframe.samples, tcp_session_mock.finite'})]:
        status, error = call(rt.web, path, body)
        assert status >= 400 and 'error' in error, (path, status, error)
    print('PASS G1/G2: real directories, lazy metadata, activation and execution entry contracts', flush=True)


def finite(rt):
    rt.ok('/api/operators/activate', {'name': 'builtin.passthrough'})
    rt.upload('libflowsql_npm_basic.so')
    rt.upload('libflowsql_baseliner.so')
    request(rt, 'E01')
    assert preview(rt, 'csv_copy') == [dict(name=n, value=v) for n, v in [('alpha', 1), ('beta', 2), ('gamma', 3)]]
    request(rt, 'E02')
    total = preview(rt, 'sqlite_total')
    assert total == [{'SUM(value)': '5'}], total
    request(rt, 'E03-SOURCE', EXAMPLE_DIR=rt.root)
    labels = base64.b64encode(block('DNS-LABELS', 'yaml').encode()).decode()
    published = request(rt, 'E03-CONFIG', LABELS_B64=labels)
    request(rt, 'E03-BASIC')
    basic = preview(rt, 'dns_basic')
    assert len(basic) == 1, basic
    assert basic[0]['packets_ab'] == basic[0]['packets_ba'] == 1, basic
    request(rt, 'E03-SESSION')
    session = preview(rt, 'dns_session')
    assert len(session) == 1, session
    assert session[0]['packets_ab'] == session[0]['packets_ba'] == 1, session
    request(rt, 'E03-DNS', LABEL_REF=published['exact_reference'])
    request(rt, 'E03-RUN')
    runs = preview(rt, 'dns_runs')
    assert len(runs) == 1 and runs[0]['status'] == 'completed', runs
    run_id = runs[0]['run_id']
    request(rt, 'E03-QUERY', RUN_ID=run_id)
    dns = preview(rt, 'dns_transaction')
    assert len(dns) == 1 and dns[0]['qname'] == 'example.com', dns
    assert dns[0]['outcome'] == 'matched' and dns[0]['response_rcode'] == 0, dns
    assert dns[0]['latency_ns'] == 10_000_000, dns
    with sqlite3.connect(rt.root / 'work.db') as db:
        for entity in ['basic', 'session', 'dns_transaction']:
            count = db.execute(f'SELECT COUNT(*) FROM npm_{entity}_ipv4_final_v1 WHERE __npm_run_id=?', (run_id,)).fetchone()[0]
            assert count == 1, (entity, count)
    request(rt, 'E04')
    with sqlite3.connect(rt.root / 'work.db') as db:
        db.row_factory = sqlite3.Row
        rows = [dict(row) for row in db.execute('SELECT * FROM workflow_baseline')]
    evaluations = [row for row in rows if row['result_kind'] == 'evaluation']
    forecasts = [row for row in rows if row['result_kind'] == 'forecast']
    assert len(evaluations) == 40 and len(forecasts) == 80, (len(evaluations), len(forecasts))
    assert {r['target_bucket'] for r in evaluations} == set(range(40))
    assert all(r['observed'] == 100 + r['target_bucket'] % 3 for r in evaluations)
    assert all(r['target_bucket'] - r['issued_after_bucket'] in (1, 2) and r['observed'] is None for r in forecasts), forecasts[:2]
    assert max(r['target_bucket'] for r in forecasts) == 41
    models = preview(rt, 'workflow_models')
    assert models and all(row['config_hash'] for row in models), models
    print('PASS G3: E01-E04 document requests and deterministic output contracts', flush=True)


def stream_status(rt, task_id, terminal=True):
    deadline = time.monotonic() + 20
    while True:
        result = rt.ok('/api/tasks/stream/status', {'task_id': task_id})
        status = result['status']
        if status in ('stopped', 'failed', 'cancelled'):
            assert status == 'stopped', result
            return result
        if not terminal and status == 'running':
            return result
        assert status in ('running', 'preparing', 'stopping', 'pending'), result
        assert time.monotonic() < deadline, result
        time.sleep(0.02)


def stream(rt):
    rt.ok('/api/operators/activate', {'name': 'builtin.passthrough_stream'})
    request(rt, 'E05-SOURCE')
    submitted = request(rt, 'E05')
    task_id = submitted['task_id']
    stream_status(rt, task_id)
    rows = preview(rt, 'workflow_stream_single_out')
    assert len(rows) == 6, rows
    assert set(rows[0]) == {'clientIP', 'clientPort', 'serverIP', 'serverPort', 'bps', 'pps'}, rows[0]
    assert [row['clientPort'] for row in rows] == list(range(10000, 10006)), rows

    request(rt, 'E05-LIVE-SOURCE')
    request(rt, 'E05-LIVE-SINK')
    live = request(rt, 'E05-LIVE')
    assert stream_status(rt, live['task_id'], terminal=False)['status'] == 'running'
    stopped = request(rt, 'E05-STOP', STREAM_TASK_ID=live['task_id'])
    assert stopped['status'] == 'stopped', stopped
    stopped = stream_status(rt, live['task_id'])
    assert stopped['processed_rows'] == stopped['output_rows'] == 0, stopped

    request(rt, 'E06-STREAM-SOURCE')
    request(rt, 'E06-STREAM-MID')
    submitted = request(rt, 'E06-STREAM')
    assert submitted['node_count'] == 2, submitted
    deadline = time.monotonic() + 20
    while True:
        result = rt.ok('/api/tasks/stream/status', {'task_id': submitted['task_id']})
        assert result['status'] == 'running', result
        assert len(result['nodes']) == 2 and {node['node_kind'] for node in result['nodes']} == {'stream'}, result
        if all(node['processed_rows'] == 6 for node in result['nodes']) and result['nodes'][0]['status'] == 'stopped':
            break
        assert time.monotonic() < deadline, result
        time.sleep(0.02)
    rows = preview(rt, 'workflow_stream_group_out')
    assert len(rows) == 6 and [row['clientPort'] for row in rows] == list(range(10000, 10006)), rows
    stopped = request(rt, 'E06-STREAM-STOP', STREAM_TASK_ID=submitted['task_id'])
    assert stopped['status'] == 'stopped', stopped
    stream_status(rt, submitted['task_id'])

    request(rt, 'E06-MIXED-MID')
    submitted = request(rt, 'E06-MIXED')
    assert submitted['node_count'] == 2, submitted
    result = stream_status(rt, submitted['task_id'])
    assert len(result['nodes']) == 2 and {node['node_kind'] for node in result['nodes']} == {'batch', 'stream'}, result
    assert preview(rt, 'workflow_mixed_out') == [dict(name=n, value=v) for n, v in [('alpha', 1), ('beta', 2), ('gamma', 3)]]

    listing = rt.ok('/api/tasks/stream/list', {})
    assert task_id in json.dumps(listing), listing
    status, error = call(rt.web, '/api/tasks/stream/execute', {
        'execution_kind': 'single', 'sql_text': 'SELECT * FROM tcp_session_mock.workflow_stream_single INTO dataframe.invalid'})
    assert status >= 400 and 'error' in error, (status, error)
    status, error = call(rt.web, '/api/tasks/stream/execute', {
        'execution_kind': 'single', 'sql_text': 'SELECT * FROM ring.workflow_idle_source USING builtin.passthrough_stream INTO sqlite.work'})
    assert status >= 400 and 'explicit table' in json.dumps(error), (status, error)
    analyzed = rt.ok('/api/tasks/sql/analyze', {'sql_text':
        "SELECT * FROM dataframe.samples USING builtin.passthrough WITH note='a;b' INTO dataframe.a;"
        'SELECT * FROM dataframe.samples INTO dataframe.b;'})
    assert analyzed['statement_count'] == 2, analyzed
    print('PASS G4: E05/E06 single, stream group and mixed group document requests and output contracts', flush=True)


def main():
    output, phase = Path(sys.argv[1]).resolve(), sys.argv[2]
    if phase == 'docs':
        documentation()
        return
    with tempfile.TemporaryDirectory(prefix='flowsql-sql-workflow-') as directory:
        rt = Runtime(output, Path(directory))
        try:
            rt.start(phase)
            {'discovery': discovery, 'finite': finite, 'stream': stream}[phase](rt)
        except BaseException:
            if hasattr(rt, 'log'):
                rt.log.flush()
                print((rt.root / 'runtime.log').read_text()[-8000:], file=sys.stderr)
            raise
        finally:
            if hasattr(rt, 'child'):
                rt.stop()


if __name__ == '__main__':
    main()
