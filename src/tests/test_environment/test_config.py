# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools"))
from test_environment.config import Config, DEFAULT_CONFIG, EnvironmentError, dependencies

BINARY = sys.argv.pop() if len(sys.argv) > 1 and not sys.argv[-1].startswith("-") else None


class ConfigurationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.path = self.root / "environment.json"
        self.data = json.loads(DEFAULT_CONFIG.read_text())
        for db in self.data["databases"].values():
            db["password_ref"] = "file:secret"
        self.data["kafka"]["password_ref"] = "file:secret"
        (self.root / "secret").write_text("private-sentinel")
        (self.root / "secret").chmod(0o600)
        self.save()

    def save(self):
        self.path.write_text(json.dumps(self.data))

    def test_runtime_location_and_role(self):
        config = Config(self.path)
        self.assertEqual(config.database("mysql", "npm", "native")["port"], 23306)
        self.assertEqual(config.database("mysql", "source", "docker")["host"], "mysql")
        self.data["databases"]["mysql"]["endpoints"]["native"]["port"] = 23307
        self.save()
        self.assertEqual(Config(self.path).database("mysql", "npm", "native")["port"], 23307)

    def test_rejects_unknown_missing_duplicate_and_wrong_types(self):
        for mutate in [lambda d: d.update(unknown=True), lambda d: d.pop("kafka"),
                       lambda d: d.update(schema_version=True),
                       lambda d: d["databases"]["mysql"]["endpoints"]["native"].update(port="3306")]:
            self.data = json.loads(DEFAULT_CONFIG.read_text())
            mutate(self.data)
            self.save()
            with self.assertRaises(EnvironmentError):
                Config(self.path)
        self.path.write_text('{"schema_version":1,"schema_version":1}')
        with self.assertRaisesRegex(EnvironmentError, "duplicate JSON field"):
            Config(self.path)

    def test_missing_and_private_credentials_do_not_leak(self):
        config = Config(self.path)
        (self.root / "secret").chmod(0o644)
        with self.assertRaises(EnvironmentError) as error:
            config.database("mysql", "driver", "native")
        self.assertNotIn("private-sentinel", str(error.exception))
        (self.root / "secret").unlink()
        with self.assertRaisesRegex(EnvironmentError, "config_missing"):
            config.database("mysql", "driver", "native")

    def test_kafka_profiles_and_dependency_manifest(self):
        self.assertIn("GSSAPI", Config(self.path).data["kafka"]["profiles"])
        manifest = dependencies()
        self.assertEqual(manifest["test_kafka_real_docker"]["services"], ["kafka"])
        self.data["kafka"]["profiles"].remove("GSSAPI")
        self.save()
        with self.assertRaises(EnvironmentError):
            Config(self.path)

    @unittest.skipUnless(BINARY, "C++ helper is exercised by its CMake target")
    def test_cpp_helper_uses_same_runtime_config_and_private_reference(self):
        env = dict(os.environ, FLOWSQL_TEST_CONFIG=str(self.path))
        for location, expected in [("native", "127.0.0.1:23306"), ("docker", "mysql:3306")]:
            result = subprocess.run([BINARY], env=dict(env, FLOWSQL_TEST_LOCATION=location), capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), expected)
            self.assertNotIn("private-sentinel", result.stdout + result.stderr)
        (self.root / "secret").unlink()
        result = subprocess.run([BINARY], env=env, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("config_missing", result.stderr)


if __name__ == "__main__":
    unittest.main()
