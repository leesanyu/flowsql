# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Check real build-context filtering with inert files, without Docker Engine.

Local dependency: Docker SDK 7.1.0; see docs/testing.md. No broker, database,
credentials or shared resources are used. Every fixture owns a temporary root.
"""
import tarfile
import tempfile
import unittest
from pathlib import Path

from docker.utils.build import tar


ROOT = Path(__file__).resolve().parents[3]


class DockerBuildContextTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="flowsql-build-context-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.patterns = [line.strip() for line in (ROOT / ".dockerignore").read_text().splitlines()
                         if line.strip() and not line.lstrip().startswith("#")]

    def package(self, paths):
        for relative in paths:
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("inert test placeholder\n")
        (self.root / ".dockerignore").write_text((ROOT / ".dockerignore").read_text())
        with tar(str(self.root), exclude=self.patterns) as archive:
            with tarfile.open(fileobj=archive) as context:
                return set(context.getnames())

    def test_private_kafka_configuration_is_excluded(self):
        examples = {
            "config/kafka/provider.example.json",
            "config/kafka/provider.docker.example.json",
            "config/kafka/deploy-scheduler.example.yaml",
        }
        private = {
            "config/kafka/provider.json",
            "config/kafka/kafka-master.key",
            "config/kafka/private/password.txt",
            "config/docker/local-provider.json",
            "secrets/kafka-master.key",
            "evidence/private.json",
        }
        members = self.package(examples | private)
        self.assertTrue(examples <= members, f"missing examples: {examples - members}")
        self.assertFalse(private & members, f"private files in context: {private & members}")

    def test_release_assets_and_versioned_runtime_are_included(self):
        assets = {
            "Dockerfile",
            "build/output/flowsql",
            "build/output/libflowsql_capture_pfring.so",
            "build/output/libpfring.so.8.8.0",
            "build/output/static/index.html",
            "build/output/static/assets/app.js",
            "build/output/share/kafka/dependencies.json",
            "config/docker/flowsql.yml",
            "src/python/requirements.txt",
            "src/python/flowsql/worker.py",
            "src/plugins/npi/conf/protocols.yml",
            ".thirdparts_installed/yaml-cpp/lib/libyaml-cpp.so.0.9.0",
        }
        members = self.package(assets)
        self.assertTrue(assets <= members, f"missing image inputs: {assets - members}")


if __name__ == "__main__":
    unittest.main()
