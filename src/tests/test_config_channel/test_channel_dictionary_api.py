# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Actual plugin hosts: local IID and separate Catalog/Kafka process through the control router."""
import contextlib
import importlib.util
import json
import subprocess
import sys
import tempfile
import time
import threading
import socketserver
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from test_data_semantics_api import request, call, port, validate


@contextlib.contextmanager
def running(host, directory, mode='full', gateway=None):
    directory.mkdir(parents=True, exist_ok=True)
    used = set([gateway] if gateway else [])
    ports = [gateway or port(used), port(used), port(used)]
    log_path = directory / 'host.log'
    with log_path.open('w') as log:
        process = subprocess.Popen([str(host), str(directory), *map(str, ports), mode],
                                   stdin=subprocess.PIPE, stdout=log, stderr=log, text=True, cwd=host.parent)
        try:
            until = time.monotonic() + 25
            while 'DATASET_READY' not in log_path.read_text():
                assert process.poll() is None and time.monotonic() < until, log_path.read_text()
                time.sleep(.05)
            base = 'http://127.0.0.1:' + str(ports[2])
            if mode != 'sources':
                until = time.monotonic() + 15
                while True:
                    try:
                        if request(base + '/api/semantics/v1/list', {})[0] == 200:
                            break
                    except OSError:
                        pass
                    assert process.poll() is None and time.monotonic() < until, log_path.read_text()
                    time.sleep(.05)
            yield base, process, log_path, ports
        finally:
            if process.poll() is None:
                try:
                    process.communicate('quit\n', timeout=15)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
                    raise AssertionError('dictionary host did not stop')
            assert process.returncode == 0, (process.returncode, log_path.read_text())


def iid(process, log, locator):
    prefix = 'DATASET_IID '
    prior = log.read_text().count(prefix)
    process.stdin.write(json.dumps(locator) + '\n')
    process.stdin.flush()
    until = time.monotonic() + 5
    while True:
        lines = [line.split(prefix, 1)[1] for line in log.read_text().splitlines() if prefix in line]
        if len(lines) > prior:
            return json.loads(lines[-1])['data']['snapshot']
        assert time.monotonic() < until and process.poll() is None, log.read_text()
        time.sleep(.02)


# Published model keys use underscores, while URI operations contain a slash.
def channel(base, operation, payload, expected=200):
    if expected == 200:
        validate('channels_' + operation + 'Request', payload)
    status, response = request(base + '/api/semantics/v1/channels/' + operation, payload)
    assert status == expected, (operation, status, expected, response)
    validate('channels_' + operation + 'Response' if status == 200 else 'error', response)
    return response.get('data', response.get('error'))


def bind(base, locator, name):
    preview = channel(base, 'preview', {'locator': locator})
    assert preview['dictionary'] and preview['source_snapshot']['schema_state'] == 'known'
    document = preview['dictionary']
    standard = call(base, 'save', {'kind': 'standard', 'name': 'channel.field', 'expected_current_revision': 0,
                   'document': {'schema_id': 'flowsql.data-standard.v1', 'title': '来源字段标准'}})
    document['fields'][0]['standard_refs'] = {'state': 'declared', 'value': [standard['exact_reference']], 'origin': 'manual'}
    document['fields'][0]['definition'] = {'state': 'declared', 'value': '人工说明', 'origin': 'manual'}
    saved = call(base, 'save', {'kind': 'dictionary', 'name': name, 'expected_current_revision': 0, 'document': document})
    source = preview['source_snapshot']
    payload = {'locator': locator, 'name': name, 'dictionary_ref': saved['exact_reference'],
               'field_map': preview['field_map'], 'expected_current_revision': 0,
               'source_basis': {key: source[key] for key in ['object_key', 'source_revision', 'schema_fingerprint']},
               'confirmations': preview['required_confirmations']}
    applied = channel(base, 'apply', payload)
    assert applied['check']['state'] == 'matched'
    payload['expected_current_revision'] = 1
    again = channel(base, 'apply', payload)
    assert again['exact_reference'] == applied['exact_reference'] and not again['created_revision']
    check = channel(base, 'check', {'locator': locator})
    assert check['state'] == 'matched' and check['dictionary_ref'] == saved['exact_reference']
    exact = call(base, 'resolve', {'exact_reference': check['binding_ref']})
    assert exact['document']['source_snapshot'] == source
    dictionary = call(base, 'resolve', {'exact_reference': check['dictionary_ref']})
    assert dictionary['document']['fields'][0]['definition']['value'] == '人工说明'
    # An ordinary OpenAPI client follows locator -> current observation -> exact binding/dictionary/standard context.
    spec = importlib.util.spec_from_file_location('channel_client', Path(__file__).resolve().parents[3] / 'examples/semantics/client.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    client = module.Client(base)
    observed = client.call('checkChannelDictionaryBinding', {'locator': locator})
    assert observed['binding_ref'] == check['binding_ref']
    key = source['object_key']
    consumed = client.consume(key['provider_id'], key['object_id'], [document['fields'][0]['field_id']])
    assert consumed['binding']['exact_reference'] == observed['binding_ref']
    assert consumed['dictionary_fields']['exact_reference'] == observed['dictionary_ref']
    assert standard['exact_reference'] in {item['exact_reference'] for item in consumed['dictionary_fields']['reference_context']}
    assert consumed['binding']['data_validation'] == 'not_run'
    return preview, payload, check


def timeout_contract(base, gateway, locator):
    observed = []

    class SilentPeer(BaseHTTPRequestHandler):
        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            observed.append((body['timeout_ms'], int(self.headers['X-FlowSQL-Metadata-Deadline-Ms'])))
            time.sleep(.4)
            try:
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b'{}')
            except (BrokenPipeError, ConnectionResetError):
                pass

        def log_message(self, *args):
            pass

    with ThreadingHTTPServer(('127.0.0.1', 0), SilentPeer) as peer:
        thread = threading.Thread(target=peer.serve_forever, kwargs={'poll_interval': .01})
        thread.start()
        try:
            assert request('http://127.0.0.1:' + str(gateway) + '/gateway/register',
                           {'prefix': '/channels/kafka', 'address': '127.0.0.1:' + str(peer.server_port)})[0] == 200
            wall = int(time.time() * 1000)
            started = time.monotonic()
            assert channel(base, 'check', {'locator': locator, 'timeout_ms': 80}, 504)['code'] == 'SOURCE_TIMEOUT'
            assert time.monotonic() - started < .35, 'one total budget must bound the whole forwarding chain'
            assert observed and 0 < observed[0][0] <= 80
            assert observed[0][1] <= wall + 90, 'forwarders must not restart the deadline'
        finally:
            peer.shutdown()
            thread.join()


def database_connect_timeouts(base):
    # Intentional negative endpoint: accepts a socket but never completes a database handshake.
    class SilentPeer(socketserver.BaseRequestHandler):
        def handle(self):
            time.sleep(.4)

    with socketserver.ThreadingTCPServer(('127.0.0.1', 0), SilentPeer) as peer:
        peer.daemon_threads = True
        thread = threading.Thread(target=peer.serve_forever, kwargs={'poll_interval': .01})
        thread.start()
        try:
            for dialect in ('mysql', 'postgres', 'clickhouse'):
                config = (f'type={dialect};name=timeout;host=127.0.0.1;port={peer.server_address[1]};'
                          'user=negative_timeout;database=negative_timeout')
                assert request(base + '/api/channels/database/add', {'config': config})[0] == 200
                locator = {'kind': 'database_table', 'type': dialect, 'name': 'timeout', 'table': 'absent'}
                started = time.monotonic()
                assert channel(base, 'describe', {'locator': locator, 'timeout_ms': 80}, 504)['code'] == 'SOURCE_TIMEOUT'
                assert time.monotonic() - started < .35, dialect + ' connection exceeded total budget'
        finally:
            peer.shutdown()
            thread.join()


def main(host):
    host = host.resolve()
    kafka = {'kind': 'kafka_source', 'name': 'events'}
    frame = {'kind': 'dataframe', 'name': 'sample'}
    table = {'kind': 'database_table', 'type': 'sqlite', 'name': 'sample', 'table': 'empty_table'}
    with tempfile.TemporaryDirectory(prefix='flowsql-dictionary-http-') as tmp:
        root = Path(tmp)
        with running(host, root / 'local') as (base, process, log, ports):
            snapshot = channel(base, 'describe', {'locator': kafka})['source_snapshot']
            assert snapshot == iid(process, log, kafka)
            assert len(snapshot['fields']) == 6
            before = channel(base, 'describe', {'locator': frame})['source_snapshot']
            assert before['schema_state'] == 'pending'
            assert channel(base, 'check', {'locator': frame})['state'] == 'pending'
            assert iid(process, log, frame)['schema_state'] == 'pending'
            status, loaded = request(base + '/api/channels/dataframe/preview', {'name': 'sample'})
            assert status == 200, loaded
            assert channel(base, 'describe', {'locator': frame})['source_snapshot']['schema_state'] == 'known'
            _, events_apply, _ = bind(base, kafka, 'events')
            imported = call(base, 'resolve', {'exact_reference': 'binding.events@1'})['document']
            imported['object_label'] = '历史导入声明'
            document = call(base, 'import', {'kind': 'binding', 'name': 'events', 'format': 'json',
                            'content': json.dumps(imported)})['document']
            call(base, 'save', {'kind': 'binding', 'name': 'events', 'expected_current_revision': 1, 'document': document})
            assert channel(base, 'check', {'locator': kafka})['state'] == 'needs_confirmation'
            observed = channel(base, 'preview', {'locator': kafka})
            assert 'observation' in observed['required_confirmations']
            events_apply['expected_current_revision'] = 2
            events_apply['confirmations'] = observed['required_confirmations']
            assert channel(base, 'apply', events_apply)['check']['state'] == 'matched'
            bind(base, frame, 'sample')
            bind(base, table, 'empty_table')
            current = channel(base, 'describe', {'locator': frame})['source_snapshot']
            assert before['object_key'] == current['object_key']
            assert request(base + '/api/channels/dataframe/rename', {'name': 'sample', 'new_name': 'renamed'})[0] == 200
            renamed = dict(frame, name='renamed')
            assert channel(base, 'check', {'locator': renamed})['state'] == 'matched'
            assert channel(base, 'describe', {'locator': renamed})['source_snapshot']['object_key'] == current['object_key']
            channel(base, 'describe', {'locator': kafka, 'timeout_ms': 0}, 400)
            channel(base, 'check', {'locator': {'kind': 'dataframe', 'name': 'absent'}}, 404)
            database_connect_timeouts(base)
        # Both process-local registry identity and provider identity survive a clean restart.
        with running(host, root / 'local') as (base, process, log, ports):
            assert channel(base, 'check', {'locator': kafka})['state'] == 'matched'
            assert channel(base, 'check', {'locator': table})['state'] == 'matched'
            assert channel(base, 'check', {'locator': renamed})['state'] == 'pending'
            assert request(base + '/api/channels/dataframe/preview', {'name': 'renamed'})[0] == 200
            assert channel(base, 'check', {'locator': renamed})['state'] == 'matched'
        with running(host, root / 'control', 'control') as (base, control, log, ports):
            with running(host, root / 'remote', 'sources', ports[0]) as (_, source, source_log, _):
                until = time.monotonic() + 15
                while True:
                    status, value = request(base + '/api/semantics/v1/channels/describe', {'locator': kafka})
                    if status == 200:
                        break
                    assert time.monotonic() < until, (status, value, source_log.read_text())
                    time.sleep(.1)
                assert value['data']['source_snapshot'] == iid(source, source_log, kafka)
                status, config_list = request(base + '/api/channels/config/list', {})
                assert status == 200, config_list
                routes = request('http://127.0.0.1:' + str(ports[0]) + '/gateway/routes')[1]
                text = json.dumps(routes)
                assert '/channels/kafka' in text and '/channels/config' in text and '/channels/dataframe' in text
                bind(base, kafka, 'remote')
                bind(base, table, 'remote_table')
                assert channel(base, 'describe', {'locator': frame})['source_snapshot']['schema_state'] == 'pending'
            assert channel(base, 'check', {'locator': kafka}, 503)['code'] == 'SOURCE_UNAVAILABLE'
            timeout_contract(base, ports[0], kafka)
    print('Channel dictionary actual HTTP, IID, restart and cross-process contracts passed')


if __name__ == '__main__':
    main(Path(sys.argv[1]))
