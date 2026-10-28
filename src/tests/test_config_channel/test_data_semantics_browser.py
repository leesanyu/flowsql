# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Actual built Vue UI with Chromium against isolated real control chain."""
import subprocess
import sys
import tempfile
from pathlib import Path
from test_data_semantics_api import running_host, ROOT


def main():
    evidence = ROOT / 'evidence/data-semantics-registry/browser'
    evidence.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='flowsql-semantics-browser-') as temporary:
        with running_host(Path(sys.argv[1]).resolve(), Path(temporary)) as (base, _, log):
            result = subprocess.run([sys.argv[2], str(Path(__file__).with_name('semantics_browser.mjs')), base,
                                     str(evidence)], capture_output=True, text=True, timeout=150)
            (evidence / 'browser.log').write_text(result.stdout + result.stderr)
            (evidence / 'host.log').write_text(log.read_text())
            print(result.stdout + result.stderr)
            assert result.returncode == 0


if __name__ == '__main__':
    main()
