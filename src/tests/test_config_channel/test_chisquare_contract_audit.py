# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Audit the representative test component through production Python/Arrow paths."""
import asyncio
import json
from pathlib import Path
import shutil
import sys
import tempfile

import jsonschema
import pandas as pd
import polars as pl
from starlette.requests import Request

ROOT = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(ROOT / 'src/python'))
from flowsql import worker
from flowsql.arrow_codec import decode_arrow_ipc_file, encode_arrow_ipc_file
from flowsql.operator_registry import OperatorRegistry


with tempfile.TemporaryDirectory(prefix='flowsql_chisquare_audit_') as temporary:
    directory = Path(temporary)
    shutil.copyfile(ROOT / 'src/tests/test_config_channel/fixtures/description_chisquare.py',
                    directory / 'chisquare.py')
    worker.registry = OperatorRegistry()
    worker.registry.discover(directory)
    metadata, = worker.registry.list_operators()
    assert (metadata['category'], metadata['name']) == ('explore', 'chisquare')
    contract = metadata['data_contract']
    jsonschema.Draft202012Validator(json.loads(
        (ROOT / 'config/semantics/documents.schema.json').read_text())).validate(contract)
    assert contract['operator'] == dict(category='explore', name='chisquare', version='1')
    assert contract['inputs'][0]['min_count'] == contract['inputs'][0]['max_count'] == 1
    for name in ('col1', 'col2'):
        role = next(role for role in contract['inputs'][0]['roles'] if role['role_id'] == name)
        assert role['selector'] == dict(kind='parameter', key=name)
        assert role['types'] == [] and role['nullable'] is None and role['required'] is False
        assert contract['configuration']['invocation_schema']['properties'][name]['default'] == ''

    async def execute(frame, parameters):
        # A fresh production instance preserves omitted-vs-explicit-empty semantics.
        worker.registry.reload(directory)
        for key, value in parameters.items():
            assert isinstance(value, str)
            assert worker.registry.configure('explore', 'chisquare', key, value)
        input_path = directory / 'sample_in.arrow'
        encode_arrow_ipc_file(frame, str(input_path))
        assert isinstance(decode_arrow_ipc_file(str(input_path)), pl.DataFrame)
        body = json.dumps({'input': str(input_path)}).encode()

        async def receive():
            return {'type': 'http.request', 'body': body, 'more_body': False}

        request = Request({'type': 'http', 'method': 'POST', 'path': '/'}, receive=receive)
        response = await worker.work('explore', 'chisquare', request)
        assert response.status_code == 200
        return decode_arrow_ipc_file(json.loads(response.body)['output']).to_pandas()

    # Independent balanced 2x2 table: each observed and expected cell is 2.
    frame = pl.DataFrame({'region': ['a'] * 4 + ['b'] * 4,
                          'result': ['x', 'x', 'y', 'y'] * 2})
    explicit = asyncio.run(execute(frame, {'col1': 'region', 'col2': 'result'}))
    assert len(explicit) == 5
    summary = explicit.iloc[0]
    assert (summary['col1'], summary['col2']) == ('region', 'result')
    assert (summary['chi2'], summary['p_value'], summary['dof']) == (0, 1, 1)
    assert summary['significant_0.05'] == 'no'
    assert pd.isna(summary['observed']) and pd.isna(summary['expected'])
    assert pd.isna(summary['region']) and pd.isna(summary['result'])
    details = explicit.iloc[1:]
    assert set(zip(details['region'], details['result'])) == {('a', 'x'), ('a', 'y'), ('b', 'x'), ('b', 'y')}
    assert (details['observed'] == 2).all() and (details['expected'] == 2).all()
    for name in ('col1', 'col2', 'chi2', 'p_value', 'dof', 'significant_0.05'):
        assert details[name].isna().all()
    # Both omitted, empty, or only one supplied: original fallback selects both columns.
    for parameters in ({}, {'col1': '', 'col2': ''}, {'col1': 'missing'}):
        pd.testing.assert_frame_equal(asyncio.run(execute(frame, parameters)), explicit)
    for bad_frame, parameters in [(frame, {'col1': 'missing', 'col2': 'result'}),
                                  (pl.DataFrame({'one': [1, 2]}), {})]:
        error = asyncio.run(execute(bad_frame, parameters))
        assert list(error.columns) == ['error'] and len(error) == 1 and error.iloc[0]['error']
    fields = contract['resources']['results']['properties']
    assert set(fields) == {'col1', 'col2', 'chi2', 'p_value', 'dof', 'observed',
                           'expected', 'significant_0.05', 'error'}
    assert contract['resources']['results']['additionalProperties'] is True
    assert fields['col1']['type'] == fields['col2']['type'] == ['string', 'null']
    assert 'NULL' in contract['resources']['results']['description']

print('PASS: chisquare test component identity, actual Worker Polars/Arrow execution, string/default '
      'parameters, summary/detail dynamic columns and NULLs, and error rows; no production coverage claim')
