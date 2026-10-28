# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Socket-free integration of real providers and Config through PluginLoader/IQuerier."""
import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from test_data_semantics_api import validate


def main(host):
    with tempfile.TemporaryDirectory(prefix='flowsql-dictionary-iid-') as tmp:
        root = Path(tmp)
        with (root / 'host.log').open('w') as log:
            process = subprocess.Popen([str(host.resolve()), tmp, '18800', '18801', '18802', 'iid'],
                                       stdin=subprocess.PIPE, stdout=log, stderr=log, text=True, cwd=host.parent)
            try:
                until = time.monotonic() + 15
                while 'DATASET_READY' not in (root / 'host.log').read_text():
                    assert process.poll() is None and time.monotonic() < until, (root / 'host.log').read_text()
                    time.sleep(.02)

                def route(path, payload, expected=0):
                    prefix = 'DATASET_ROUTE '
                    before = (root / 'host.log').read_text().count(prefix)
                    process.stdin.write(json.dumps({'route': path, 'request': payload}) + '\n')
                    process.stdin.flush()
                    until = time.monotonic() + 5
                    while True:
                        lines = [line.split(prefix, 1)[1] for line in (root / 'host.log').read_text().splitlines() if prefix in line]
                        if len(lines) > before:
                            code, result = lines[-1].split(' ', 1)
                            assert int(code) == expected, result
                            return json.loads(result)
                        assert process.poll() is None and time.monotonic() < until, (root / 'host.log').read_text()
                        time.sleep(.01)

                def semantics(op, data):
                    key = op.replace('/', '_')
                    validate(key + 'Request', data)
                    result = route('/semantics/v1/' + op, data)
                    validate(key + 'Response', result)
                    return result['data']

                frame = {'kind': 'dataframe', 'name': 'sample'}
                assert semantics('channels/check', {'locator': frame})['state'] == 'pending'
                assert semantics('channels/preview', {'locator': frame})['dictionary'] is None
                route('/channels/dataframe/preview', {'name': 'sample'})
                for name, locator in [('frame', frame), ('events', {'kind': 'kafka_source', 'name': 'events'})]:
                    preview = semantics('channels/preview', {'locator': locator})
                    doc = preview['dictionary']
                    doc['fields'][0]['definition'] = {'state': 'declared', 'origin': 'manual', 'value': '人工说明'}
                    saved = semantics('save', {'kind': 'dictionary', 'name': name, 'expected_current_revision': 0, 'document': doc})
                    source = preview['source_snapshot']
                    applied = semantics('channels/apply', {'locator': locator, 'name': name, 'dictionary_ref': saved['exact_reference'],
                        'field_map': preview['field_map'], 'expected_current_revision': 0,
                        'source_basis': {k: source[k] for k in ['object_key', 'source_revision', 'schema_fingerprint']},
                        'confirmations': preview['required_confirmations']})
                    assert applied['check']['state'] == 'matched'
                    check = semantics('channels/check', {'locator': locator})
                    assert check['state'] == 'matched'
                    binding = semantics('resolve', {'exact_reference': check['binding_ref']})
                    assert binding['document']['source_snapshot'] == source
                    again = semantics('channels/preview', {'locator': locator})
                    assert again['dictionary']['fields'][0]['definition']['value'] == '人工说明'
                    assert again['dictionary']['fields'][0]['field_id'] == doc['fields'][0]['field_id']
            finally:
                if process.poll() is None:
                    process.communicate('quit\n', timeout=15)
                assert process.returncode == 0, (root / 'host.log').read_text()
    print('Real Config / Kafka / Catalog IID generation and binding integration passed')


if __name__ == '__main__':
    main(Path(sys.argv[1]))
