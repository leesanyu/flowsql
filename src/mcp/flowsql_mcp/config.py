"""Deployment-owned credential bindings; business policy stays in FlowSQL."""

from __future__ import annotations

import argparse
import json
import os
import re
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import urlsplit


class ConfigurationError(ValueError):
    pass


@dataclass(frozen=True)
class Binding:
    principal: str
    token: str
    upstream_token: str


@dataclass(frozen=True)
class McpConfig:
    host: str
    port: int
    public_url: str
    upstream_url: str
    bindings: tuple[Binding, ...]
    allowed_origins: tuple[str, ...] = ()

    @classmethod
    def load(cls, path: str) -> McpConfig:
        value = json.loads(Path(path).read_text())
        allowed = {
            "host",
            "port",
            "public_url",
            "upstream_url",
            "bindings",
            "allowed_origins",
        }
        if not isinstance(value, dict) or set(value) - allowed:
            raise ConfigurationError("unknown MCP configuration property")
        for key in ("public_url", "upstream_url"):
            url = urlsplit(value.get(key, ""))
            if (
                url.scheme not in ("http", "https")
                or not url.hostname
                or url.username
                or url.password
                or url.query
                or url.fragment
            ):
                raise ConfigurationError(
                    f"{key} must be an HTTP URL without credentials, query or fragment"
                )
        if urlsplit(value["public_url"]).path != "/mcp":
            raise ConfigurationError(
                "public_url must identify this service's /mcp endpoint"
            )
        port = value.get("port", 18804)
        if type(port) is not int or not 1 <= port <= 65535:
            raise ConfigurationError("port must be between 1 and 65535")
        bindings = []
        tokens, principals = set(), set()
        entries = value.get("bindings")
        if not isinstance(entries, list) or not 1 <= len(entries) <= 64:
            raise ConfigurationError(
                "one to 64 explicit identity bindings are required"
            )
        for item in entries:
            if not isinstance(item, dict) or set(item) != {
                "principal",
                "token_env",
                "upstream_token_env",
            }:
                raise ConfigurationError(
                    "binding requires principal, token_env and upstream_token_env"
                )
            principal = item["principal"]
            if (
                not isinstance(principal, str)
                or not principal
                or len(principal) > 128
                or principal in principals
            ):
                raise ConfigurationError("invalid or duplicate principal")
            credentials = []
            for key in ("token_env", "upstream_token_env"):
                name = item[key]
                if not isinstance(name, str) or not re.fullmatch(
                    r"[A-Za-z_][A-Za-z0-9_]*", name
                ):
                    raise ConfigurationError(
                        "credential must reference an environment variable"
                    )
                token = os.environ.get(name, "")
                if (
                    not 32 <= len(token) <= 256
                    or token in tokens
                    or any(ord(c) < 33 or ord(c) > 126 for c in token)
                ):
                    raise ConfigurationError(
                        "missing, short, invalid or reused audience credential"
                    )
                tokens.add(token)
                credentials.append(token)
            principals.add(principal)
            bindings.append(Binding(principal, *credentials))
        origins = value.get("allowed_origins", [])
        if not isinstance(origins, list) or any(
            not isinstance(origin, str)
            or urlsplit(origin).scheme not in ("https", "http")
            or not urlsplit(origin).netloc
            or urlsplit(origin).path
            or urlsplit(origin).query
            or urlsplit(origin).fragment
            for origin in origins
        ):
            raise ConfigurationError("allowed_origins must be exact HTTP origins")
        host = value.get("host", "127.0.0.1")
        if not isinstance(host, str) or not host or len(host) > 255:
            raise ConfigurationError("invalid listening host")
        return cls(
            host,
            port,
            value["public_url"],
            value["upstream_url"].rstrip("/"),
            tuple(bindings),
            tuple(origins),
        )

    @classmethod
    def from_args(cls) -> McpConfig:
        parser = argparse.ArgumentParser(
            description="FlowSQL independent MCP HTTP adapter"
        )
        parser.add_argument("--config", default=os.environ.get("FLOWSQL_MCP_CONFIG"))
        args = parser.parse_args()
        if not args.config:
            parser.error("--config or FLOWSQL_MCP_CONFIG is required")
        return cls.load(args.config)
