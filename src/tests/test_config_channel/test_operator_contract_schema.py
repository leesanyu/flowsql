# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
import copy
import json
import pathlib
import sys
import jsonschema

root = pathlib.Path(sys.argv[1])
base = root / 'config/semantics'
schema = json.loads((base / 'documents.schema.json').read_text())
jsonschema.Draft202012Validator.check_schema(schema)
validator = jsonschema.Draft202012Validator(schema)
examples = sorted((base / 'examples').glob('operator-contract-*.json'))
assert len(examples) >= 2
for path in examples:
    value = json.loads(path.read_text())
    validator.validate(value)
    invalid = copy.deepcopy(value)
    invalid['operator']['version'] = ''
    assert not validator.is_valid(invalid)
    invalid = copy.deepcopy(value)
    invalid['inputs'][0]['max_count'] = -1
    assert not validator.is_valid(invalid)
    invalid = copy.deepcopy(value)
    invalid['outputs'][0]['mode'] = 'execute_user_code'
    assert not validator.is_valid(invalid)
baseliner = json.loads((base / 'examples/operator-contract-baseliner.json').read_text())
invocation = baseliner['configuration']['invocation_schema']
v = jsonschema.Draft202012Validator(invocation)
assert v.is_valid({'config': 'config.baseline@1'})
assert v.is_valid({'parameters': '{}'})
assert not v.is_valid({'config': 'config.baseline@1', 'parameters': '{}'})
assert not v.is_valid({'config': 'config.baseline@latest'})
assert len(baseliner['outputs']) == 4
api = json.loads((base / 'api.schema.json').read_text())
jsonschema.Draft202012Validator.check_schema(api)
for operation in ('save', 'import'):
    assert 'operator_use' not in api['$defs'][operation+'Request']['properties']['kind']['enum']
for kind in ('operator-contract', 'operator-use'):
    jsonschema.Draft202012Validator.check_schema(json.loads((base / (kind + '.schema.json')).read_text()))
print('PASS: published contract schemas, representative templates, configuration branches, read-only use records')

fixtures = json.loads((root / 'src/tests/test_config_channel/fixtures/operator-baseliner-configs.json').read_text())
configuration_validator = jsonschema.Draft202012Validator(dict(baseliner, **{'$ref': '#/resources/inline_config'}))
for fixture in fixtures.values():
    configuration_validator.validate(fixture['config'])
invalid = copy.deepcopy(fixtures['value']['config'])
invalid['datasets'][0]['metrics'][0]['feature_type'] = 'value_sampled'
assert not configuration_validator.is_valid(invalid)
invalid = copy.deepcopy(fixtures['multi']['config'])
del invalid['datasets'][1]['metrics'][0]['denominator']
assert not configuration_validator.is_valid(invalid)
invalid = copy.deepcopy(fixtures['relation']['config'])
invalid['datasets'][0]['metrics'][0]['column'] = 'value'
assert not configuration_validator.is_valid(invalid)
print('PASS: public Baseliner configuration resources and representative branch fixtures')
