# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Native temporary SQLite + actual Web/Gateway/Router/Config contract acceptance."""
import base64
import contextlib
import concurrent.futures
import importlib.util
import threading
import hashlib
import json
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

import jsonschema
import yaml

ROOT = Path(__file__).resolve().parents[3]
CONTRACT = json.loads((ROOT / 'config/semantics/api.schema.json').read_text())


def validate(key, value):
    schema = dict(CONTRACT, **{'$ref': '#/%24defs/' + key})
    jsonschema.Draft202012Validator(schema).validate(value)


def request(url, payload=None, raw=None):
    encoded = raw if raw is not None else json.dumps(payload, ensure_ascii=False).encode() if payload is not None else None
    req = urllib.request.Request(url, data=encoded, headers={'Content-Type': 'application/json'})
    try:
        response = urllib.request.urlopen(req, timeout=15)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        return response.status, json.load(response)


def call(base, operation, payload, expected=200, raw=None):
    if expected == 200:
        validate(operation + 'Request', payload)
    status, result = request(base + '/api/semantics/v1/' + operation, payload, raw)
    assert status == expected, (operation, status, expected, result)
    validate(operation + 'Response' if status == 200 else 'error', result)
    return result.get('data', result.get('error'))


def port(used):
    while True:
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            value = sock.getsockname()[1]
        if value not in used:
            used.add(value)
            return value


@contextlib.contextmanager
def running_host(host, directory, provider=True):
    used = set()
    ports = [port(used) for _ in range(3)]
    base = 'http://127.0.0.1:' + str(ports[2])
    log_path = directory / ('host-' + str(ports[2]) + '.log')
    with log_path.open('w') as log:
        process = subprocess.Popen([str(host), str(directory), *map(str, ports), *([] if provider else ['without-provider'])], stdin=subprocess.PIPE,
                                   stdout=log, stderr=log, text=True, cwd=host.parent)
        try:
            deadline = time.monotonic() + 20
            while True:
                try:
                    status, result = request(base + '/api/semantics/v1/list', {}) if provider else request(base + '/api/semantics/v1/openapi.json')
                    if status == 200:
                        break
                except OSError:
                    pass
                if process.poll() is not None or time.monotonic() >= deadline:
                    raise AssertionError(log_path.read_text())
                time.sleep(.05)
            yield base, process, log_path
        finally:
            if process.poll() is None:
                try:
                    process.communicate('quit\n', timeout=15)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
                    raise AssertionError('test host did not stop')
            assert process.returncode == 0, log_path.read_text()


def iid(process, log_path, reference):
    prior = log_path.read_text().count('SEMANTICS_IID ')
    process.stdin.write(reference + '\n')
    process.stdin.flush()
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        lines = [line.split('SEMANTICS_IID ', 1)[1] for line in log_path.read_text().splitlines() if 'SEMANTICS_IID ' in line]
        if len(lines) > prior:
            return json.loads(lines[-1])
        time.sleep(.02)
    raise AssertionError('IID read did not complete')


def example(kind):
    return json.loads((ROOT / 'examples/semantics' / (kind + '.json')).read_text())


def save(base, kind, name, document, expected=0, status=200, **extra):
    return call(base, 'save', dict(kind=kind, name=name, expected_current_revision=expected, document=document, **extra), status)


def main():
    host = Path(sys.argv[1]).resolve()
    for name in ('api.schema', 'documents.schema'):
        jsonschema.Draft202012Validator.check_schema(json.loads((ROOT / 'config/semantics' / (name + '.json')).read_text()))
    with tempfile.TemporaryDirectory(prefix='flowsql-semantics-http-') as temporary:
        directory = Path(temporary)
        with running_host(host, directory) as (base, process, log):
            for filename in ('openapi.json', 'api.schema.json', 'documents.schema.json'):
                status, served = request(base + '/api/semantics/v1/' + filename)
                assert status == 200 and served == json.loads((ROOT / 'config/semantics' / filename).read_text())
            api = request(base + '/api/semantics/v1/openapi.json')[1]
            assert api['openapi'].startswith('3.1.')
            ids = [entry['post']['operationId'] for entry in api['paths'].values()]
            assert len(ids) == len(set(ids)) == 16
            assert {'describe', 'channels_save_description', 'operators_save_description'} <= set(ids)
            for path, entry in api['paths'].items():
                assert entry['post']['requestBody']['content']['application/json']['schema']['$ref'].endswith(path[1:].replace('/', '_') + 'Request')
            legacy = json.dumps({'legacy': '原配置\n保留'}, ensure_ascii=False).encode()
            status, old = request(base + '/api/channels/config/publish', {'name': 'legacy', 'expected_current_revision': 0, 'format': 'json', 'schema_id': 'legacy', 'content_base64': base64.b64encode(legacy).decode()})
            assert status == 200 and old['revision'] == 1
            standard = save(base, 'standard', 'request.duration', example('standard'))
            dictionary = save(base, 'dictionary', 'app.requests', example('dictionary'))
            binding = save(base, 'binding', 'app.requests', example('binding'))
            assert binding['structure_check'] == 'matched' and binding['data_validation'] == 'not_run'
            assert not save(base, 'standard', 'request.duration', example('standard'))['created_revision']
            resolved = call(base, 'resolve', {'exact_reference': dictionary['exact_reference']})
            canonical = json.dumps(resolved['document'], sort_keys=True, ensure_ascii=False, separators=(',', ':')).encode()
            assert hashlib.sha256(canonical).hexdigest() == resolved['sha256']
            local = iid(process, log, dictionary['exact_reference'])
            assert local['ok'] and local['document'] == resolved['document'] and local['sha256'] == resolved['sha256']
            assert resolved['semantic_gaps'] and resolved['reference_context'][0]['exact_reference'] == standard['exact_reference']
            assert dictionary['exact_reference'] in {x['exact_reference'] for x in call(base, 'list', {'q': 'elapsed_ms'})['items']}
            assert call(base, 'list', {'q': '请求经过时间'})['items']
            first = call(base, 'list', {'limit': 1})
            assert first['has_more'] and first['next_cursor']
            second = call(base, 'list', {'limit': 1, 'cursor': first['next_cursor']})
            assert first['items'][0]['exact_reference'] != second['items'][0]['exact_reference']
            assert call(base, 'list', {'kind': 'standard', 'cursor': first['next_cursor']}, 400)['code'] == 'INVALID_DOCUMENT'
            assert call(base, 'list', {'limit': 0}, 400)['code'] == 'INVALID_DOCUMENT'
            assert call(base, 'list', {}, 400, raw=b'{"q":"x","q":"y"}')['code'] == 'INVALID_DOCUMENT'
            projection = call(base, 'resolve', {'exact_reference': dictionary['exact_reference'], 'view': 'fields', 'field_ids': ['elapsed'], 'limit': 1})
            assert projection['projection'] and projection['fields'][0]['field_id'] == 'elapsed'
            assert projection['context_fields'][0]['field_id'] == 'request_id' and projection['required_context_refs']
            assert projection['entity']['row_semantics']['value'] == 'event'
            page = call(base, 'resolve', {'exact_reference': dictionary['exact_reference'], 'view': 'fields', 'limit': 1})
            more = call(base, 'resolve', {'exact_reference': dictionary['exact_reference'], 'view': 'fields', 'limit': 1, 'cursor': page['next_cursor']})
            assert more['fields'][0]['field_id'] == 'elapsed' and not more['has_more']
            call(base, 'resolve', {'exact_reference': dictionary['exact_reference'], 'view': 'fields', 'field_ids': ['absent']}, 400)
            call(base, 'resolve', {'exact_reference': standard['exact_reference'], 'view': 'fields'}, 400)
            assert call(base, 'resolve', {'exact_reference': 'dictionary.app.requests@latest'}, 400)['code'] == 'INVALID_REFERENCE'
            assert call(base, 'resolve', {'exact_reference': 'dictionary.app.requests@999'}, 404)['code'] == 'NOT_FOUND'
            bound = call(base, 'binding_lookup', {'object_key': {'provider_id': 'manual', 'object_id': 'app.requests'}})
            assert bound['dictionary_ref'] == dictionary['exact_reference']
            bound_snapshot = call(base, 'resolve', {'exact_reference': bound['exact_reference']})
            assert {x['exact_reference'] for x in bound_snapshot['reference_context']} == {dictionary['exact_reference'], standard['exact_reference']}
            refs = call(base, 'references', {'exact_reference': standard['exact_reference'], 'scope': 'current'})
            assert refs['items'][0]['source_reference'] == dictionary['exact_reference']
            for format in ('json', 'yaml'):
                exported = call(base, 'export', {'exact_reference': dictionary['exact_reference'], 'format': format})
                imported = call(base, 'import', {'kind': 'dictionary', 'name': 'preview.copy', 'format': format, 'content': exported['content']})
                assert imported['document'] == resolved['document'] and imported['operation_source'] == 'import'
                call(base, 'history', {'kind': 'dictionary', 'name': 'preview.copy'}, 404)
                created = save(base, 'dictionary', 'copy.' + format, imported['document'])
                assert created['revision'] == 1
            for format, content in [('json', '{"schema_id":"x","schema_id":"y"}'), ('yaml', 'a: 1\na: 2'), ('yaml', 'a: &x 1\nb: *x'), ('yaml', 'a: !custom x'), ('yaml', 'a: .nan'), ('json', '{"$ref":"https://example.invalid/schema"}')]:
                call(base, 'import', {'kind': 'standard', 'name': 'negative', 'format': format, 'content': content}, 400)
            too_deep = '[' * 65 + '0' + ']' * 65
            call(base, 'import', {'kind': 'standard', 'name': 'negative', 'format': 'json', 'content': too_deep}, 400)
            call(base, 'import', {'kind': 'standard', 'name': 'negative', 'format': 'json', 'content': 'x' * (8 * 1024 * 1024 + 1)}, 413)
            call(base, 'list', {}, 413, raw=b' ' * (12 * 1024 * 1024 + 1))
            precise = example('dictionary')
            precise['fields'][0]['type']['parameters']['integer'] = 18446744073709551615
            precise['fields'][0]['definition'] = {'state': 'declared', 'value': 'preserved origin', 'origin': 'producer'}
            for format in ('json', 'yaml'):
                content = json.dumps(precise) if format == 'json' else yaml.safe_dump(precise)
                preview = call(base, 'import', {'kind': 'dictionary', 'name': 'precise', 'format': format, 'content': content})
                assert preview['document']['fields'][0]['type']['parameters']['integer'] == 18446744073709551615
                assert preview['document']['fields'][0]['definition']['origin'] == 'producer'
            precise['fields'][0]['type']['parameters']['integer'] = 18446744073709551616
            call(base, 'import', {'kind': 'dictionary', 'name': 'precise', 'format': 'yaml', 'content': yaml.safe_dump(precise)}, 400)
            call(base, 'import', {'kind': 'dictionary', 'name': 'precise', 'format': 'json', 'content': json.dumps(precise)}, 400)
            big = {'schema_id': 'flowsql.data-standard.v1', 'title': 'x' * (6 * 1024 * 1024 + 1024)}
            large = save(base, 'standard', 'large', big)
            assert call(base, 'export', {'exact_reference': large['exact_reference'], 'format': 'json'}, 413)['code'] == 'LIMIT_EXCEEDED'
            changed = example('dictionary'); changed['title'] = '新版定义'
            newer = save(base, 'dictionary', 'app.requests', changed, expected=1)
            assert newer['revision'] == 2
            assert call(base, 'binding_lookup', {'object_key': {'provider_id': 'manual', 'object_id': 'app.requests'}})['dictionary_ref'].endswith('@1')
            assert save(base, 'dictionary', 'app.requests', example('dictionary'), expected=0, status=409)['code'] == 'REVISION_CONFLICT'
            history = call(base, 'history', {'kind': 'dictionary', 'name': 'app.requests', 'limit': 1})
            assert history['items'][0]['revision'] == 2 and history['has_more']
            restored = save(base, 'dictionary', 'app.requests', example('dictionary'), expected=2, base_revision=1)
            assert restored['revision'] == 3
            assert call(base, 'resolve', {'exact_reference': dictionary['exact_reference']})['sha256'] == resolved['sha256']
            with sqlite3.connect(directory / 'config.db') as connection:
                connection.execute("CREATE TRIGGER semantics_http_fault BEFORE INSERT ON semantics_revision BEGIN SELECT RAISE(ABORT,'injected'); END")
            assert save(base, 'standard', 'failure', example('standard'), status=500)['code'] == 'STORAGE_ERROR'
            call(base, 'history', {'kind': 'standard', 'name': 'failure'}, 404)
            with sqlite3.connect(directory / 'config.db') as connection:
                connection.execute('DROP TRIGGER semantics_http_fault')
            # Failure after inserting content/current must roll back the reference index as well.
            with sqlite3.connect(directory / 'config.db') as connection:
                connection.execute("CREATE TRIGGER semantics_reference_fault BEFORE INSERT ON semantics_reference BEGIN SELECT RAISE(ABORT,'injected reference write'); END")
            assert save(base, 'dictionary', 'rolled_back', example('dictionary'), status=500)['code'] == 'STORAGE_ERROR'
            call(base, 'resolve', {'exact_reference': 'dictionary.rolled_back@1'}, 404)
            assert not iid(process, log, 'dictionary.rolled_back@1')['ok']
            with sqlite3.connect(directory / 'config.db') as connection:
                assert connection.execute("SELECT count(*) FROM semantics_revision WHERE name='rolled_back'").fetchone()[0] == 0
                assert connection.execute("SELECT count(*) FROM semantics_object WHERE name='rolled_back'").fetchone()[0] == 0
                assert connection.execute("SELECT count(*) FROM semantics_reference WHERE source='dictionary.rolled_back@1'").fetchone()[0] == 0
                connection.execute('DROP TRIGGER semantics_reference_fault')
            assert save(base, 'dictionary', 'rolled_back', example('dictionary'))['revision'] == 1
            # Two independent HTTP clients start together and compete for the same current revision.
            race = save(base, 'standard', 'race', example('standard'))
            gate = threading.Barrier(2)
            def competing(title):
                document = example('standard'); document['title'] = title
                payload = dict(kind='standard', name='race', expected_current_revision=1, document=document)
                validate('saveRequest', payload)
                gate.wait(timeout=5)
                status, result = request(base + '/api/semantics/v1/save', payload)
                validate('saveResponse' if status == 200 else 'error', result)
                return status, result
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                left = pool.submit(competing, 'client one')
                right = pool.submit(competing, 'client two')
                races = [left.result(timeout=20), right.result(timeout=20)]
            assert sorted(status for status, _ in races) == [200, 409]
            assert next(result['error']['code'] for status, result in races if status == 409) == 'REVISION_CONFLICT'
            assert [item['revision'] for item in call(base, 'history', {'kind': 'standard', 'name': 'race'})['items']] == [2, 1]
            race_snapshot = call(base, 'resolve', {'exact_reference': 'standard.race@2'})
            assert iid(process, log, 'standard.race@2')['sha256'] == race_snapshot['sha256']
            # A stdlib client discovers operationIds from the shipped OpenAPI and follows the exact binding.
            spec = importlib.util.spec_from_file_location('semantics_example', ROOT / 'examples/semantics/client.py')
            module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
            client = module.Client(base)
            consumed = client.consume('manual', 'app.requests', ['elapsed'])
            for key, response in [('resolve', consumed['binding']), ('resolve', consumed['dictionary_fields']), ('references', consumed['references'])]:
                validate(key + 'Response', {'data': response})
            assert consumed['dictionary_fields']['exact_reference'] == dictionary['exact_reference']
            assert consumed['dictionary_fields']['semantic_gaps'] and consumed['dictionary_fields']['required_context_refs']
            assert consumed['dictionary_fields']['entity']['limitations']['state'] == 'declared'
            assert consumed['binding']['data_validation'] == 'not_run'
            status, old = request(base + '/api/channels/config/resolve', {'exact_reference': 'config.legacy@1'})
            assert status == 200 and base64.b64decode(old['content_base64']) == legacy
        with running_host(host, directory) as (base, process, log):
            recovered = call(base, 'resolve', {'exact_reference': dictionary['exact_reference']})
            assert recovered['document'] == resolved['document'] and recovered['sha256'] == resolved['sha256']
            assert iid(process, log, dictionary['exact_reference'])['document'] == resolved['document']
            assert call(base, 'resolve', {'exact_reference': 'standard.race@2'})['sha256'] == race_snapshot['sha256']
            assert iid(process, log, 'standard.race@2')['sha256'] == race_snapshot['sha256']
            rebound = call(base, 'binding_lookup', {'object_key': {'provider_id': 'manual', 'object_id': 'app.requests'}})
            assert rebound == bound
            assert client.operations == module.Client(base).operations
            status, old = request(base + '/api/channels/config/resolve', {'exact_reference': 'config.legacy@1'})
            assert status == 200 and base64.b64decode(old['content_base64']) == legacy
        absent = directory / 'unavailable'; absent.mkdir()
        with running_host(host, absent, provider=False) as (base, _, _):
            assert call(base, 'list', {}, 503)['code'] == 'UNAVAILABLE'
    print('PASS A4/A5/A7/A8: eight operations, schemas, projections, exchange, budgets, concurrent HTTP clients, mid-transaction rollback, process recovery, IID agreement, legacy bytes and OpenAPI consumer')


if __name__ == '__main__':
    main()
