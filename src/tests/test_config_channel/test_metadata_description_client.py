# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Ordinary Python client plus actual Chromium cross-page SQL/object workflow."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from test_data_semantics_api import ROOT, request
from test_operator_contract_api import running_contract_host, bind_input
from test_metadata_description_api import describe, call


def main():
    evidence = ROOT / "evidence/channel-description-refinement/t5-workflow"
    evidence.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="flowsql-description-client-") as temporary:
        with running_contract_host(Path(sys.argv[1]).resolve(), Path(temporary)) as (base, _, log):
            client = ROOT / "examples/metadata_description/client.py"
            command = [sys.executable, str(client), "--web-url", base]
            read = subprocess.run(command, capture_output=True, text=True, timeout=40)
            assert read.returncode == 0, read.stderr
            value = json.loads(read.stdout)
            assert not value["executed"] and value["source"]["structure"]["fields"]
            assert describe(base, dict(kind="dataframe", name="lazy"))["structure"]["state"] == "pending"
            bind_input(base)
            target_locator = dict(kind="database_table", type="sqlite", name="example", table="one")
            target = describe(base, target_locator)
            call(base, 'channels/save-description', dict(locator=target_locator, basis=target['basis'],
                 editor_changes=dict(description='Reference entity'), confirmations=target['checks'].get('required_confirmations', [])))
            target = describe(base, target_locator)
            target_ref = target['basis']['dictionary_ref']
            target_id = target['description']['document']['fields'][0]['field_id']
            source_locator = dict(kind='dataframe', name='input')
            source = describe(base, source_locator)
            source_name = source['structure']['fields'][0]['name']
            call(base, 'channels/save-description', dict(locator=source_locator, basis=source['basis'],
                 editor_changes=dict(fields={source_name: dict(time=dict(mode='duration', unit='ms'))},
                                     relations=[dict(dictionary_ref=target_ref, field_map={source_name: target_id})]),
                 confirmations=source['checks'].get('required_confirmations', [])))
            executed = subprocess.run(command + ["--execute", "--output", "description_copy"],
                                      capture_output=True, text=True, timeout=45)
            assert executed.returncode == 0, executed.stderr
            result = json.loads(executed.stdout)
            assert result["executed"] and result["result"]["rows"] == 3
            assert result["preflight"]["check"]["state"] == "compatible"
            assert result["uses"] and result["dictionaries"]
            assert result['source']['description']['document']['fields'][0]['time_semantics']['value']['kind'] == 'duration'
            assert result['source']['description']['document']['entity']['relations']['value'][0]['dictionary_ref'] == target_ref
            output_refs = {output['dictionary_ref'] for use in result['uses'] for output in use['document']['outputs'] if output.get('dictionary_ref')}
            produced = [item['document'] for item in result['dictionaries'] if item['exact_reference'] in output_refs]
            assert produced
            for document in produced:
                assert document['fields'][0]['time_semantics']['value']['kind'] == 'duration', document
                assert document['entity']['relations']['value'][0]['dictionary_ref'] == target_ref, document
                relation = document['entity']['relations']['value'][0]
                assert set(relation['field_map']).issubset({f['field_id'] for f in document['fields']})
                assert list(relation['field_map'].values()) == [target_id]
            assert describe(base, target_locator)['basis']['dictionary_ref'] == target_ref

            for use in result["uses"]:
                assert use["document"]["contract_ref"] == result["operator"]["basis"]["contract_ref"]
            assert any(d["document"]["fields"][0]["unit"]["value"]["code"] == "ms"
                       for d in result["dictionaries"] if d["exact_reference"].startswith("dictionary."))
            rejected = subprocess.run(command + ["--execute", "--output", "description_copy"],
                                      capture_output=True, text=True, timeout=40)
            assert rejected.returncode != 0 and "new output name" in rejected.stderr
            preview = request(base + "/api/channels/dataframe/preview", dict(name="description_copy", offset=0, limit=20))[1]
            assert "1000" in json.dumps(preview) and "3000" in json.dumps(preview)
            (evidence / "client.json").write_text(json.dumps(result, ensure_ascii=False, indent=2))
            browser = subprocess.run([sys.argv[2], str(Path(__file__).with_name("metadata_workflow_browser.mjs")),
                                      base, str(evidence)], capture_output=True, text=True, timeout=170)
            (evidence / "browser.log").write_text(browser.stdout + browser.stderr)
            (evidence / "host.log").write_text(log.read_text())
            print(browser.stdout + browser.stderr)
            assert browser.returncode == 0
    print("PASS: ordinary client directory/describe/check/SQL/use/dictionary closure and actual browser route/result compatibility")


if __name__ == "__main__":
    main()
