# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""The real sink entry must fail before network access when configuration is absent."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory() as directory:
    result = subprocess.run([sys.executable, str(Path(__file__).with_name('real_sink.py')),
                             '--location', 'native', '--config', directory + '/missing.json'],
                            capture_output=True, text=True, timeout=10)
    assert result.returncode != 0
    report = json.loads(result.stdout)
    assert report['category'] == 'config_missing' and report['stage'] == 'load'
print('PASS: missing shared sink configuration fails without a skip')
