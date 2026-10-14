# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.

"""链路 CSV 时间转换的契约与 Worker 发现验证。"""

import csv
from datetime import datetime, timedelta, timezone
from pathlib import Path
import unittest

import polars as pl
from polars.testing import assert_frame_equal

from flowsql import OperatorRegistry
from flowsql.arrow_codec import decode_arrow_ipc, encode_arrow_ipc


ROOT = Path(__file__).resolve().parents[3]


class LinkTimestampTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.registry = OperatorRegistry()
        cls.registry.discover(str(ROOT / "samples/python_operator"))
        cls.operator = cls.registry.get("explore", "link_timestamp")
        if cls.operator is None:
            raise AssertionError("Worker 未发现 explore.link_timestamp")

    def test_shanghai_time_and_source_preserved(self):
        source = pl.DataFrame({
            "time": ["2021/6/26 15:24", "2021/6/26 15:23"],
            "Mbps": [1966.08, 1955.84],
        })
        original = source.clone()
        output = self.operator.work(source)

        self.assertEqual(output["time_s"].to_list(), [1624692240, 1624692180])
        self.assertEqual(output.schema["time_s"], pl.Int64)
        self.assertEqual(output.schema["link"], pl.String)
        self.assertEqual(output["link"].to_list(), ["link_1", "link_1"])
        assert_frame_equal(source, original)
        assert_frame_equal(output.select(source.columns), original)

    def test_existing_link_is_preserved(self):
        source = pl.DataFrame({
            "time": ["2021/6/26 15:23", "2021/6/26 15:24"],
            "link": ["wan_a", "wan_b"],
            "Mbps": [1955.84, 1966.08],
        })
        output = self.operator.work(source)
        assert_frame_equal(output.select(source.columns), source)

    def test_empty_frame_keeps_output_schema(self):
        source = pl.DataFrame(schema={"time": pl.String, "Mbps": pl.Float64})
        output = self.operator.work(source)
        self.assertEqual(output.height, 0)
        self.assertEqual(output.schema, {
            "time": pl.String,
            "Mbps": pl.Float64,
            "time_s": pl.Int64,
            "link": pl.String,
        })

    def test_missing_time_fails(self):
        with self.assertRaisesRegex(ValueError, "time"):
            self.operator.work(pl.DataFrame({"Mbps": [1955.84]}))

    def test_non_text_time_fails(self):
        with self.assertRaisesRegex(ValueError, "time"):
            self.operator.work(pl.DataFrame({"time": [1624692180]}))

    def test_null_time_fails(self):
        source = pl.DataFrame({"time": ["2021/6/26 15:23", None]})
        with self.assertRaisesRegex(ValueError, "time"):
            self.operator.work(source)

    def test_invalid_time_fails(self):
        for value in ("", "not-a-date", "2021/2/30 15:23", "2021/6/26 25:23"):
            with self.subTest(time=value), self.assertRaisesRegex(ValueError, "time"):
                self.operator.work(pl.DataFrame({"time": [value]}))

    def test_full_csv_and_arrow_round_trip(self):
        path = ROOT / "src/tests/data/csv/link_data_2_month.csv"
        source = pl.read_csv(path)
        output = self.operator.work(source)
        with path.open(encoding="utf-8-sig", newline="") as csv_file:
            tz = timezone(timedelta(hours=8))
            expected = [
                int(datetime.strptime(row["time"], "%Y/%m/%d %H:%M")
                    .replace(tzinfo=tz).timestamp())
                for row in csv.DictReader(csv_file)
            ]

        self.assertEqual(output.height, 57761)
        self.assertEqual(output["time_s"].to_list(), expected)
        self.assertEqual(output["time_s"].diff().drop_nulls().unique().to_list(), [60])
        self.assertEqual(output["link"].unique().to_list(), ["link_1"])
        assert_frame_equal(output.select(source.columns), source)
        decoded = decode_arrow_ipc(encode_arrow_ipc(output))
        assert_frame_equal(decoded, output)


if __name__ == "__main__":
    unittest.main()
