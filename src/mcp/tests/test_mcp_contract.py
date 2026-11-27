"""Real MCP client + independent adapter process; HTTP authority is an explicit fixture."""

import asyncio
import json
import os
import socket
import sqlite3
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import httpx
import pytest
from flowsql_mcp.config import ConfigurationError, McpConfig
from mcp.client.streamable_http import streamable_http_client

from mcp import ClientSession

ALICE_MCP = "alice-mcp-fixture-credential-000000000"
ALICE_FLOW = "alice-flow-fixture-credential-00000000"
BOB_MCP = "bob-mcp-fixture-credential-00000000000"
BOB_FLOW = "bob-flow-fixture-credential-0000000000"


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


@pytest.fixture
def anyio_backend():
    return "asyncio"


@pytest.fixture
def upstream():
    contract = json.loads(
        (
            Path(__file__).parents[3] / "config/agent-data-access/api.schema.json"
        ).read_text()
    )
    calls = []

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def handle_call(self):
            token = self.headers.get("Authorization")
            body = json.loads(
                self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}"
            )
            calls.append((token, self.path, body))
            status = 200
            if token not in ("Bearer " + ALICE_FLOW, "Bearer " + BOB_FLOW):
                status, value = (
                    401,
                    {
                        "error": {
                            "code": "UNAUTHENTICATED",
                            "message": "invalid downstream credential",
                        }
                    },
                )
            elif self.path == "/data-access/tools":
                value = {"tools": contract["tools"]}
            elif body["name"] == "list_sources":
                value = {
                    "sources": [
                        {
                            "fixture_principal": "alice"
                            if token.endswith(ALICE_FLOW)
                            else "bob",
                            "source_ref": {
                                "source_id": "fixture",
                                "source_revision": "fixture",
                            },
                            "kind": "dataframe",
                            "provider": "fixture",
                            "name": "fixture",
                            "locator": {},
                            "description": {},
                            "capabilities": {},
                        }
                    ],
                    "total_sources": 1,
                    "offset": 0,
                    "has_more": False,
                }
            elif body["name"] == "cancel_operation":
                value = {
                    "operation_id": body["arguments"]["operation_id"],
                    "request_id": "fixture",
                    "kind": "query",
                    "arguments_sha256": "fixture",
                    "status": "cancelling",
                }
            elif body["name"] == "get_operation":
                value = {
                    "operation_id": body["arguments"]["operation_id"],
                    "request_id": "fixture",
                    "kind": "query",
                    "arguments_sha256": "fixture",
                    "status": "cancelled",
                }
            elif body["name"] == "query":
                status, value = (
                    403,
                    {
                        "error": {
                            "code": "FORBIDDEN",
                            "message": "fixture scope rejects source",
                        }
                    },
                )
            else:
                status, value = (
                    422,
                    {
                        "error": {
                            "code": "UNSUPPORTED_OPERATION",
                            "message": "fixture operation unavailable",
                        }
                    },
                )
            data = json.dumps(value).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        do_GET = handle_call
        do_POST = handle_call

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever)
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_port}", calls, server
    finally:
        server.shutdown()
        thread.join()
        server.server_close()


@pytest.fixture
def adapter(tmp_path, upstream, monkeypatch):
    url, calls, _ = upstream
    port = free_port()
    public = f"http://127.0.0.1:{port}/mcp"
    config_file = tmp_path / "mcp.json"
    config_file.write_text(
        json.dumps(
            {
                "host": "127.0.0.1",
                "port": port,
                "public_url": public,
                "upstream_url": url,
                "bindings": [
                    {
                        "principal": "alice",
                        "token_env": "TEST_MCP_ALICE",
                        "upstream_token_env": "TEST_FLOW_ALICE",
                    },
                    {
                        "principal": "bob",
                        "token_env": "TEST_MCP_BOB",
                        "upstream_token_env": "TEST_FLOW_BOB",
                    },
                ],
            }
        )
    )
    for name, value in {
        "TEST_MCP_ALICE": ALICE_MCP,
        "TEST_FLOW_ALICE": ALICE_FLOW,
        "TEST_MCP_BOB": BOB_MCP,
        "TEST_FLOW_BOB": BOB_FLOW,
    }.items():
        monkeypatch.setenv(name, value)
    environment = dict(os.environ)
    environment.pop("PYTHONPATH", None)
    log = (tmp_path / "adapter.log").open("w+")
    child = subprocess.Popen(
        [sys.executable, "-m", "flowsql_mcp", "--config", str(config_file)],
        cwd=tmp_path,
        env=environment,
        stdout=log,
        stderr=log,
    )
    deadline = time.monotonic() + 12
    with httpx.Client(trust_env=False) as client:
        while time.monotonic() < deadline and child.poll() is None:
            try:
                if (
                    client.get(public.replace("/mcp", "/ready"), timeout=1).status_code
                    == 200
                ):
                    break
            except httpx.HTTPError:
                pass
            time.sleep(0.05)
        else:
            child.terminate()
            child.wait(timeout=5)
            log.seek(0)
            pytest.fail(log.read())
    try:
        yield public, calls, child, config_file
    finally:
        child.terminate()
        child.wait(timeout=5)
        log.close()


@pytest.mark.anyio
async def test_real_client_discovery_identity_errors_and_cancellation(adapter):
    url, calls, _, _ = adapter
    for token, principal in ((ALICE_MCP, "alice"), (BOB_MCP, "bob")):
        async with (
            httpx.AsyncClient(
                headers={"Authorization": "Bearer " + token}, trust_env=False
            ) as client,
            streamable_http_client(url, http_client=client) as (read, write, _),
            ClientSession(read, write) as session,
        ):
            initialized = await session.initialize()
            assert initialized.protocolVersion == "2025-11-25"
            listed = await session.list_tools()
            assert {"list_sources", "query", "read_result", "cancel_operation"} <= {
                t.name for t in listed.tools
            }
            result = await session.call_tool("list_sources", {})
            assert (
                result.structuredContent["sources"][0]["fixture_principal"] == principal
            )
            assert result.structuredContent["total_sources"] == 1
            rejected = await session.call_tool(
                "query", {"request_id": "denied", "source_ref": {}, "workflow": []}
            )
            assert (
                rejected.isError
                and rejected.structuredContent["error"]["code"] == "FORBIDDEN"
            )
            cancelling = await session.call_tool(
                "cancel_operation", {"operation_id": "fixture-op"}
            )
            assert cancelling.structuredContent["status"] == "cancelling"
            final = await session.call_tool(
                "get_operation", {"operation_id": "fixture-op"}
            )
            assert final.structuredContent["status"] == "cancelled"
    assert all(
        token in ("Bearer " + ALICE_FLOW, "Bearer " + BOB_FLOW) for token, _, _ in calls
    )
    assert all(
        path in ("/data-access/tools", "/data-access/call") for _, path, _ in calls
    )


def test_transport_and_credential_boundaries(adapter):
    url, _, _, config_file = adapter
    headers = {
        "Authorization": "Bearer " + ALICE_MCP,
        "Accept": "application/json,text/event-stream",
    }
    with httpx.Client(trust_env=False) as client:
        assert client.post(url, json={}).status_code == 401
        assert (
            client.post(
                url, headers={"Authorization": "Bearer " + ALICE_FLOW}, json={}
            ).status_code
            == 401
        )
        assert (
            client.post(
                url, headers=dict(headers, Origin="https://untrusted.example"), json={}
            ).status_code
            == 403
        )
        assert (
            client.post(
                url, headers=dict(headers, Host="untrusted.example"), json={}
            ).status_code
            == 421
        )
        assert (
            client.post(
                url, headers=headers, content=b"x" * (1024 * 1024 + 1)
            ).status_code
            == 413
        )
    config = json.loads(config_file.read_text())
    config["bindings"][0]["upstream_token_env"] = "TEST_MCP_ALICE"
    config_file.write_text(json.dumps(config))
    with pytest.raises(ConfigurationError, match="reused audience"):
        McpConfig.load(str(config_file))


@pytest.mark.anyio
async def test_upstream_failure_is_explicit(adapter, upstream):
    url, _, _, _ = adapter
    upstream[2].shutdown()
    upstream[2].server_close()
    async with (
        httpx.AsyncClient(
            headers={"Authorization": "Bearer " + ALICE_MCP}, trust_env=False
        ) as client,
        streamable_http_client(url, http_client=client) as (read, write, _),
        ClientSession(read, write) as session,
    ):
        await session.initialize()
        result = await session.call_tool("list_sources", {})
        assert (
            result.isError
            and result.structuredContent["error"]["code"] == "UPSTREAM_UNAVAILABLE"
        )


@pytest.mark.anyio
async def test_real_flowsql_query_snapshot_and_mcp_restart(
    adapter, tmp_path, monkeypatch
):
    """No NPM, Worker, model, Web, Router or Agent; only real framework plugins."""
    root = Path(__file__).parents[3]
    output = Path(
        os.environ.get("FLOWSQL_TEST_OUTPUT_DIR", root / "build/output")
    ).resolve()
    assert (output / "flowsql").is_file(), (
        "build flowsql and the data-access/database/catalog plugins first"
    )
    url, fake_calls, original, config_file = adapter
    original.terminate()
    original.wait(timeout=5)
    fake_count = len(fake_calls)
    frames = tmp_path / "frames"
    frames.mkdir()
    (frames / "orders.csv").write_text("amount,day\n10.5,2026-11-01\n20.5,2026-11-02\n")
    database = tmp_path / "business.db"
    with sqlite3.connect(database) as connection:
        connection.executescript(
            "CREATE TABLE sales(id INTEGER,amount REAL,day TEXT);"
            "INSERT INTO sales VALUES(1,8.75,'2026-11-01'),(2,2.25,'2026-11-02'),(9223372036854775807,NULL,NULL);"
            "CREATE VIEW sales_view AS SELECT * FROM sales;"
        )
    flow_port = free_port()
    flow_url = f"http://127.0.0.1:{flow_port}"
    grants = [
        {
            "locator": {
                "kind": "database_table",
                "type": "sqlite",
                "name": "business",
                "schema": "main",
                "table": table,
            },
            "operations": ["query"],
        }
        for table in ("sales", "sales_view")
    ]
    grants.append(
        {"locator": {"kind": "dataframe", "name": "orders"}, "operations": ["query"]}
    )
    policy = tmp_path / "policy.json"
    policy.write_text(
        json.dumps(
            {
                "subjects": [
                    {
                        "principal": "alice",
                        "token_env": "TEST_FLOW_ALICE",
                        "sources": grants,
                    },
                    {"principal": "bob", "token_env": "TEST_FLOW_BOB", "sources": []},
                ]
            }
        )
    )
    plugins = [
        {
            "name": str(output / "libflowsql_config_channel.so"),
            "option": f"db_path={tmp_path}/config.db",
        },
        {"name": str(output / "libflowsql_builtin.so")},
        {
            "name": str(output / "libflowsql_database.so"),
            "option": f"type=sqlite;name=business;path={database}",
        },
        {
            "name": str(output / "libflowsql_catalog.so"),
            "option": f"data_dir={frames};operator_db_path={tmp_path}/catalog.db",
        },
        {"name": str(output / "libflowsql_data_access.so")},
    ]
    service_option = json.dumps(
        {
            "host": "127.0.0.1",
            "port": flow_port,
            "policy_file": str(policy),
            "contract_file": str(root / "config/agent-data-access/api.schema.json"),
            "state_path": str(tmp_path / "access.db"),
        }
    )
    deployment = tmp_path / "flowsql.yaml"
    deployment.write_text(
        json.dumps(
            {
                "services": [
                    {"name": "all", "option": service_option, "plugins": plugins}
                ]
            }
        )
    )
    environment = dict(os.environ, LD_LIBRARY_PATH=str(output))
    environment.pop("PYTHONPATH", None)
    log = (tmp_path / "flow.log").open("w+")
    flow = await asyncio.to_thread(
        subprocess.Popen,
        [str(output / "flowsql"), "--config", str(deployment)],
        cwd=tmp_path,
        env=environment,
        stdout=log,
        stderr=log,
    )
    child = None
    mcp_log = (tmp_path / "real-mcp.log").open("w+")

    def start_adapter():
        process = subprocess.Popen(
            [sys.executable, "-m", "flowsql_mcp", "--config", str(config_file)],
            cwd=tmp_path,
            env=environment,
            stdout=mcp_log,
            stderr=mcp_log,
        )
        deadline = time.monotonic() + 12
        with httpx.Client(trust_env=False) as client:
            while time.monotonic() < deadline and process.poll() is None:
                try:
                    if (
                        client.get(url.replace("/mcp", "/ready"), timeout=1).status_code
                        == 200
                    ):
                        return process
                except httpx.HTTPError:
                    pass
                time.sleep(0.05)
        process.terminate()
        process.wait(timeout=5)
        mcp_log.seek(0)
        log.seek(0)
        pytest.fail(
            "real runtime startup failed: "
            + mcp_log.read()[-3000:]
            + log.read()[-3000:]
        )

    try:
        config = json.loads(config_file.read_text())
        config["upstream_url"] = flow_url
        config_file.write_text(json.dumps(config))
        child = start_adapter()

        async def session_call(session, name, arguments):
            result = await session.call_tool(name, arguments)
            assert not result.isError, result
            return result.structuredContent

        async def wait_operation(session, operation_id):
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                state = await session_call(
                    session, "get_operation", {"operation_id": operation_id}
                )
                if state["status"] not in ("queued", "running", "cancelling"):
                    assert state["status"] == "completed", state
                    return state
                await __import__("anyio").sleep(0.01)
            pytest.fail("real operation did not terminate within budget")

        async with (
            httpx.AsyncClient(
                headers={"Authorization": "Bearer " + ALICE_MCP}, trust_env=False
            ) as client,
            streamable_http_client(url, http_client=client) as (read, write, _),
            ClientSession(read, write) as session,
        ):
            await session.initialize()
            listing = await session_call(session, "list_sources", {})
            assert len(listing["sources"]) == 3
            sources = {
                item["name"] + ":" + item["locator"].get("table", ""): item
                for item in listing["sources"]
            }
            sales = sources["business:sales"]
            frame = sources["orders:"]
            assert frame["description"]["schema_state"] == "pending"
            aggregate = [
                {
                    "type": "view",
                    "query": [
                        {
                            "op": "aggregate",
                            "groupBy": [],
                            "measures": [
                                {"field": "f1", "agg": "sum", "asFieldKey": "sum"},
                                {
                                    "field": "f1",
                                    "agg": "count",
                                    "asFieldKey": "nonnull",
                                },
                                {"field": "*", "agg": "count", "asFieldKey": "all"},
                            ],
                        }
                    ],
                }
            ]
            query = {
                "request_id": "real-business",
                "source_ref": sales["source_ref"],
                "workflow": aggregate,
            }
            submitted = await session_call(session, "query", query)
            operation = await wait_operation(session, submitted["operation_id"])
            data = await session_call(
                session, "read_result", {"result_ref": operation["result_ref"]}
            )
            assert [cell["value"] for cell in data["rows"][0]] == ["11", "2", "3"]
            assert data["basis"]["source"]["source_ref"] == sales["source_ref"]
            async with httpx.AsyncClient(trust_env=False) as direct:
                response = await direct.post(
                    flow_url + "/data-access/call",
                    json={"name": "query", "arguments": query},
                    headers={"Authorization": "Bearer " + ALICE_FLOW},
                )
                assert (
                    response.status_code == 200
                    and response.json()["operation_id"] == operation["operation_id"]
                )
                bob = await direct.post(
                    flow_url + "/data-access/call",
                    json={
                        "name": "get_operation",
                        "arguments": {"operation_id": operation["operation_id"]},
                    },
                    headers={"Authorization": "Bearer " + BOB_FLOW},
                )
                assert bob.status_code == 404
                bypass = await direct.post(
                    flow_url + "/scheduler/batch/submit",
                    json={"sql_text": "DELETE FROM sales"},
                )
                assert bypass.status_code == 404
            raw = await session_call(
                session,
                "query",
                {
                    "request_id": "raw",
                    "source_ref": sales["source_ref"],
                    "workflow": [
                        {
                            "type": "view",
                            "query": [{"op": "raw", "fields": ["f0", "f1"]}],
                        }
                    ],
                },
            )
            raw = await wait_operation(session, raw["operation_id"])
            exact = await session_call(
                session, "read_result", {"result_ref": raw["result_ref"]}
            )
            assert exact["rows"][2][0] == {
                "type": "int64",
                "value": "9223372036854775807",
            }
            assert exact["rows"][2][1] is None
            frame_request = {
                "request_id": "snapshot",
                "source_ref": frame["source_ref"],
                "capture_snapshot": True,
                "workflow": [
                    {
                        "type": "view",
                        "query": [
                            {
                                "op": "aggregate",
                                "groupBy": [],
                                "measures": [
                                    {"field": "f0", "agg": "sum", "asFieldKey": "sum"}
                                ],
                            }
                        ],
                    }
                ],
            }
            snapshot = await session_call(session, "query", frame_request)
            snapshot = await wait_operation(session, snapshot["operation_id"])
            frame_data = await session_call(
                session, "read_result", {"result_ref": snapshot["result_ref"]}
            )
            assert float(frame_data["rows"][0][0]["value"]) == 31
            assert frame_data["basis"]["snapshot_id"]
            assert not frame_data["basis"]["numeric_display"]
        child.terminate()
        child.wait(timeout=5)
        assert flow.poll() is None
        child = start_adapter()
        async with (
            httpx.AsyncClient(
                headers={"Authorization": "Bearer " + ALICE_MCP}, trust_env=False
            ) as client,
            streamable_http_client(url, http_client=client) as (read, write, _),
            ClientSession(read, write) as session,
        ):
            await session.initialize()
            reread = await session_call(
                session, "read_result", {"result_ref": operation["result_ref"]}
            )
            assert reread["content_sha256"] == data["content_sha256"]
            retry = await session_call(session, "query", query)
            assert retry["operation_id"] == operation["operation_id"]
        assert len(fake_calls) == fake_count, (
            "real data verification must never use the authority fixture"
        )
    finally:
        if child is not None:
            child.terminate()
            child.wait(timeout=5)
        flow.terminate()
        flow.wait(timeout=8)
        log.close()
        mcp_log.close()
