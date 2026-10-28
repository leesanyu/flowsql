# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Built Vue and real Chromium at the three actual channel entry points."""
import subprocess
import sys
import tempfile
from pathlib import Path
from test_channel_dictionary_api import running
from test_data_semantics_api import ROOT


def main():
    evidence = ROOT / 'evidence/channel-data-dictionary/browser'
    evidence.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='flowsql-channel-browser-') as temporary:
        with running(Path(sys.argv[1]).resolve(), Path(temporary)) as (base, _, log, _):
            result = subprocess.run([sys.argv[2], str(Path(__file__).with_name('channel_dictionary_browser.mjs')),
                                     base, str(evidence)], capture_output=True, text=True, timeout=170)
            (evidence / 'browser.log').write_text(result.stdout + result.stderr)
            (evidence / 'host.log').write_text(log.read_text())
            print(result.stdout + result.stderr)
            assert result.returncode == 0


if __name__ == '__main__':
    main()
