"""Python component snapshots. Compile candidates before replacing live registrations."""

import copy
from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import sys
import tempfile
import threading
import time
import types
import uuid

from .operator_base import OperatorBase, _registration_target


@dataclass(frozen=True)
class Component:
    cls: type
    instance: OperatorBase
    filename: str
    revision: str
    declaration: dict | None
    source: bytes

    def metadata(self):
        attr = self.instance.attribute()
        return dict(category=attr.category, name=attr.name, description=attr.description,
                    position=attr.position, component_revision=self.revision,
                    source_filename=self.filename, data_contract=copy.deepcopy(self.declaration))


class ComponentChanged(ValueError):
    pass


class OperatorRegistry:
    def __init__(self):
        self._components = {}
        self._history = {}
        self._staged = {}
        self._generation = 0
        self._invocations = {}
        self._lock = threading.RLock()
        self.operators_dir = None

    @staticmethod
    def _compile(filename, source, directory):
        if Path(filename).name != filename or not filename.endswith('.py') or filename.startswith('_'):
            raise ValueError('invalid Python component filename')
        if len(source) > 8 * 1024 * 1024:
            raise ValueError('component exceeds 8 MiB')
        digest = hashlib.sha256(source).hexdigest()
        module_name = f'_flowsql_{Path(filename).stem}_{digest}_{uuid.uuid4().hex}'
        module = types.ModuleType(module_name)
        module.__file__ = str(directory / filename)
        sys.modules[module_name] = module
        captured = {}
        context = _registration_target.set(captured)
        try:
            exec(compile(source, module.__file__, 'exec'), module.__dict__)
            entries = {}
            for key, cls in captured.items():
                if cls.__module__ != module_name:
                    continue
                instance = cls()
                attr = instance.attribute()
                if key != f'{attr.category}.{attr.name}':
                    raise ValueError('decorator and runtime operator identity differ')
                entries[key] = Component(cls, instance, filename, digest,
                                         copy.deepcopy(getattr(cls, '_data_contract', None)), source)
            if not entries:
                # Helper modules remain importable without inventing operator identities.
                return {}
            return entries
        except BaseException:
            sys.modules.pop(module_name, None)
            raise
        finally:
            _registration_target.reset(context)

    def _install(self, entries):
        self._components = entries
        self._history.update({(key, entry.revision): entry for key, entry in entries.items()})
        self._generation += 1

    def discover(self, operators_dir):
        directory = Path(operators_dir).resolve()
        directory.mkdir(parents=True, exist_ok=True)
        if str(directory) not in sys.path:
            sys.path.insert(0, str(directory))
        with self._lock:
            generation = self._generation
        entries = {}
        for path in sorted(directory.glob('*.py')):
            if path.name.startswith('_'):
                continue
            component = self._compile(path.name, path.read_bytes(), directory)
            if entries.keys() & component.keys():
                raise ValueError('duplicate operator identity across components')
            entries.update(component)
        with self._lock:
            if generation != self._generation:
                raise ComponentChanged('registry changed during discovery')
            self.operators_dir = directory
            self._install(entries)

    def reload(self, operators_dir):
        self.discover(operators_dir)

    def stage_source(self, filename, source, expected_revision=''):
        if self.operators_dir is None:
            raise ValueError('operator directory unavailable')
        source_bytes = source.encode('utf-8')
        candidate = self._compile(filename, source_bytes, self.operators_dir)
        if not candidate:
            raise ValueError('component declares no operators')
        with self._lock:
            previous = {key: entry for key, entry in self._components.items() if entry.filename == filename}
            revisions = {entry.revision for entry in previous.values()}
            if expected_revision and revisions != {expected_revision}:
                raise ComponentChanged('source component changed')
            entries = {key: entry for key, entry in self._components.items() if entry.filename != filename}
            if entries.keys() & candidate.keys():
                raise ValueError('operator identity already belongs to another component')
            entries.update(candidate)
            if len(self._staged) >= 16:
                raise ValueError('too many pending component publications')
            token = uuid.uuid4().hex
            self._staged[token] = dict(entries=entries, source=source_bytes, filename=filename,
                                       generation=self._generation, before=self._components)
            return dict(token=token, operators=[entry.metadata() for entry in entries.values()])

    def transition(self, token, action):
        with self._lock:
            staged = self._staged.get(token)
            if staged is None:
                raise ComponentChanged('candidate is no longer available')
            path = self.operators_dir / staged['filename']
            if action == 'activate':
                if staged.get('activated') or staged['generation'] != self._generation:
                    raise ComponentChanged('registry changed since preparation')
                staged['old_source'] = path.read_bytes() if path.exists() else None
                self._write_source(path, staged['source'])
                self._install(staged['entries'])
                staged['activated'] = self._generation
            elif action == 'rollback':
                if staged.get('activated'):
                    if staged['activated'] != self._generation:
                        raise ComponentChanged('cannot roll back a newer registry')
                    if staged['old_source'] is None:
                        path.unlink(missing_ok=True)
                    else:
                        self._write_source(path, staged['old_source'])
                    self._install(staged['before'])
                self._staged.pop(token)
            elif action == 'finalize':
                self._staged.pop(token)
            else:
                raise ValueError('invalid publication action')

    @staticmethod
    def _write_source(path, source):
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix='.component-', delete=False) as stream:
            temporary = stream.name
            stream.write(source)
        try:
            os.replace(temporary, path)
        finally:
            Path(temporary).unlink(missing_ok=True)

    def get_component(self, category, name, revision=''):
        key = f'{category}.{name}'
        with self._lock:
            return self._history.get((key, revision)) if revision else self._components.get(key)

    def read_source(self, category, name):
        component = self.get_component(category, name)
        if component is None:
            raise ValueError('operator not found')
        return dict(filename=component.filename, source=component.source.decode('utf-8'),
                    component_revision=component.revision)

    def get(self, category, name):
        component = self.get_component(category, name)
        return component.instance if component else None

    def prepare(self, category, name, revision, parameters, inputs, execution_context):
        """Freeze one configured instance; work consumes exactly that instance."""
        if not revision or not isinstance(parameters, dict):
            raise ValueError('component revision and parameters required')
        component = self.get_component(category, name, revision)
        if component is None:
            raise ComponentChanged('frozen component is unavailable')
        instance = component.cls()
        for key, value in parameters.items():
            if not isinstance(key, str) or not isinstance(value, str):
                raise ValueError('configure parameters must be strings')
            instance.configure(key, value)
        result = instance.resolve_invocation(copy.deepcopy(inputs), copy.deepcopy(execution_context))
        if not isinstance(result, dict) or not isinstance(result.get('effective_parameters'), dict):
            raise ValueError('invalid invocation adapter response')
        import json
        result = json.loads(json.dumps(result, allow_nan=False))
        if len(json.dumps(result).encode()) > 12 * 1024 * 1024:
            raise ValueError('invocation response exceeds 12 MiB')
        with self._lock:
            now = time.monotonic()
            self._invocations = {k: v for k, v in self._invocations.items() if v[3] > now}
            if len(self._invocations) >= 256:
                raise ValueError('too many prepared invocations')
            token = uuid.uuid4().hex
            self._invocations[token] = (f'{category}.{name}', revision, instance, now + 300)
        return dict(result, invocation_token=token, component_revision=revision)

    def take_invocation(self, category, name, revision, token):
        with self._lock:
            prepared = self._invocations.pop(token, None)
        if (prepared is None or prepared[0] != f'{category}.{name}' or
                prepared[1] != revision or prepared[3] <= time.monotonic()):
            raise ComponentChanged('prepared invocation is unavailable or changed')
        return prepared[2]

    def release_invocation(self, token):
        with self._lock:
            self._invocations.pop(token, None)

    def list_operators(self):
        with self._lock:
            return [entry.metadata() for entry in self._components.values()]

    def configure(self, category, name, key, value):
        op = self.get(category, name)
        if op:
            op.configure(key, value)
            return True
        return False
