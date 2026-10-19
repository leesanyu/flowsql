# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Exercise CMake's package-install contract without changing the host system."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class SystemDependencyTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="flowsql-packages-")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.state = self.directory / "installed"
        self.calls = self.directory / "calls"
        self.tools = {}
        scripts = {
            "dpkg": """import os, pathlib, sys
p = sys.argv[-1]
if p == 'libdpdk-dev' and not pathlib.Path(os.environ['PACKAGE_STATE']).exists():
    sys.exit(1)
print('install ok installed', end='')
""",
            "apt": """import os, pathlib, sys
with open(os.environ['PACKAGE_CALLS'], 'a') as f:
    f.write(' '.join(sys.argv[1:]) + '\\n')
if os.environ.get('APT_FAIL'):
    sys.exit(100)
if 'install' in sys.argv and not os.environ.get('APT_NO_EFFECT'):
    pathlib.Path(os.environ['PACKAGE_STATE']).touch()
""",
            "id": "import os; print(os.environ.get('PACKAGE_UID', '0'))\n",
            "sudo": "import sys; sys.exit(1)\n",
        }
        for name, script in scripts.items():
            path = self.directory / name
            path.write_text("#!/usr/bin/python3\n" + script)
            path.chmod(0o755)
            self.tools[name] = path

    def configure(self, *, install=True, mode="ON", **environment):
        script = self.directory / "check.cmake"
        script.write_text(f"""
cmake_minimum_required(VERSION 3.28)
set(FLOWSQL_INSTALL_SYSTEM_DEPS {'ON' if install else 'OFF'})
set(FLOWSQL_FLOW_LABELING_MODE {mode})
set(FLOWSQL_DPKG_QUERY_EXECUTABLE "{self.tools['dpkg']}")
set(FLOWSQL_APT_EXECUTABLE "{self.tools['apt']}")
set(FLOWSQL_ID_EXECUTABLE "{self.tools['id']}")
set(FLOWSQL_SUDO_EXECUTABLE "{self.tools['sudo']}")
include("{ROOT / 'src/cmake/system_dependencies.cmake'}")
flowsql_prepare_system_dependencies()
""")
        env = dict(os.environ, PACKAGE_STATE=str(self.state), PACKAGE_CALLS=str(self.calls))
        env.update(environment)
        return subprocess.run(["cmake", "-P", str(script)], env=env, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15)

    def test_installs_missing_dpdk_and_is_idempotent(self):
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout)
        calls = self.calls.read_text()
        self.assertIn("update", calls)
        self.assertIn("install -y --no-install-recommends libdpdk-dev", calls)
        self.assertEqual(self.configure().returncode, 0)
        self.assertEqual(self.calls.read_text(), calls)

    def test_check_only_rejects_missing_package_without_installing(self):
        result = self.configure(install=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Missing system build packages: libdpdk-dev", result.stdout)
        self.assertFalse(self.calls.exists())

    def test_auto_check_only_keeps_dpdk_optional(self):
        result = self.configure(install=False, mode="AUTO")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertFalse(self.calls.exists())

    def test_off_does_not_require_dpdk(self):
        result = self.configure(install=False, mode="OFF")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertFalse(self.calls.exists())

    def test_failed_apt_update_stops_before_install(self):
        result = self.configure(APT_FAIL="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("apt update failed", result.stdout)
        self.assertNotIn("install -y", self.calls.read_text())

    def test_success_exit_requires_actual_package_presence(self):
        result = self.configure(APT_NO_EFFECT="1")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("still missing after apt install", result.stdout)

    def test_missing_sudo_permission_fails_without_apt(self):
        result = self.configure(PACKAGE_UID="1000")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires root or non-interactive sudo", result.stdout)
        self.assertFalse(self.calls.exists())


if __name__ == "__main__":
    unittest.main()
