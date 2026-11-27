# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Official client, independent of framework/Worker imports."""

import asyncio
import json
import os
import sys

import httpx
from mcp.client.streamable_http import streamable_http_client

from mcp import ClientSession


async def main():
    url, operation, reference = sys.argv[1:]
    async with (
        httpx.AsyncClient(
            headers={"Authorization": "Bearer " + os.environ["ACCESS_MCP_TOKEN"]},
            trust_env=False,
        ) as client,
        streamable_http_client(url, http_client=client) as (read, write, _),
        ClientSession(read, write) as session,
    ):
        await session.initialize()
        tools = await session.list_tools()
        assert {"submit_analysis", "preflight_analysis", "read_result"} <= {
            t.name for t in tools.tools
        }
        state = await session.call_tool("get_operation", {"operation_id": operation})
        assert not state.isError and state.structuredContent["status"] == "completed", (
            state
        )
        assert state.structuredContent["result_ref"] == reference
        result = await session.call_tool("read_result", {"result_ref": reference})
        assert not result.isError, result
        print(
            json.dumps(
                {
                    "operation_id": operation,
                    "result_ref": reference,
                    "content_sha256": result.structuredContent["content_sha256"],
                }
            )
        )


asyncio.run(main())
