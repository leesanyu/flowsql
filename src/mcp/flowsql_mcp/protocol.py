"""Business result mapping; wire protocol and transport belong to the MCP SDK."""

import json

from mcp.types import CallToolResult, TextContent


def tool_result(value: dict, *, error: bool = False) -> CallToolResult:
    return CallToolResult(
        content=[TextContent(type="text", text=json.dumps(value, ensure_ascii=False))],
        structuredContent=value,
        isError=error,
    )
