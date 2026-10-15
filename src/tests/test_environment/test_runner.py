# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[3]/"tools"))
from test_environment.config import EnvironmentError
from test_environment.runner import case, select, preflight
from test_environment.config import Config

class RunnerTests(unittest.TestCase):
    def test_selection_and_missing_registration(self):
        declaration={"integration":{}}
        with patch("test_environment.runner.dependencies",return_value=declaration):
            self.assertEqual(select(["unit","integration"],"unit"),["unit"])
            self.assertEqual(select(["unit","integration"],"integration"),["integration"])
            self.assertEqual(select(["unit","integration"],"all"),["unit","integration"])
            with self.assertRaises(EnvironmentError):select(["unit"],"all")
    def test_failure_skip_and_actual_execution_are_distinct(self):
        with tempfile.TemporaryDirectory() as root,patch("test_environment.runner.dependencies",return_value={}),patch("test_environment.runner.report_path",return_value=Path(root)/"report.json"):
            self.assertEqual(case("unit",[sys.executable,"-c","print('PASS')"]),0)
            self.assertTrue(json.loads((Path(root)/"report.json").read_text())["executed"])
            self.assertEqual(case("unit",[sys.executable,"-c","print('[SKIP] no DB')"]),1)
            self.assertEqual(json.loads((Path(root)/"report.json").read_text())["status"],"skipped")
            self.assertEqual(case("unit",["/missing/artifact"]),1)
    def test_doctor_is_read_only_and_authentication_is_not_retried(self):
        declaration={"services":["mysql"],"role":"driver","resources":[],"location":"both"}
        with patch("test_environment.runner.prepare_databases") as prepare,patch("test_environment.runner.probe_database",return_value={"category":"ready"}) as probe:
            preflight(Config(),[declaration],"native")
            prepare.assert_not_called()
            self.assertFalse(probe.call_args.args[4])
        from test_environment.fixture import wait_databases
        with patch("test_environment.fixture.probe_database",side_effect=EnvironmentError("authentication_failed","connect","mysql")) as probe:
            with self.assertRaises(EnvironmentError):wait_databases(Config(),["mysql"])
            self.assertEqual(probe.call_count,1)
    def test_preparation_has_one_total_budget(self):
        declaration={"services":["mysql","postgres"],"role":"driver","resources":[],"location":"both"}
        with patch("test_environment.runner.time.monotonic",side_effect=[0,0,0,61]),patch("test_environment.runner.probe_database",return_value={"category":"ready"}) as probe:
            with self.assertRaises(EnvironmentError) as error:preflight(Config(),[declaration],"native")
            self.assertEqual(error.exception.category,"preparation_timeout")
            self.assertEqual(probe.call_count,1)
    def test_resource_lock_comes_from_the_dependency_declaration(self):
        declaration={"services":["mysql"],"role":"driver","resources":["shared_tables"],"location":"native"}
        with tempfile.TemporaryDirectory() as root,patch("test_environment.runner.dependencies",return_value={"integration":declaration}),patch("test_environment.runner.preflight"),patch("test_environment.runner.lock",return_value=contextlib.nullcontext()) as locking,patch("test_environment.runner.report_path",return_value=Path(root)/"report.json"):
            self.assertEqual(case("integration",[sys.executable,"-c","print('passed')"]),0)
            locking.assert_called_once_with(Config().data["project"]+"-shared_tables")
    def test_preflight_failure_does_not_execute_business_case(self):
        declaration={"services":["mysql"],"role":"driver","resources":[],"location":"both"}
        with tempfile.TemporaryDirectory() as root,patch("test_environment.runner.dependencies",return_value={"integration":declaration}),patch("test_environment.runner.preflight",side_effect=EnvironmentError("authentication_failed","connect","mysql")),patch("test_environment.runner.report_path",return_value=Path(root)/"report.json"):
            self.assertEqual(case("integration",[sys.executable,"-c","raise AssertionError('must not execute')"]),1)
            self.assertFalse(json.loads((Path(root)/"report.json").read_text())["executed"])

if __name__=="__main__":
    # Expected inner failure/skip reports are assertions, not this suite's coverage result.
    with contextlib.redirect_stdout(io.StringIO()):unittest.main()
