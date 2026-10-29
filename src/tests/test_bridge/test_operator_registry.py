# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Candidate failure isolation and real Worker/Catalog registration acceptance."""
import copy
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'src/python'))
from flowsql.operator_registry import OperatorRegistry, ComponentChanged


def source(declaration, name='demo', suffix=''):
    return ('import json\nimport polars as pl\nfrom flowsql import OperatorBase, register_operator\n'
            f'@register_operator(category="py", name="{name}", data_contract=json.loads({json.dumps(declaration)!r}))\n'
            'class Example(OperatorBase):\n'
            '    def __init__(self):\n        super().__init__()\n        self.factor = 1\n'
            '    def configure(self, key, value):\n        super().configure(key, value)\n'
            '        if key == "factor":\n            self.factor = int(value)\n'
            '    def resolve_invocation(self, inputs, execution_context):\n'
            '        return dict(effective_parameters=dict(self._config, factor=str(self.factor)), config_refs=[], inputs=inputs)\n'
            '    def work(self, df_in):\n        return df_in.with_columns((pl.col("x") * self.factor).alias("x"))\n' + suffix)


def contract():
    value = json.loads((ROOT / 'config/semantics/examples/operator-contract-passthrough.json').read_text())
    value['operator'].update(category='py', name='demo')
    value['inputs'][0]['roles'] = [dict(role_id='value', required=True,
        types=[dict(system='arrow', name='int64', parameters={})], nullable=None,
        semantic_requirements={}, selector=dict(kind='column', column='x'))]
    return value


def unit():
    with tempfile.TemporaryDirectory(prefix='flowsql_registry_') as temporary:
        directory = Path(temporary)
        good = source(contract())
        path = directory / 'py_demo.py'
        path.write_text(good)
        registry = OperatorRegistry()
        registry.discover(directory)
        original = registry.get('py', 'demo')
        revision = hashlib.sha256(good.encode()).hexdigest()
        assert registry.list_operators()[0]['component_revision'] == revision
        prepared_a = registry.prepare('py', 'demo', revision, {'factor': '3'}, [], {})
        prepared_b = registry.prepare('py', 'demo', revision, {'factor': '7'}, [], {})
        assert prepared_a['effective_parameters']['factor'] == '3'
        assert prepared_b['effective_parameters']['factor'] == '7'
        defaulted = registry.prepare('py', 'demo', revision, {}, [], {})
        assert defaulted['effective_parameters']['factor'] == '1'
        registry.release_invocation(defaulted['invocation_token'])
        path.write_text('invalid python !')
        try:
            registry.reload(directory)
            raise AssertionError('bad candidate accepted')
        except SyntaxError:
            pass
        assert registry.get('py', 'demo') is original
        path.write_text(good)
        candidate = registry.stage_source(path.name, source(contract(), suffix='# newer\n'), revision)
        other = registry.stage_source(path.name, good, revision)
        registry.transition(candidate['token'], 'activate')
        try:
            registry.transition(other['token'], 'activate')
            raise AssertionError('stale candidate accepted')
        except ComponentChanged:
            pass
        registry.transition(candidate['token'], 'rollback')
        registry.transition(other['token'], 'rollback')
        assert registry.get('py', 'demo') is original and path.read_text() == good
        changed = registry.stage_source(path.name, good + '# committed\n', revision)
        registry.transition(changed['token'], 'activate')
        registry.transition(changed['token'], 'finalize')
        import polars as pl
        frame = pl.DataFrame({'x': [2]})
        a = registry.take_invocation('py', 'demo', revision, prepared_a['invocation_token'])
        b = registry.take_invocation('py', 'demo', revision, prepared_b['invocation_token'])
        assert a is not b and a.work(frame)['x'].to_list() == [6] and b.work(frame)['x'].to_list() == [14]
        try:
            registry.take_invocation('py', 'demo', revision, prepared_a['invocation_token'])
            raise AssertionError('consumed invocation replayed')
        except ComponentChanged:
            pass
        try:
            registry.prepare('py', 'demo', 'missing', {}, [], {})
            raise AssertionError('missing component silently changed')
        except ComponentChanged:
            pass
        assert registry.get_component('py', 'demo', revision).instance is original
        assert registry.read_source('py', 'demo')['source'] == good + '# committed\n'
        (directory / 'duplicate.py').write_text(good)
        before = registry.list_operators()
        try:
            registry.reload(directory)
            raise AssertionError('duplicate candidate accepted')
        except ValueError:
            pass
        assert registry.list_operators() == before
    print('PASS: actual source digest, atomic reload, candidate CAS/rollback, frozen old component, duplicate isolation')


def integration(executable):
    import httpx
    with tempfile.TemporaryDirectory(prefix='flowsql_contract_python_') as temporary:
        root = Path(temporary)
        (root / 'operators').mkdir()
        doc = contract()
        (root / 'good.py').write_text(source(doc))
        (root / 'changed.py').write_text(source(doc).replace('self.factor = 1', 'self.factor = 11'))
        invalid = copy.deepcopy(doc)
        invalid['inputs'][0]['max_count'] = -1
        (root / 'invalid.py').write_text(source(invalid))
        (root / 'missing.py').write_text(source('operator_contract.py.demo@999'))
        (root / 'reference.py').write_text(source('operator_contract.py.demo@1').replace(
            'pl.col("x") * self.factor', 'pl.col("x") * self.factor + 1000'))
        (root / 'legacy.py').write_text(source(None, name='legacy'))
        # Every declaration in a component is validated before switching any one of them.
        other = copy.deepcopy(doc)
        other['operator']['name'] = 'other'
        (root / 'mixed.py').write_text(source(other, name='other') + source(invalid))
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            port = listener.getsockname()[1]
        env = dict(os.environ, PYTHONPATH=str(ROOT / 'src/python'))
        with (root / 'worker.log').open('w+') as log:
            worker = subprocess.Popen([sys.executable, '-m', 'flowsql.worker', '--port', str(port),
                                       '--operators-dir', str(root / 'operators')], env=env, stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 15
                while True:
                    if worker.poll() is not None:
                        log.seek(0)
                        raise AssertionError(log.read())
                    try:
                        if httpx.get(f'http://127.0.0.1:{port}/operators/python/health', timeout=0.2).status_code == 200:
                            break
                    except httpx.HTTPError:
                        pass
                    if time.monotonic() > deadline:
                        raise AssertionError('real Worker readiness deadline exceeded')
                    time.sleep(0.05)
                subprocess.run([executable, str(port), str(root)], check=True, timeout=60)
                # The old no-contract endpoint still executes a real Arrow DataFrame.
                import pyarrow as pa
                input_path = root / 'legacy_in'
                with pa.OSFile(str(input_path), 'wb') as stream:
                    with pa.ipc.new_stream(stream, pa.schema([('x', pa.int64())])) as writer:
                        writer.write_table(pa.table({'x': pa.array([1, 2], type=pa.int64())}))
                result = httpx.post(f'http://127.0.0.1:{port}/operators/python/work/py/legacy',
                                    json={'input': str(input_path)}, timeout=5)
                assert result.status_code == 200, result.text
                with pa.memory_map(result.json()['output']) as mapped:
                    assert pa.ipc.open_stream(mapped).read_all().schema == pa.schema([('x', pa.int64())])
            finally:
                worker.terminate()
                worker.wait(timeout=5)
    print('PASS: real FastAPI Worker and Arrow IPC compatibility')


if __name__ == '__main__':
    if len(sys.argv) == 1:
        unit()
    else:
        integration(sys.argv[1])
