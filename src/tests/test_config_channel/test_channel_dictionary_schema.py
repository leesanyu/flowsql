# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
import copy,json,sys
from pathlib import Path
from jsonschema import Draft202012Validator
root=Path(sys.argv[1])
schema=json.loads((root/'config/semantics/documents.schema.json').read_text())
api=json.loads((root/'config/semantics/api.schema.json').read_text())
Draft202012Validator.check_schema(schema);Draft202012Validator.check_schema(api)
for name in ['standard','dictionary','binding','channel-binding']:
    Draft202012Validator(schema).validate(json.loads((root/f'examples/semantics/{name}.json').read_text()))
binding=json.loads((root/'examples/semantics/channel-binding.json').read_text())
for mutate in [lambda b:b.update(schema_id='flowsql.data-binding.v1'),lambda b:b.pop('confirmation'),lambda b:b['source_snapshot'].update(schema_state='made_up'),lambda b:b.update(unexpected=1)]:
    bad=copy.deepcopy(binding);mutate(bad);assert list(Draft202012Validator(schema).iter_errors(bad))
openapi=json.loads((root/'config/semantics/openapi.json').read_text())
ids=[p['post']['operationId'] for p in openapi['paths'].values() if 'post' in p]
assert len(ids)==len(set(ids))
errors=Draft202012Validator(dict(api,**{'$ref':'#/$defs/error'}))
for code in ['INVALID_REQUEST','INCOMPLETE_STRUCTURE','UNSUPPORTED_SOURCE','CONFIRMATION_REQUIRED','SOURCE_CHANGED','SOURCE_TIMEOUT','SOURCE_UNAVAILABLE']:
    errors.validate({'error':{'code':code,'message':'source failure','field_path':'','details':{}}})
assert list(errors.iter_errors({'error':{'code':'UNDECLARED_ERROR','message':'','field_path':'','details':{}}}))
for op in ['describe','preview','check','apply']:
    assert openapi['servers'][0]['url'] + f'/channels/{op}' == f'/api/semantics/v1/channels/{op}'
    assert f'/channels/{op}' in openapi['paths']
    request={'locator':{'kind':'dataframe','name':'example'}}
    if op=='apply':request.update(name='example',dictionary_ref='dictionary.app.requests@1',field_map=binding['field_map'],expected_current_revision=0,source_basis={k:binding['source_snapshot'][k] for k in ['object_key','source_revision','schema_fingerprint']},confirmations=[])
    validator=Draft202012Validator(dict(api,**{'$ref':f'#/$defs/channels_{op}Request'}))
    validator.validate(request)
    request['timeout_ms']=0;assert list(validator.iter_errors(request))
print('PASS: published channel models, v1/v2 samples, negative cases and four OpenAPI operations')
