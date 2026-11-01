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

# T1 freezes the wire contract independently of T2's future describe implementation.
assert 'operatorUsageResponse' in api['$defs'], 'operator usage response contract is not frozen'
usage_cases = json.loads((base / 'examples/operator-usage.json').read_text())['cases']
assert {case['name'] for case in usage_cases} == {'passthrough', 'chisquare', 'baseliner'}


def api_validator(name):
    return jsonschema.Draft202012Validator(dict(api, **{'$ref': '#/$defs/' + name}))


def pointer(document, path):
    value = document
    for token in path.lstrip('/').split('/'):
        key = token.replace('~1', '/').replace('~0', '~')
        value = value[int(key)] if isinstance(value, list) else value[key]
    return value


def declared_contract(case):
    path = root / case['contract_source']
    if path.suffix == '.json':
        return json.loads(path.read_text())
    # Read the real fixture's author declaration without importing or executing it.
    import ast
    tree = ast.parse(path.read_text())
    for node in ast.walk(tree):
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == 'register_operator':
            return ast.literal_eval(next(arg.value for arg in node.keywords if arg.arg == 'data_contract'))
    raise AssertionError('author declaration not found')


request_validator = api_validator('operatorDescribeRequest')
usage_validator = api_validator('operatorUsageResponse')
full_validator = api_validator('operatorFullDescriptionResponse')
for case in usage_cases:
    request_validator.validate(case['request'])
    usage_validator.validate(case['response'])
    contract = declared_contract(case)
    api_validator('operators_checkRequest').validate(case['check_request'])
    jsonschema.Draft202012Validator(
        contract['configuration']['invocation_schema']).validate(case['check_request']['parameters'])
    for decoding in contract['configuration'].get('decoded_parameters', []):
        encoded = case['check_request']['parameters'][decoding['key']]
        decoded_validator = jsonschema.Draft202012Validator(dict(contract, **{'$ref': decoding['schema_ref']}))
        decoded_validator.validate(json.loads(encoded))
        assert not decoded_validator.is_valid(encoded)  # A double-encoded JSON string is not the config object.
    data = case['response']['data']
    usage = data['usage']
    assert data['subject'] == case['request']['target']
    assert usage['identity']['name'] == data['subject']['name']
    assert usage['identity']['title'] == contract['title']
    assert usage['identity']['author_version'] == contract['operator']['version']
    assert set(usage) == {'identity', 'inputs', 'parameters', 'outputs', 'limitations', 'examples', 'annotations'}
    assert not {'contract', 'document', 'resources', 'normalized_config_schema'} & data.keys()
    assert usage['identity']['purposes'] == ([dict(text=contract['description'], origin='author', source_pointer='/description')] if 'description' in contract else [])  # Only explicit author text.
    assert usage['examples'] == [dict(text=text, origin='author', source_pointer='/examples/'+str(i)) for i,text in enumerate(contract.get('examples', []))]
    assert [item['text'] for item in usage['limitations']] == contract['limitations']
    for detail in data['details'] + usage['parameters']['constraints']:
        assert detail['exact_reference'] == data['basis']['contract_ref']
        pointer(contract, detail['resource_pointer'])
    for input_item in usage['inputs']:
        original = pointer(contract, input_item['source_pointer'])
        for key in ['port_id', 'shape', 'min_count', 'max_count', 'roles']:
            assert input_item[key] == original[key]
    invocation = contract['configuration']['invocation_schema']
    assert [item['name'] for item in usage['parameters']['items']] == list(invocation.get('properties', {}))
    for parameter in usage['parameters']['items']:
        assert parameter['schema'] == pointer(contract, parameter['source_pointer'])
        if parameter['wire_encoding'] == 'json':
            pointer(contract, parameter['decoded_schema_pointer'])
        for link in parameter['input_roles']:
            role = next(role for item in contract['inputs'] if item['port_id'] == link['port_id']
                        for role in item['roles'] if role['role_id'] == link['role_id'])
            assert role['selector'] == {'kind': 'parameter', 'key': parameter['name']}
        if parameter['required'] is not None:
            assert parameter['required'] == (parameter['name'] in invocation.get('required', []))
    assert [item['port_id'] for item in usage['outputs']] == [item['port_id'] for item in contract['outputs']]
    for output in usage['outputs']:
        original = pointer(contract, output['source_pointer'])
        for key in ['shape', 'mode', 'availability', 'row_cases', 'context_semantics']:
            assert output.get(key) == original.get(key)
        for field in output['fields']:
            assert field['declaration'] == pointer(contract, field['source_pointer'])
    # Full remains a separate, explicit read shape; no full document is hidden in usage.
    full = {'data': {'subject': data['subject'], 'basis': data['basis'],
                    'structure': {'state': 'unsupported'},
                    'description': {'state': 'present', 'origin': 'component_author',
                                    'document': {'title': contract['title']}, 'notes': None},
                    'contract': {'state': 'declared', 'document': contract},
                    'checks': {'data_validation': 'not_run'}, 'gaps': [], 'context': {}}}
    full_validator.validate(full)
    invalid_full = copy.deepcopy(full)
    invalid_full['data']['contract']['document'] = {}
    assert not full_validator.is_valid(invalid_full)
    default_request = {'target': case['request']['target']}
    request_validator.validate(default_request)
    assert 'view' not in default_request  # Schema defaults do not mutate caller data.
    for view in ['usage', 'full']:
        request_validator.validate(dict(case['request'], view=view))
    assert not request_validator.is_valid(dict(case['request'], view='unknown'))
    assert not request_validator.is_valid(dict(case['request'], timeout_ms=0))
    channel = {'kind': 'channel', 'locator': {'kind': 'dataframe', 'name': 'x'}}
    assert not request_validator.is_valid({'target': channel})
    assert not request_validator.is_valid({'target': {'kind': 'channel', 'name': data['subject']['name']}})
    for mutate in [lambda d: d.update(resources={}),
                   lambda d: d['usage'].pop('outputs'),
                   lambda d: d.update(contract_state='unavailable'),
                   lambda d: d['basis'].update(contract_ref=None)]:
        invalid = copy.deepcopy(case['response'])
        mutate(invalid['data'])
        assert not usage_validator.is_valid(invalid)

simple = next(case for case in usage_cases if case['name'] == 'chisquare')
params = simple['response']['data']['usage']['parameters']['items']
assert [item['name'] for item in params] == ['col1', 'col2']
assert all(item['control'] == 'field' and item['schema']['default'] == '' for item in params)
assert all(isinstance(value, str) for value in simple['check_request']['parameters'].values())
assert simple['check_request']['parameters'] == {'col1': 'region', 'col2': 'result'}
assert [field['name'] for field in simple['response']['data']['usage']['outputs'][0]['fields']][:2] == ['col1', 'col2']
assert simple['response']['data']['usage']['outputs'][0]['fields_state'] == 'dynamic'
assert 'NULL' in simple['response']['data']['usage']['outputs'][0]['summary']
# Author defaults are hints. Validation leaves an omitted parameter omitted, and an explicit empty string empty.
simple_invocation = declared_contract(simple)['configuration']['invocation_schema']
for supplied in ({}, {'col1': '', 'col2': ''}, {'col1': 'region'}):
    before = copy.deepcopy(supplied)
    jsonschema.Draft202012Validator(simple_invocation).validate(supplied)
    assert supplied == before
    request = dict(simple['check_request'], parameters=supplied)
    api_validator('operators_checkRequest').validate(request)
for invalid_value in (None, True, 3, {'field': 'region'}):
    invalid_request = copy.deepcopy(simple['check_request'])
    invalid_request['parameters']['col1'] = invalid_value
    assert not api_validator('operators_checkRequest').is_valid(invalid_request)
    assert not jsonschema.Draft202012Validator(simple_invocation).is_valid(invalid_request['parameters'])
passthrough = next(case for case in usage_cases if case['name'] == 'passthrough')
copy_parameters = passthrough['response']['data']['usage']['parameters']
assert copy_parameters['additional_parameters'] == 'allowed'
assert [(item['name'], item['schema']['default'], item['wire_encoding'], item['control'])
        for item in copy_parameters['items']] == [('force_fail', 'false', 'string', 'text'),
                                                  ('delay_ms', '0', 'string', 'text')]
copy_invocation = jsonschema.Draft202012Validator(declared_contract(passthrough)['configuration']['invocation_schema'])
for supplied in ({}, {'force_fail': '1', 'delay_ms': '-1'},
                 {'force_fail': '', 'delay_ms': 'invalid', 'future_parameter': 'kept'}):
    before = copy.deepcopy(supplied)
    copy_invocation.validate(supplied)
    api_validator('operators_checkRequest').validate(dict(passthrough['check_request'], parameters=supplied))
    assert supplied == before
assert not copy_invocation.is_valid({'force_fail': True})
assert not copy_invocation.is_valid({'delay_ms': 1})
invalid = copy.deepcopy(simple['response'])
invalid['data']['usage']['inputs'][0]['roles'][0]['nullable'] = 'unknown'
assert not usage_validator.is_valid(invalid)
invalid = copy.deepcopy(simple['response'])
invalid['data']['usage']['parameters']['items'][0]['control'] = 'guessed-field'
assert not usage_validator.is_valid(invalid)
invalid = copy.deepcopy(simple['response'])
invalid['data']['usage']['parameters']['items'][0]['input_roles'] = []
assert not usage_validator.is_valid(invalid)
complex_case = next(case for case in usage_cases if case['name'] == 'baseliner')
complex_usage = complex_case['response']['data']['usage']
json_parameter = next(item for item in complex_usage['parameters']['items'] if item['name'] == 'parameters')
assert json_parameter['wire_encoding'] == 'json' and json_parameter['control'] == 'json'
assert json_parameter['required'] is None  # Conditional requirements are not flattened to false.
assert complex_usage['parameters']['constraints']
assert [item['name'] for item in complex_usage['parameters']['items'] if item['required'] is None] == ['config', 'parameters']
assert complex_usage['parameters']['constraints'][0]['resource_pointer'] == '/configuration/invocation_schema/oneOf'
complex_invocation = jsonschema.Draft202012Validator(declared_contract(complex_case)['configuration']['invocation_schema'])
assert not complex_invocation.is_valid({})
assert not complex_invocation.is_valid({'config': 'config.baseline@1', 'parameters': '{}'})
forecast = next(row for row in complex_usage['outputs'][0]['row_cases'] if row['case_id'] == 'forecast')
assert forecast['field_rules']['observed']['must_be_null']
assert forecast['field_rules']['can_alert']['must_be_null']
assert {name for name, rule in forecast['field_rules'].items() if rule.get('must_be_null')} == {
    'observed', 'confidence', 'maturity', 'score', 'is_outside_band', 'can_score', 'can_update',
    'update_weight', 'can_alert'}
invalid = copy.deepcopy(complex_case['response'])
next(item for item in invalid['data']['usage']['parameters']['items']
     if item['name'] == 'parameters').pop('decoded_schema_pointer')
assert not usage_validator.is_valid(invalid)
invalid = copy.deepcopy(complex_case['response'])
next(item for item in invalid['data']['usage']['parameters']['items']
     if item['name'] == 'parameters')['wire_encoding'] = 'string'
assert not usage_validator.is_valid(invalid)
for reference, path in [('operator_contract.explore.baseliner@latest', '/inputs'),
                        ('https://example.com/contract', '/inputs'),
                        ('operator_contract.explore.baseliner@1', '/bad~2')]:
    invalid = copy.deepcopy(complex_case['response'])
    invalid['data']['details'][0].update(exact_reference=reference, resource_pointer=path)
    assert not usage_validator.is_valid(invalid)
missing = copy.deepcopy(simple['response'])
missing['data'].update(contract_state='missing', details=[], required_context_refs=[],
                       gaps=[{'code': 'CONTRACT_MISSING', 'section': 'identity', 'message': '未声明契约'}])
missing['data']['basis']['contract_ref'] = None
missing['data']['usage'].update(inputs=[], outputs=[], limitations=[], examples=[])
missing['data']['usage']['parameters'].update(items=[], constraints=[], additional_parameters='unknown')
usage_validator.validate(missing)
print('PASS: operator usage/full contracts, author provenance, field mappings, conditional JSON parameters, '
      'same-revision details, mixed rows, unknown states and negative wire cases')

# Local descriptions are flat field lists; ownership is written in descriptions.
notes_validator = api_validator('descriptionNotes')
notes_validator.validate(json.loads((base / 'examples/operator-usage.json').read_text())['local_description_example'])
notes = dict(purpose='',usage_notes='',examples=[],inputs=[],parameters=[],outputs=[])
notes_validator.validate(notes)
for key in ['input_description', 'parameter_description', 'output_description']:
    for value in ['', '作者说明直接进入编辑框', 'x' * 30000]:
        notes_validator.validate(dict(notes, **{key: value}))
    for value in [None, 1, 'x' * 30001]:
        assert not notes_validator.is_valid(dict(notes, **{key: value}))
notes_validator.validate(dict(notes, purpose='x' * 30000, examples=['x' * 30000]))
row = dict(name='column',type='',required=None,default='',description='')
notes['parameters'] = [row]
notes_validator.validate(notes)
for parameter_type in ['', 'string', 'integer', 'number', 'boolean', 'json']:
    notes_validator.validate(dict(notes,parameters=[dict(row,type=parameter_type,default='""')]))
for requirement in ['', 'required_non_null', 'required_nullable', 'optional']:
    notes_validator.validate(dict(notes,inputs=[dict(name='value',type='double',requirement=requirement,description='Primary input: observed value')]))
for nullable in [None,True,False]:
    notes_validator.validate(dict(notes,outputs=[dict(name='value',type='double',nullable=nullable,description='Model output: only when enabled')]))
input_row=dict(name='value',type='',requirement='',description='Primary input')
output_row=dict(name='value',type='',nullable=None,description='Results output')
for section,field in [('inputs',input_row),('outputs',output_row),('parameters',row)]:
    notes_validator.validate(dict(notes,**{section:[field]*100}))
    assert not notes_validator.is_valid(dict(notes,**{section:[field]*101}))
    for key in field:
        incomplete=dict(field);del incomplete[key]
        assert not notes_validator.is_valid(dict(notes,**{section:[incomplete]}))
    assert not notes_validator.is_valid(dict(notes,**{section:[dict(field,fields=[])]}))
    assert not notes_validator.is_valid(dict(notes,**{section:[dict(field,port='removed')]}))
notes_validator.validate(dict(notes,inputs=[input_row,dict(input_row,description='Reference input')],outputs=[output_row,dict(output_row,description='Model output')]))
for invalid in [dict(notes,inputs='legacy text'),dict(notes,parameters=[dict(row,required='yes')]),
                dict(notes,parameters=[dict(row,default=None)]),dict(notes,parameters=[dict(row,default=0)]),
                dict(notes,parameters=[dict(row,encoding='json')]),dict(notes,parameters=[dict(row,type='object')]),
                dict(notes,inputs=[dict(name='primary',description='',fields=[input_row])]),
                dict(notes,outputs=[dict(name='results',description='',fields=[output_row])]),
                dict(notes,outputs=[{}]),dict(notes,input_requirements='removed')]:
    assert not notes_validator.is_valid(invalid), invalid
print('PASS: flat local fields, textual ownership, three requirements, JSON type, default text and rejected grouped/malformed descriptions')

# Optional short descriptions are a shared author contract, not operator-name UI branches.
for definition in ['operatorInput', 'operatorOutput', 'operatorConfiguration', 'operatorContract']:
    assert schema['$defs'][definition] == api['$defs'][definition]
for location in [(), ('configuration',), ('inputs', 0), ('outputs', 0)]:
    for bad in [False, '', 'x' * 30001]:
        invalid = copy.deepcopy(baseliner)
        target = invalid
        for key in location: target = target[key]
        target['description'] = bad
        assert not validator.is_valid(invalid)
for case in usage_cases:
    contract = declared_contract(case)
    usage = case['response']['data']['usage']
    for section in ['inputs', 'outputs']:
        for original, projected in zip(contract[section], usage[section]):
            assert projected.get('description') == original.get('description')
    assert usage['parameters'].get('description') == contract['configuration'].get('description')
print('PASS: shared compact author descriptions, length/type bounds and exact usage origins')
