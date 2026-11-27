"""Independent MCP service. No SQL, task ledger, result store or Worker imports."""

from __future__ import annotations

import hmac
from contextlib import asynccontextmanager
from urllib.parse import urlsplit

import anyio
import httpx
import uvicorn
from mcp.server.auth.middleware.auth_context import (
    AuthContextMiddleware,
    get_access_token,
)
from mcp.server.auth.middleware.bearer_auth import (
    BearerAuthBackend,
    RequireAuthMiddleware,
)
from mcp.server.auth.provider import AccessToken
from mcp.server.lowlevel import Server
from mcp.server.streamable_http_manager import StreamableHTTPSessionManager
from mcp.server.transport_security import TransportSecuritySettings
from mcp.shared.exceptions import McpError
from mcp.types import CallToolRequest, ErrorData, ServerResult, Tool
from pydantic import AnyHttpUrl
from starlette.applications import Starlette
from starlette.middleware.authentication import AuthenticationMiddleware
from starlette.responses import JSONResponse
from starlette.routing import Route

from .backend import BackendError, FlowSqlBackend
from .config import McpConfig
from .protocol import tool_result


class DeploymentTokens:
    def __init__(self, config: McpConfig):
        self.config = config

    async def verify_token(self, token: str) -> AccessToken | None:
        if not token.isascii() or len(token) > 256:
            return None
        for binding in self.config.bindings:
            if hmac.compare_digest(token, binding.token):
                return AccessToken(
                    token=token,
                    client_id=binding.principal,
                    subject=binding.principal,
                    scopes=["data:access"],
                    resource=self.config.public_url,
                )
        return None


def create_app(config: McpConfig) -> Starlette:
    server = Server("flowsql-data-access")
    client = httpx.AsyncClient(
        base_url=config.upstream_url,
        timeout=httpx.Timeout(35, connect=3),
        limits=httpx.Limits(max_connections=20),
        follow_redirects=False,
        trust_env=False,
    )
    backend = FlowSqlBackend(client)
    bindings = {binding.principal: binding for binding in config.bindings}

    def credential() -> str:
        identity = get_access_token()
        assert identity is not None
        return bindings[identity.client_id].upstream_token

    @server.list_tools()
    async def tools() -> list[Tool]:
        # The HTTP service is the authority for both schemas and deployed modules.
        try:
            response = await backend.request(credential())
        except BackendError as error:
            raise McpError(
                ErrorData(code=-32000, message=error.code, data=error.payload())
            ) from error
        return [Tool.model_validate(item) for item in response["tools"]]

    async def call(request: CallToolRequest) -> ServerResult:
        try:
            result = tool_result(
                await backend.request(
                    credential(), request.params.name, request.params.arguments
                )
            )
        except BackendError as error:
            result = tool_result(error.payload(), error=True)
        return ServerResult(result)

    # Input validation and execution share the FlowSQL authority. Register the
    # SDK's typed handler directly so a schema-refresh failure cannot replace a
    # business error with an unstructured SDK cache error.
    server.request_handlers[CallToolRequest] = call

    transport = StreamableHTTPSessionManager(
        server,
        json_response=True,
        stateless=True,
        security_settings=TransportSecuritySettings(
            allowed_hosts=[urlsplit(config.public_url).netloc],
            allowed_origins=list(config.allowed_origins),
        ),
        max_request_body_size=1024 * 1024,
    )
    protected = AuthenticationMiddleware(
        AuthContextMiddleware(
            RequireAuthMiddleware(
                transport.handle_request, required_scopes=["data:access"]
            )
        ),
        backend=BearerAuthBackend(
            DeploymentTokens(config), resource_server_url=AnyHttpUrl(config.public_url)
        ),
    )

    async def health(request):
        return JSONResponse({"status": "ok"})

    async def ready(request):
        try:
            with anyio.fail_after(5):
                for binding in config.bindings:
                    await backend.request(binding.upstream_token)
            return JSONResponse({"status": "ready"})
        except (BackendError, TimeoutError):
            return JSONResponse({"status": "upstream_unavailable"}, status_code=503)

    @asynccontextmanager
    async def lifespan(app):
        async with client, transport.run():
            yield

    return Starlette(
        routes=[
            Route("/health", health),
            Route("/ready", ready),
            Route("/mcp", protected, methods=["GET", "POST", "DELETE"]),
        ],
        lifespan=lifespan,
    )


def main():
    config = McpConfig.from_args()
    uvicorn.run(
        create_app(config),
        host=config.host,
        port=config.port,
        log_level="warning",
        access_log=False,
    )
