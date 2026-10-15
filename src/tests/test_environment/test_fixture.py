# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools"))
from test_environment.config import Config, DEFAULT_CONFIG, EnvironmentError
from test_environment.fixture import compose_document, fingerprint, initialize_credentials, private_write, runtime_command


class FixtureTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        data = json.loads(DEFAULT_CONFIG.read_text())
        for db in data["databases"].values():
            db["password_ref"] = "env:TEST_ENVIRONMENT_SENTINEL"
        data["kafka"]["password_ref"] = "env:TEST_ENVIRONMENT_SENTINEL"
        self.path = self.root / "config.json"
        self.path.write_text(json.dumps(data))
        self.config = Config(self.path)
        patcher = patch.dict(os.environ, TEST_ENVIRONMENT_SENTINEL="private-test-password")
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_compose_is_owned_and_addresses_are_derived(self):
        with patch("test_environment.fixture.STATE", self.root):
            document = compose_document(self.config)
        self.assertNotIn("container_name", json.dumps(document))
        self.assertEqual(document["name"], "flowsql-test")
        self.assertEqual(document["services"]["mysql"]["ports"], ["127.0.0.1:23306:3306"])
        self.assertEqual(document["services"]["mysql"]["environment"]["MYSQL_PASSWORD"], "private-test-password")
        self.assertEqual(document["networks"]["test"]["labels"]["flowsql.test.project"], "flowsql_test")

    def test_default_docker_preparation_does_not_bind_host_ports(self):
        with patch("test_environment.fixture.STATE", self.root):
            document=compose_document(self.config,publish=False)
        self.assertTrue(all("ports" not in service for service in document["services"].values()))

    def test_private_files_and_missing_external_reference(self):
        private_write(self.root / "secret", "test-secret")
        self.assertEqual((self.root / "secret").stat().st_mode & 0o777, 0o600)
        self.config.data["databases"]["mysql"]["password_ref"] = "file:missing"
        with self.assertRaises(EnvironmentError):
            initialize_credentials(self.config)
        self.assertFalse((self.root / "missing").exists())

    def test_first_nested_write_and_reuse_keep_state_private(self):
        for existing in (False, True):
            state = self.root / str(existing) / "state"
            if existing:
                state.mkdir(parents=True)
                state.chmod(0o755)
            with patch("test_environment.fixture.STATE", state):
                private_write(state / "runs" / "first" / "config.json", "private configuration")
            self.assertEqual(state.stat().st_mode & 0o777, 0o700)
            self.assertEqual((state / "runs/first/config.json").stat().st_mode & 0o777, 0o600)

    def test_public_fingerprint_does_not_depend_on_environment_secret(self):
        before = fingerprint(self.config)
        with patch.dict(os.environ, TEST_ENVIRONMENT_SENTINEL="rotated-secret"):
            self.assertEqual(fingerprint(self.config), before)

    def test_container_client_uses_service_network_and_no_secret_argv(self):
        args = runtime_command(self.config, ["test"], location="docker", env={"FLOWSQL_TEST_CONFIG": str(self.path)})
        self.assertIn("flowsql-test-network", args)
        self.assertNotIn("host", args)
        self.assertNotIn("private-test-password", " ".join(args))


if __name__ == "__main__":
    unittest.main()
