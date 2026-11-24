# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Actual workbench, readonly DESC and position-bound result reading in Chromium."""
from pathlib import Path
import json
import subprocess
import sys
import tempfile
from test_data_semantics_api import ROOT
from test_operator_contract_api import running_contract_host, call, bind_input


def main():
    evidence = ROOT / 'evidence/data-semantics-consumption/workbench-browser'
    evidence.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='flowsql-consumption-browser-') as tmp:
        with running_contract_host(Path(sys.argv[1]).resolve(), Path(tmp)) as (base, _, log):
            bind_input(base)
            locator = dict(kind='dataframe', name='input')
            before = call(base, 'describe', dict(target=dict(kind='channel', locator=locator)))
            document = before['description']['document']
            declared = lambda value: dict(state='declared', value=value, origin='manual')
            field = document['fields'][0]
            field['definition'] = declared('<img src=x onerror="window.__unsafe=true"> captured latency')
            field['value_domain'] = declared(dict(kind='range', min=0, max=18446744073709551615, min_inclusive=True, max_inclusive=True))
            field['null_meaning'] = declared('not measured')
            field['validity'] = declared(dict(mode='descriptive', description='only valid samples', field_ids=[]))
            document['entity']['limitations'] = declared(['cumulative revisions must not be summed'])
            saved = call(base, 'save', dict(kind='dictionary', name='http.input', expected_current_revision=1, document=document))
            preview = call(base, 'channels/describe', dict(locator=locator))
            call(base, 'channels/apply', dict(locator=locator, name='http.input', expected_current_revision=1,
                dictionary_ref=saved['exact_reference'], field_map=before['context']['binding']['field_map'],
                source_basis={key: preview['source_snapshot'][key] for key in ('object_key','source_revision','schema_fingerprint')},
                confirmations=before['checks']['required_confirmations']))
            result = subprocess.run([sys.argv[2], str(Path(__file__).with_name('consumption_browser.mjs')), base, str(evidence)],
                                    capture_output=True, text=True, timeout=170)
            (evidence / 'browser.log').write_text(result.stdout + result.stderr)
            (evidence / 'host.log').write_text(log.read_text())
            print(result.stdout + result.stderr)
            assert result.returncode == 0
    print('PASS workbench real Chromium consumption')


if __name__ == '__main__':
    main()
