"""Only the public controlled HTTP contract crosses this process boundary."""

from __future__ import annotations

import json
from typing import Any

import httpx


class BackendError(RuntimeError):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code

    def payload(self) -> dict:
        return {"error": {"code": self.code, "message": str(self)}}


class FlowSqlBackend:
    def __init__(self, client: httpx.AsyncClient):
        self.client = client

    async def request(
        self,
        token: str,
        tool: str | None = None,
        arguments: dict[str, Any] | None = None,
    ) -> dict:
        path = "/data-access/tools" if tool is None else "/data-access/call"
        body = None if tool is None else {"name": tool, "arguments": arguments or {}}
        try:
            async with self.client.stream(
                "GET" if tool is None else "POST",
                path,
                json=body,
                headers={
                    "Authorization": "Bearer " + token,
                    "Accept": "application/json",
                },
            ) as response:
                content = bytearray()
                async for chunk in response.aiter_bytes():
                    content.extend(chunk)
                    if len(content) > 1024 * 1024:
                        raise BackendError(
                            "UPSTREAM_RESPONSE_LIMIT", "FlowSQL response exceeds 1 MiB"
                        )
                try:
                    result = json.loads(content)
                except (ValueError, UnicodeDecodeError) as error:
                    raise BackendError(
                        "UPSTREAM_INVALID_RESPONSE", "FlowSQL returned invalid JSON"
                    ) from error
                if not isinstance(result, dict):
                    raise BackendError(
                        "UPSTREAM_INVALID_RESPONSE",
                        "FlowSQL returned a non-object response",
                    )
                if response.status_code >= 400:
                    issue = result.get("error", {})
                    if (
                        not isinstance(issue, dict)
                        or not isinstance(issue.get("code"), str)
                        or not isinstance(issue.get("message"), str)
                    ):
                        raise BackendError(
                            "UPSTREAM_UNAVAILABLE",
                            "FlowSQL controlled endpoint is unavailable",
                        )
                    raise BackendError(issue["code"], issue["message"])
                return result
        except httpx.TimeoutException as error:
            raise BackendError(
                "UPSTREAM_TIMEOUT",
                "FlowSQL HTTP request timed out; check operation by request_id",
            ) from error
        except httpx.HTTPError as error:
            raise BackendError(
                "UPSTREAM_UNAVAILABLE", "FlowSQL HTTP service is unavailable"
            ) from error
