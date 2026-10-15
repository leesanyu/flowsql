# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Resource lifecycle and ownership regressions; no Docker daemon required."""
import copy
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools"))
from test_environment import cleanup
from test_environment.config import EnvironmentError


class CleanupTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        state = patch.object(cleanup, "STATE", Path(temporary.name))
        state.start()
        self.addCleanup(state.stop)
        self.project = "flowsql_lifecycle"
        self.labels = {"flowsql.test.project": self.project,
                       "com.docker.compose.project": "flowsql-lifecycle"}
        self.container = {"id": "container-id", "name": "/owned", "state": "exited",
                          "labels": self.labels, "mounts": [{"Type": "volume", "Name": "owned-volume"}]}
        self.volume = {"name": "owned-volume", "labels": self.labels}
        self.network = {"id": "network-id", "name": "owned-network", "labels": self.labels,
                        "containers": {"container-id": {}}}
        self.inventory = {"containers": [self.container], "volumes": [self.volume], "networks": [self.network]}

    def inspect(self, kind):
        return copy.deepcopy(self.inventory[kind])

    def test_preview_never_mutates(self):
        with patch.object(cleanup, "inspect_resources", side_effect=self.inspect), patch.object(cleanup, "command") as command:
            result = cleanup.cleanup_project(self.project)
        self.assertEqual(result["containers"], ["container-id"])
        self.assertEqual(result["volumes"], ["owned-volume"])
        self.assertFalse(result["applied"])
        command.assert_not_called()

    def test_apply_and_repeat_are_idempotent(self):
        def remove(args, **kwargs):
            self.inventory[{"rm": "containers", "volume": "volumes", "network": "networks"}[args[1]]] = []
            return ""
        with patch.object(cleanup, "inspect_resources", side_effect=self.inspect), patch.object(cleanup, "command", side_effect=remove) as command:
            self.assertTrue(cleanup.cleanup_project(self.project, apply=True)["applied"])
            self.assertEqual(command.call_count, 3)
            cleanup.cleanup_project(self.project, apply=True)
            self.assertEqual(command.call_count, 3)

    def test_other_projects_and_unlabelled_resources_are_preserved(self):
        self.inventory["containers"].append({"id": "user", "labels": {}, "mounts": []})
        self.inventory["volumes"].append({"name": "user-volume", "labels": {}})
        self.inventory["networks"].append({"id": "user-network", "name": "user-network", "labels": {}, "containers": {}})
        with patch.object(cleanup, "inspect_resources", side_effect=self.inspect):
            result = cleanup.cleanup_project(self.project)
        self.assertEqual(result["containers"], ["container-id"])
        self.assertEqual(result["volumes"], ["owned-volume"])
        self.assertEqual(result["networks"], ["network-id"])

    def test_shared_volume_and_network_refuse_all_mutations(self):
        for shared in ("volume", "network", "compose"):
            with self.subTest(shared=shared):
                original = copy.deepcopy(self.inventory)
                if shared == "volume":
                    self.inventory["containers"].append({"id": "user", "labels": {}, "mounts": self.container["mounts"]})
                elif shared == "network":
                    self.inventory["networks"][0]["containers"]["user"] = {}
                else:
                    self.inventory["volumes"][0]["labels"]["com.docker.compose.project"] = "other"
                with patch.object(cleanup, "inspect_resources", side_effect=self.inspect), patch.object(cleanup, "command") as command:
                    with self.assertRaises(EnvironmentError):
                        cleanup.cleanup_project(self.project, apply=True)
                    command.assert_not_called()
                self.inventory = original

    def test_delete_failure_and_remaining_resources_are_errors(self):
        for failure in (True, False):
            with self.subTest(failure=failure), patch.object(cleanup, "inspect_resources", side_effect=self.inspect), patch.object(cleanup, "command", side_effect=EnvironmentError("service_unavailable", "command") if failure else None):
                with self.assertRaises(EnvironmentError):
                    cleanup.cleanup_project(self.project, apply=True)

    def test_orphan_volume_does_not_require_compose_or_secrets(self):
        self.inventory["containers"] = []
        self.inventory["networks"] = []
        with patch.object(cleanup, "inspect_resources", side_effect=self.inspect):
            self.assertEqual(cleanup.cleanup_project(self.project)["volumes"], ["owned-volume"])

    def test_activity_lock_blocks_cleanup_but_allows_other_readers(self):
        with cleanup.project_activity(self.project), cleanup.project_activity(self.project):
            with self.assertRaises(EnvironmentError) as caught:
                cleanup.cleanup_project(self.project, apply=True)
        self.assertEqual(caught.exception.category, "resource_busy")
        with patch.object(cleanup, "inspect_resources", return_value=[]):
            cleanup.cleanup_project(self.project, apply=True)

    def test_temporary_scope_reclaims_on_success_test_and_preparation_failure(self):
        for stage in (None, "test", "preparation"):
            with self.subTest(stage=stage), patch.object(cleanup, "cleanup_project") as reclaim:
                try:
                    with cleanup.temporary_project(self.project):
                        if stage:
                            raise RuntimeError(stage)
                except RuntimeError as error:
                    self.assertEqual(str(error), stage)
                reclaim.assert_called_once_with(self.project, apply=True)

    def test_temporary_cleanup_failure_is_not_silenced(self):
        with patch.object(cleanup, "cleanup_project", side_effect=EnvironmentError("service_unavailable", "cleanup")):
            with self.assertRaises(EnvironmentError):
                with cleanup.temporary_project(self.project):
                    pass

    def test_inspection_requests_only_public_fields(self):
        for kind in ("containers", "volumes", "networks"):
            with self.subTest(kind=kind), patch.object(cleanup, "command", side_effect=["resource-id\n", '{"name":"resource"}\n']) as command:
                self.assertEqual(cleanup.inspect_resources(kind), [{"name": "resource"}])
                arguments = command.call_args.args[0]
                self.assertIn("--format", arguments)
                self.assertNotIn(".Env", " ".join(arguments))
                self.assertEqual(arguments[-1], "resource-id")

    def test_invalid_project_is_rejected_before_docker(self):
        with patch.object(cleanup, "command") as command:
            for project in (None, "", "--all", "../user", "a/b"):
                with self.assertRaises(EnvironmentError):
                    cleanup.cleanup_project(project, apply=True)
            command.assert_not_called()


if __name__ == "__main__":
    unittest.main()
