"""Runtime-selected installed package; no virtualenv path is captured by CMake."""

import os
import subprocess
import sys
from pathlib import Path

python = os.environ.get("FLOWSQL_TEST_MCP_PYTHON", sys.executable)
environment = dict(os.environ, FLOWSQL_TEST_OUTPUT_DIR=str(Path(sys.argv[1]).resolve()))
environment.pop("PYTHONPATH", None)
root = Path(__file__).resolve().parents[3]
probe = subprocess.run(
    [python, "-c", "import mcp,pytest,flowsql_mcp; print(flowsql_mcp.__file__)"],
    check=False,
    env=environment,
    capture_output=True,
    text=True,
    cwd="/tmp",
)
if probe.returncode or str(root) in probe.stdout:
    sys.exit(
        "MCP test environment requires the installed package, MCP SDK and pytest; set FLOWSQL_TEST_MCP_PYTHON"
    )
sys.exit(
    subprocess.call(
        [python, "-m", "pytest", "-q", str(root / "src/mcp/tests")],
        env=environment,
        cwd="/tmp",
    )
)
