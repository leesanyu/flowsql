# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Real controlled HTTP -> scheduler -> NPM/Baseliner, plus installed MCP."""

import copy
import hashlib
import http.client
import json
import os
import queue
import secrets
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from decimal import Decimal
from pathlib import Path

import jsonschema

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "src/tests/test_config_channel"))
from test_npm_semantics_pipeline import (
    assert_dns_rows,
    publish_labels,
    write_dns_capture,
)

SCHEMAS = {
    t["name"]: t
    for t in json.loads(
        (ROOT / "config/agent-data-access/api.schema.json").read_text()
    )["tools"]
}
EVIDENCE = ROOT / "evidence/agent-data-access/analysis"
EVIDENCE.mkdir(parents=True, exist_ok=True)


def port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class Host:
    def __init__(self, executable, root, environment):
        self.root, self.port = root, port()
        self.lines = queue.Queue()
        self.log = (EVIDENCE / "host.log").open("a")
        self.child = subprocess.Popen(
            ["stdbuf", "-oL", str(executable), str(root), str(self.port)],
            cwd=executable.parent,
            env=environment,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.log,
            text=True,
        )

        def reader():
            for line in self.child.stdout:
                self.log.write(line)
                self.log.flush()
                if line.startswith("ACCESS_"):
                    self.lines.put(line.rstrip())

        self.reader = threading.Thread(target=reader)
        self.reader.start()
        self.expect("ACCESS_READY")

    def expect(self, prefix):
        try:
            line = self.lines.get(timeout=25)
        except queue.Empty:
            raise AssertionError(
                "host did not reply; see " + str(EVIDENCE / "host.log")
            )
        assert line.startswith(prefix), line
        return line[len(prefix) :]

    def control(self, path, payload, pending=False):
        self.child.stdin.write(json.dumps({"path": path, "payload": payload}) + "\n")
        self.child.stdin.flush()
        reply = json.loads(self.expect("ACCESS_REPLY "))
        if pending and reply["rc"] == -2:
            return None
        assert reply["rc"] == 0, reply
        return reply["result"]

    def stop(self, kill=False):
        if self.child.poll() is None:
            if kill:
                self.child.kill()
            else:
                self.child.stdin.write("quit\n")
                self.child.stdin.flush()
            self.child.wait(timeout=15)
        self.reader.join(timeout=2)
        self.log.close()
        if not kill:
            assert self.child.returncode == 0, self.child.returncode


class Access:
    def __init__(self, host, tokens):
        self.host, self.tokens, self.sequence = host, tokens, 0
        self.source_cache = {}

    def call(self, name, arguments, user="alice", status=200):
        if status == 200:
            jsonschema.Draft4Validator(SCHEMAS[name]["inputSchema"]).validate(arguments)
        data = json.dumps({"name": name, "arguments": arguments}).encode()
        request = urllib.request.Request(
            f"http://127.0.0.1:{self.host.port}/data-access/call",
            data=data,
            headers={
                "Authorization": "Bearer " + self.tokens[user],
                "Content-Type": "application/json",
            },
        )
        try:
            response = urllib.request.urlopen(request, timeout=35)
        except urllib.error.HTTPError as error:
            response = error
        result = json.loads(response.read())
        assert response.code == status, (name, response.code, result)
        if status == 200:
            jsonschema.Draft4Validator(SCHEMAS[name]["outputSchema"]).validate(result)
        return result

    def sources(self, user="alice"):
        if user in self.source_cache:
            return self.source_cache[user]
        result, offset = [], 0
        while True:
            page = self.call("list_sources", {"offset": offset, "limit": 2}, user)
            result.extend(page["sources"])
            if not page["has_more"]:
                self.source_cache[user] = result
                return result
            offset += len(page["sources"])

    def lose_submit_response(self, arguments):
        connection = http.client.HTTPConnection("127.0.0.1", self.host.port, timeout=35)
        connection.request(
            "POST",
            "/data-access/call",
            json.dumps({"name": "submit_analysis", "arguments": arguments}),
            {
                "Authorization": "Bearer " + self.tokens["alice"],
                "Content-Type": "application/json",
            },
        )
        response = connection.getresponse()
        assert response.status == 200
        # Drop the entire operation response body, then recover by durable request identity.
        response.close()
        connection.close()
        return self.call("get_operation", {"request_id": arguments["request_id"]})

    def wait(self, identity, expected="completed"):
        deadline = time.monotonic() + 35
        while time.monotonic() < deadline:
            state = self.call("get_operation", {"operation_id": identity})
            if state["status"] not in ("queued", "running", "cancelling"):
                assert state["status"] == expected, state
                return state
            time.sleep(0.015)
        raise AssertionError("operation did not terminate: " + identity)

    def read(self, reference):
        lease = self.call(
            "acquire_result_lease", {"result_ref": reference, "duration_ms": 300000}
        )
        offset, rows, encoded = 0, [], []
        while True:
            page = self.call(
                "read_result",
                {
                    "result_ref": reference,
                    "offset": offset,
                    "limit": 3,
                    "lease_id": lease["lease_id"],
                },
            )
            rows += page["rows"]
            encoded += page["row_json"]
            if not page["has_more"]:
                break
            offset += len(page["rows"])
        digest = hashlib.sha256(
            (
                page["integrity_header"] + "\n" + "".join(r + "\n" for r in encoded)
            ).encode()
        ).hexdigest()
        assert digest == page["content_sha256"]
        self.call("release_result_lease", {"lease_id": lease["lease_id"]})
        page["rows"] = rows
        return page

    def query(self, reference, workflow, capture=False):
        self.sequence += 1
        operation = self.call(
            "query",
            {
                "request_id": f"q-{self.sequence}",
                "source_ref": reference,
                "workflow": workflow,
                "capture_snapshot": capture,
                "max_rows": 1000,
                "timeout_ms": 30000,
            },
        )
        state = self.wait(operation["operation_id"])
        result = self.read(state["result_ref"])

        def value(cell):
            if cell is None:
                return None
            return {
                "int64": int,
                "float64": float,
                "decimal": Decimal,
                "string": str,
                "binary_hex": bytes.fromhex,
            }[cell["type"]](cell["value"])

        result["decoded"] = [
            dict(zip([f["name"] for f in result["fields"]], map(value, row)))
            for row in result["rows"]
        ]
        return result


def view(query):
    return {"type": "view", "query": [query]}


def convert_captures(root):
    write_dns_capture(root / "pcap")
    content = (root / "pcap").read_bytes()
    position, records = 24, []
    while position < len(content):
        sec, micros, size, wire = struct.unpack_from("<IIII", content, position)
        position += 16
        records.append(
            (sec * 1000000 + micros, content[position : position + size], wire)
        )
        position += size

    def block(kind, body):
        body += b"\0" * (-len(body) % 4)
        return (
            struct.pack("<II", kind, len(body) + 12)
            + body
            + struct.pack("<I", len(body) + 12)
        )

    with (root / "pcapng").open("wb") as output:
        output.write(block(0x0A0D0D0A, struct.pack("<IHHq", 0x1A2B3C4D, 1, 0, -1)))
        output.write(block(1, struct.pack("<HHI", 1, 0, 65535)))
        for timestamp, body, wire in records:
            output.write(
                block(
                    6,
                    struct.pack(
                        "<IIIII",
                        0,
                        timestamp >> 32,
                        timestamp & 0xFFFFFFFF,
                        len(body),
                        wire,
                    )
                    + body,
                )
            )
    with (root / "slow").open("wb") as output:
        output.write(content[:24])
        for cycle in range(12000):
            for timestamp, body, wire in records:
                sec, micros = divmod(timestamp + cycle * 10000000, 1000000)
                output.write(struct.pack("<IIII", sec, micros, len(body), wire) + body)
    assert (root / "slow").stat().st_size < 67108864


def policy():
    pcap = lambda name: {"kind": "block_stream", "type": "pcapfile", "name": name}
    table = {
        "kind": "database_table",
        "type": "sqlite",
        "name": "business",
        "schema": "main",
        "table": "one",
    }
    frame = {"kind": "dataframe", "name": "input"}
    labels = {"kind": "config", "reference": "config.npm-labels@1"}
    config = {
        "schema_version": 1,
        "core": {"labeling": "config.npm-labels@1"},
        "dns": {"primary_label_ids": [1002]},
    }
    profiles = [
        {
            "name": "dns_" + name,
            "operator": {"category": "npm", "name": "basic"},
            "source": pcap(name),
            "parameters": {
                "features": "basic,session,labeling,dns",
                "observing": "dns_transaction_ipv4",
                "parameters": json.dumps(config, separators=(",", ":")),
            },
            "config_refs": ["config.npm-labels@1"],
            "output": {"kind": "managed_database", "type": "sqlite", "name": "results"},
            "max_input_bytes": 67108864,
        }
        for name in ("pcap", "pcapng", "slow")
    ]
    baseline = json.loads(
        (
            ROOT
            / "src/tests/test_config_channel/fixtures/operator-baseliner-configs.json"
        ).read_text()
    )["value"]["config"]
    for name, source in [
        ("baseline_frame", frame),
        ("baseline_database", table),
        ("hidden_input", table),
    ]:
        config = copy.deepcopy(baseline)
        config["task_key"] = name
        if name != "baseline_frame":
            config["source"] = "sqlite.business"
            config["datasets"][0]["table"] = "one"
            config["datasets"][0]["scope"] = {
                "consistency": "immutable_range",
                "begin_bucket": 0,
                "end_bucket": 100,
            }
        if name == "hidden_input":
            second = copy.deepcopy(config["datasets"][0])
            second.update(id="hidden", table="two")
            config["datasets"].append(second)
        profiles.append(
            {
                "name": name,
                "operator": {"category": "explore", "name": "baseliner"},
                "source": source,
                "parameters": {"parameters": json.dumps(config, separators=(",", ":"))},
                "config_refs": [],
                "output": {"kind": "dataframe"},
            }
        )
    alice = {
        "principal": "alice",
        "token_env": "ACCESS_ALICE_TOKEN",
        "max_rows": 1000,
        "max_bytes": 8388608,
        "timeout_ms": 30000,
        "result_ttl_ms": 3600000,
        "analyses": profiles,
        "sources": [
            {"locator": pcap(name), "operations": ["analyze"]}
            for name in ("pcap", "pcapng", "slow")
        ]
        + [
            {"locator": labels, "operations": ["analyze"]},
            {"locator": table, "operations": ["query", "analyze"]},
            {"locator": frame, "operations": ["query", "analyze"]},
        ],
    }
    bob_profile = copy.deepcopy(profiles[0])
    bob_profile["output"]["name"] = "results_bob"
    bob = {
        "principal": "bob",
        "token_env": "ACCESS_BOB_TOKEN",
        "analyses": [bob_profile],
        "sources": [
            {"locator": pcap("pcap"), "operations": ["analyze"]},
            {"locator": labels, "operations": []},
        ],
    }
    return {"subjects": [alice, bob]}


def prepare(access, name, user="alice"):
    operations = access.call("list_operations", {}, user)["operations"]
    profile = next(p for p in operations if p["operation_ref"]["operation_id"] == name)
    source = next(s for s in access.sources(user) if s["locator"] == profile["source"])
    return {
        "operation_ref": profile["operation_ref"],
        "source_ref": source["source_ref"],
        "capture_snapshot": source["kind"] == "dataframe",
    }


def dns_check(access, manifest):
    basis = manifest["basis"]
    run = basis["managed_result"]["run_id"]
    relations = [
        s
        for s in basis["derived_sources"]
        if "dns_transaction" in s["locator"].get("table", "")
    ]
    assert len(relations) == 6, relations
    values = {}
    for relation in relations:
        locator = relation["locator"]
        source = access.call("describe_source", {"source_ref": relation["source_ref"]})
        fields = {f["name"]: f["field_id"] for f in source["description"]["fields"]}
        ref = source["source_ref"]
        filters = [
            {
                "fid": fields["__npm_run_id"],
                "rule": {"type": "comparison", "op": "eq", "value": run},
            }
        ]
        workflow = [{"type": "filter", "filters": filters}]
        raw = access.query(
            ref, workflow + [view({"op": "raw", "fields": list(fields.values())})]
        )["decoded"]
        assert_dns_rows(raw)
        family = 4 if "ipv4" in locator["table"] else 6
        values[locator["table"]] = raw
        aggregate = {
            "op": "aggregate",
            "groupBy": [],
            "measures": [{"field": "*", "agg": "count", "asFieldKey": "n"}],
        }
        assert access.query(ref, workflow + [view(aggregate)])["decoded"] == [{"n": 8}]
        if not locator["table"].endswith("_final"):
            continue
        grouped = {
            "op": "aggregate",
            "groupBy": [fields["outcome"]],
            "measures": aggregate["measures"],
        }
        outcomes = access.query(ref, workflow + [view(grouped)])["decoded"]
        assert {r["outcome"]: r["n"] for r in outcomes} == {
            "matched": 3,
            "query_only": 2,
            "response_only": 1,
            "truncated_response": 1,
            "unsupported_message": 1,
        }
        valid = filters + [
            {
                "fid": fields["outcome"],
                "rule": {"type": "comparison", "op": "eq", "value": "matched"},
            },
            {"fid": fields["latency_ns"], "rule": {"type": "is not null"}},
            {
                "fid": fields["response_tc"],
                "rule": {"type": "comparison", "op": "eq", "value": 0},
            },
        ]
        measures = aggregate["measures"] + [
            {"field": fields["latency_ns"], "agg": agg, "asFieldKey": agg}
            for agg in ("mean", "min", "max")
        ]
        stats = access.query(
            ref,
            [
                {"type": "filter", "filters": valid},
                view({"op": "aggregate", "groupBy": [], "measures": measures}),
            ],
        )["decoded"][0]
        assert stats["n"] == 2 and all(
            abs(stats[k] - 10000000) < 1e-6 for k in ("mean", "min", "max")
        ), stats
        grouped["groupBy"] = [fields["response_rcode"]]
        rcodes = access.query(ref, workflow + [view(grouped)])["decoded"]
        assert {r["response_rcode"]: r["n"] for r in rcodes} == {None: 3, 0: 4, 3: 1}, (
            rcodes
        )
        grouped["groupBy"] = [fields["latency_ns"]]
        distribution = access.query(
            ref, [{"type": "filter", "filters": valid}, view(grouped)]
        )["decoded"]
        assert distribution == [{"latency_ns": 10000000, "n": 2}], distribution
        histogram = access.query(
            ref,
            workflow
            + [view({"op": "histogram", "field": fields["query_at_ns"], "bins": 2})],
        )["decoded"]
        base = family * 1000000000
        assert [r["bin_count"] for r in histogram] == [3, 4], histogram
        assert [r["bin_lower"] for r in histogram] == [base, base + 56500000], histogram
        assert [r["bin_upper"] for r in histogram] == [
            base + 56500000,
            base + 113000000,
        ], histogram
    for family in (4, 6):
        ids = [
            {r["entity_instance_id"] for r in rows}
            for table, rows in values.items()
            if f"ipv{family}" in table
        ]
        assert ids[0] == ids[1] == ids[2] and len(ids[0]) == 8
    return values


def runtime_running(access, operation):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        record = access.call(
            "get_operation", {"operation_id": operation["operation_id"]}
        )
        if record.get("backend_submitted"):
            state = access.host.control(
                "/scheduler/batch/status",
                {"runtime_task_id": record["runtime_task_id"]},
                pending=True,
            )
            if state and state["status"] == "running":
                return record
            assert state is None or state["status"] in (
                "queued",
                "pending",
                "running",
            ), state
        time.sleep(0.01)
    raise AssertionError(("real scheduler did not start", record))


def mcp_check(access, environment, root, operation, result):
    python = os.environ["FLOWSQL_TEST_MCP_PYTHON"]
    environment = dict(environment, ACCESS_MCP_TOKEN=secrets.token_urlsafe(32))
    environment.pop("PYTHONPATH", None)
    number = port()
    public = f"http://127.0.0.1:{number}/mcp"
    config = root / "mcp.json"
    config.write_text(
        json.dumps(
            {
                "host": "127.0.0.1",
                "port": number,
                "public_url": public,
                "upstream_url": f"http://127.0.0.1:{access.host.port}",
                "bindings": [
                    {
                        "principal": "alice",
                        "token_env": "ACCESS_MCP_TOKEN",
                        "upstream_token_env": "ACCESS_ALICE_TOKEN",
                    }
                ],
            }
        )
    )
    for _ in range(2):
        with (EVIDENCE / "mcp.log").open("a") as log:
            process = subprocess.Popen(
                [python, "-m", "flowsql_mcp", "--config", str(config)],
                cwd="/tmp",
                env=environment,
                stdout=log,
                stderr=log,
            )
            try:
                deadline = time.monotonic() + 12
                while time.monotonic() < deadline and process.poll() is None:
                    try:
                        if (
                            urllib.request.urlopen(
                                public.replace("/mcp", "/ready"), timeout=1
                            ).code
                            == 200
                        ):
                            break
                    except (OSError, urllib.error.HTTPError):
                        pass
                    time.sleep(0.05)
                else:
                    raise AssertionError("MCP not ready; see mcp.log")
                probe = subprocess.run(
                    [
                        python,
                        str(ROOT / "src/mcp/tests/analysis_probe.py"),
                        public,
                        operation["operation_id"],
                        operation["result_ref"],
                    ],
                    cwd="/tmp",
                    env=environment,
                    check=False,
                    capture_output=True,
                    text=True,
                    timeout=20,
                )
                assert probe.returncode == 0, probe.stdout + probe.stderr
                observed = json.loads(probe.stdout.strip().splitlines()[-1])
                assert observed["content_sha256"] == result["content_sha256"]
                assert access.host.child.poll() is None
            finally:
                process.terminate()
                process.wait(timeout=5)


def main():
    executable = Path(sys.argv[1]).resolve()
    tokens = {"alice": secrets.token_urlsafe(32), "bob": secrets.token_urlsafe(32)}
    environment = dict(
        os.environ, ACCESS_ALICE_TOKEN=tokens["alice"], ACCESS_BOB_TOKEN=tokens["bob"]
    )
    with tempfile.TemporaryDirectory(prefix="flowsql-access-analysis-") as temporary:
        root = Path(temporary)
        for directory in ("frames", "uploads"):
            (root / directory).mkdir()
        convert_captures(root)
        (root / "policy.json").write_text(json.dumps(policy()))
        host = Host(executable, root, environment)
        access = Access(host, tokens)
        try:
            assert publish_labels(host.control) == "config.npm-labels@1"
            print("CHECK: authentic finite preflight and authorization", flush=True)
            bob_args = prepare(access, "dns_pcap", "bob")
            assert (
                access.call("preflight_analysis", bob_args, "bob", 403)["error"]["code"]
                == "FORBIDDEN"
            )
            hidden = prepare(access, "hidden_input")
            assert (
                access.call("preflight_analysis", hidden, status=403)["error"]["code"]
                == "FORBIDDEN"
            )
            previous = None
            for name in ("dns_pcap", "dns_pcapng"):
                args = prepare(access, name)
                preflight = access.call("preflight_analysis", args)
                assert (
                    preflight["available"]
                    and preflight["component_revision"]
                    and preflight["contract_ref"]
                )
                for key in ("sql", "path", "parameters", "output"):
                    access.call(
                        "submit_analysis",
                        dict(args, request_id="attack", **{key: "rejected"}),
                        status=400,
                    )
                request = dict(args, request_id=name, timeout_ms=30000)
                operation = (
                    access.lose_submit_response(request)
                    if name == "dns_pcap"
                    else access.call("submit_analysis", request)
                )
                duplicate = access.call("submit_analysis", request)
                assert duplicate["operation_id"] == operation["operation_id"]
                access.call(
                    "submit_analysis", dict(request, timeout_ms=29999), status=409
                )
                state = access.wait(operation["operation_id"])
                manifest = access.read(state["result_ref"])
                assert manifest["basis"]["runtime_task_id"] == state["runtime_task_id"]
                access.call(
                    "get_operation", {"operation_id": state["operation_id"]}, "bob", 404
                )
                access.call(
                    "read_result", {"result_ref": state["result_ref"]}, "bob", 404
                )
                derived = dns_check(access, manifest)
                (EVIDENCE / (name + ".json")).write_text(
                    json.dumps(derived, indent=2, default=str)
                )
                if previous is not None:
                    for table in derived:

                        def normalized(rows):
                            return sorted(
                                [
                                    {
                                        k: v
                                        for k, v in r.items()
                                        if k
                                        not in (
                                            "__npm_run_id",
                                            "__npm_task_id",
                                            "session_id",
                                            "entity_instance_id",
                                            "observation_domain_id",
                                        )
                                    }
                                    for r in rows
                                ],
                                key=lambda r: r["dns_id"],
                            )

                        assert normalized(previous[table]) == normalized(
                            derived[table]
                        ), [
                            {k: (a[k], b[k]) for k in a if a[k] != b[k]}
                            for a, b in zip(
                                normalized(previous[table]), normalized(derived[table])
                            )
                        ]
                previous = derived
                good_state, good_manifest = state, manifest
                print(
                    "CHECK: "
                    + name
                    + " DNS raw/aggregate/histogram and immutable pages",
                    flush=True,
                )
            for name in ("baseline_frame", "baseline_database"):
                args = prepare(access, name)
                if name == "baseline_frame":
                    access.call(
                        "preflight_analysis",
                        dict(args, capture_snapshot=False),
                        status=422,
                    )
                access.call("preflight_analysis", args)
                operation = access.call(
                    "submit_analysis", dict(args, request_id=name, timeout_ms=30000)
                )
                state = access.wait(operation["operation_id"])
                manifest = access.read(state["result_ref"])
                derived = manifest["basis"]["derived_sources"][0]
                source = access.call(
                    "describe_source", {"source_ref": derived["source_ref"]}
                )
                access.call(
                    "describe_source", {"source_ref": source["source_ref"]}, "bob", 403
                )
                fields = {
                    f["name"]: f["field_id"] for f in source["description"]["fields"]
                }
                result = access.query(
                    source["source_ref"],
                    [
                        view(
                            {
                                "op": "raw",
                                "fields": [
                                    fields[k]
                                    for k in (
                                        "target_bucket",
                                        "observed",
                                        "result_kind",
                                    )
                                ],
                            }
                        ),
                        {
                            "type": "sort",
                            "by": [fields["target_bucket"]],
                            "sort": "ascending",
                        },
                    ],
                    capture=True,
                )["decoded"]
                assert [r["observed"] for r in result] == [2.5, 3.5, 4.5], result
                print("CHECK: actual " + name + " declared operator", flush=True)
            mcp_check(access, environment, root, good_state, good_manifest)
            slow = prepare(access, "dns_slow")
            for reason in ("cancel", "timeout"):
                operation = access.call(
                    "submit_analysis",
                    dict(
                        slow,
                        request_id=reason,
                        timeout_ms=5000 if reason == "timeout" else 30000,
                    ),
                )
                actual = runtime_running(access, operation)
                if reason == "cancel":
                    access.call(
                        "cancel_operation", {"operation_id": operation["operation_id"]}
                    )
                terminal = access.wait(
                    operation["operation_id"],
                    "cancelled" if reason == "cancel" else "failed",
                )
                assert terminal["error_code"] == (
                    "CANCELLED" if reason == "cancel" else "TIMEOUT"
                ), terminal
                underlying = host.control(
                    "/scheduler/batch/status",
                    {"runtime_task_id": actual["runtime_task_id"]},
                )
                assert underlying["status"] in ("cancelled", "stopped", "timeout"), (
                    underlying
                )
                print("CHECK: underlying " + reason + " confirmed", flush=True)
            # A fresh task on the same source can start after reader reclamation.
            crash_args = dict(slow, request_id="crash", timeout_ms=30000)
            operation = access.call("submit_analysis", crash_args)
            actual = runtime_running(access, operation)
            host.stop(kill=True)
            with sqlite3.connect(root / "results.db") as database:
                runs_before = database.execute(
                    "SELECT run_id FROM npm_result_runs ORDER BY run_id"
                ).fetchall()
            host = Host(executable, root, environment)
            access.host = host
            recovered = access.call(
                "get_operation", {"operation_id": operation["operation_id"]}
            )
            assert recovered["status"] == "reconcile_required", recovered
            duplicate = access.call("submit_analysis", crash_args)
            assert (
                duplicate["operation_id"] == operation["operation_id"]
                and duplicate["status"] == "reconcile_required"
            )
            access.call("get_operation", {"operation_id": operation["operation_id"]})
            assert (
                access.call(
                    "get_operation", {"operation_id": good_state["operation_id"]}
                )["result_ref"]
                == good_state["result_ref"]
            )
            assert (
                access.read(good_state["result_ref"])["content_sha256"]
                == good_manifest["content_sha256"]
            )
            with sqlite3.connect(root / "results.db") as database:
                assert (
                    database.execute(
                        "SELECT run_id FROM npm_result_runs ORDER BY run_id"
                    ).fetchall()
                    == runs_before
                )
            print(
                "PASS: DNS PCAP/PCAPNG, real non-NPM database/DataFrame analysis, isolation, cancellation, timeout, restart deduplication, installed MCP restart",
                flush=True,
            )
        finally:
            host.stop()


if __name__ == "__main__":
    main()
