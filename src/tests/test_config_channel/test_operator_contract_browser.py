# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Chromium against the built UI and isolated actual HTTP/Worker control chain."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from test_data_semantics_api import ROOT
from test_operator_contract_api import running_contract_host, bind_input


def main():
    evidence = ROOT / 'evidence/operator-data-contract/t5-ui/browser'
    evidence.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='flowsql-contract-browser-') as temporary:
        directory = Path(temporary)
        contract = json.loads((ROOT / 'config/semantics/examples/operator-contract-passthrough.json').read_text())
        contract['operator'] = dict(category='browser', name='uploaded', version='1')
        source = ('from flowsql import OperatorBase, register_operator\n'
                  '@register_operator(category="browser", name="uploaded", data_contract=' + repr(contract) + ')\n'
                  'class Uploaded(OperatorBase):\n    def work(self, df):\n        return df\n')
        upload = directory / 'browser_uploaded.py'
        upload.write_text(source)
        with running_contract_host(Path(sys.argv[1]).resolve(), directory) as (base, _, log):
            bind_input(base)
            result = subprocess.run([sys.argv[2], str(Path(__file__).with_name('operator_contract_browser.mjs')),
                                     base, str(evidence), str(upload)], capture_output=True, text=True, timeout=160)
            (evidence / 'browser.log').write_text(result.stdout + result.stderr)
            (evidence / 'host.log').write_text(log.read_text())
            print(result.stdout + result.stderr)
            assert result.returncode == 0


if __name__ == '__main__':
    main()
