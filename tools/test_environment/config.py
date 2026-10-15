# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Runtime contract shared by environment preparation, probes and test runners."""
import json
import os
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEFAULT_CONFIG = REPO / "config/testing/environment.json"
STATE = Path(os.environ.get("FLOWSQL_TEST_STATE", f"/tmp/flowsql-test-{os.getuid()}"))
BACKENDS = ("mysql", "postgres", "clickhouse")
PROFILES = ("PLAINTEXT", "SSL", "MTLS", "PLAIN", "SCRAM-SHA-256", "SCRAM-SHA-512", "GSSAPI", "OAUTHBEARER")


class EnvironmentError(RuntimeError):
    def __init__(self, category, stage, backend="", detail=""):
        self.category, self.stage, self.backend, self.detail = category, stage, backend, detail
        super().__init__(f"{category}: {stage}" + (f" ({backend})" if backend else "") + (f": {detail}" if detail else ""))

    def result(self):
        return {"category": self.category, "stage": self.stage, "backend": self.backend, "detail": self.detail}


def object_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise EnvironmentError("config_invalid", "parse", detail="duplicate JSON field")
        result[key] = value
    return result


def fields(value, expected, location):
    if not isinstance(value, dict) or set(value) != set(expected):
        raise EnvironmentError("config_invalid", "schema", detail=f"invalid fields at {location}")


def identifier(value, location):
    if not isinstance(value, str) or not re.fullmatch(r"[a-z][a-z0-9_]{0,62}", value):
        raise EnvironmentError("config_invalid", "schema", detail=f"invalid identifier at {location}")


class Config:
    def __init__(self, path=None):
        self.path = Path(path or os.environ.get("FLOWSQL_TEST_CONFIG", DEFAULT_CONFIG)).resolve()
        try:
            self.data = json.loads(self.path.read_text(), object_pairs_hook=object_pairs)
        except FileNotFoundError:
            raise EnvironmentError("config_missing", "load") from None
        except (OSError, ValueError):
            raise EnvironmentError("config_invalid", "parse") from None
        d = self.data
        fields(d, ("schema_version", "project", "managed", "budgets", "images", "databases", "kafka"), "root")
        if type(d["schema_version"]) is not int or d["schema_version"] != 1 or type(d["managed"]) is not bool:
            raise EnvironmentError("config_invalid", "schema", detail="version or managed")
        identifier(d["project"], "project")
        fields(d["budgets"], ("preparation_seconds", "connection_seconds"), "budgets")
        for key, value in d["budgets"].items():
            if type(value) is not int or not 1 <= value <= (600 if key == "preparation_seconds" else 3):
                raise EnvironmentError("config_invalid", "schema", detail="invalid budget")
        fields(d["images"], (*BACKENDS, "runtime", "harness"), "images")
        for key, value in d["images"].items():
            if not isinstance(value, str) or not value or value.endswith(":latest"):
                raise EnvironmentError("config_invalid", "schema", detail="unpinned image")
        fields(d["databases"], BACKENDS, "databases")
        for name, db in d["databases"].items():
            fields(db, ("user", "password_ref", "endpoints", "roles"), name)
            identifier(db["user"], name + ".user")
            self.validate_reference(db["password_ref"])
            fields(db["endpoints"], ("native", "docker"), name + ".endpoints")
            for endpoint in db["endpoints"].values():
                fields(endpoint, ("host", "port"), "endpoint")
                if (not isinstance(endpoint["host"], str) or not endpoint["host"] or
                        any(c in endpoint["host"] for c in ";|\n") or type(endpoint["port"]) is not int or
                        not 1 <= endpoint["port"] <= 65535):
                    raise EnvironmentError("config_invalid", "schema", name, "invalid endpoint")
            fields(db["roles"], ("driver", "npm", "source", "target", "production"), name + ".roles")
            for value in db["roles"].values():
                identifier(value, name + ".role")
        kafka = d["kafka"]
        fields(kafka, ("version", "distribution_url", "distribution_sha512", "hosts", "profiles", "password_ref",
                       "topic", "second_topic", "group", "realm", "service_name"), "kafka")
        if kafka["version"] != "4.1.0" or not re.fullmatch(r"[0-9a-f]{128}", kafka["distribution_sha512"]):
            raise EnvironmentError("config_invalid", "schema", "kafka", "version/checksum")
        if not kafka["distribution_url"].startswith("https://"):
            raise EnvironmentError("config_invalid", "schema", "kafka", "distribution URL")
        fields(kafka["hosts"], ("native", "docker"), "kafka.hosts")
        for host in kafka["hosts"].values():
            if not isinstance(host, str) or not re.fullmatch(r"[a-zA-Z0-9.-]+", host):
                raise EnvironmentError("config_invalid", "schema", "kafka", "hostname")
        if kafka["profiles"] != list(PROFILES) or kafka["realm"] != "FLOWSQL.TEST" or kafka["service_name"] != "kafka":
            raise EnvironmentError("config_invalid", "schema", "kafka", "authentication profiles")
        self.validate_reference(kafka["password_ref"])
        for key in ("topic", "second_topic", "group"):
            identifier(kafka[key], "kafka." + key)
        if kafka["topic"] == kafka["second_topic"]:
            raise EnvironmentError("config_invalid", "schema", "kafka", "Topic identities must differ")

    @staticmethod
    def validate_reference(reference):
        if not isinstance(reference, str) or not re.fullmatch(r"(file|env):[^\r\n]+", reference):
            raise EnvironmentError("config_invalid", "schema", detail="credential reference")

    def secret(self, reference):
        kind, value = reference.split(":", 1)
        if kind == "env":
            secret = os.environ.get(value)
            if secret is None:
                raise EnvironmentError("config_missing", "credential")
        else:
            path = self.secret_path(reference)
            try:
                if path.stat().st_mode & 0o077:
                    raise EnvironmentError("permission_denied", "credential", detail="private file permissions required")
                if path.stat().st_size > 65536:
                    raise EnvironmentError("config_invalid", "credential", detail="credential too large")
                secret = path.read_text().strip()
            except OSError:
                raise EnvironmentError("config_missing", "credential") from None
        if not secret or any(c in secret for c in ";|\r\n"):
            raise EnvironmentError("config_invalid", "credential", detail="empty or unsupported credential encoding")
        return secret

    def secret_path(self, reference):
        value = reference.split(":", 1)[1]
        return ((STATE / value[7:]) if value.startswith("@state/") else (self.path.parent / value)).resolve()

    def database(self, backend, role, location):
        db = self.data["databases"][backend]
        return dict(db["endpoints"][location], user=db["user"], password=self.secret(db["password_ref"]),
                    database=db["roles"][role])


def dependencies(path=None):
    try:
        data = json.loads(Path(path or REPO / "config/testing/dependencies.json").read_text(), object_pairs_hook=object_pairs)
    except (OSError, ValueError):
        raise EnvironmentError("config_invalid", "dependencies") from None
    fields(data, ("schema_version", "tests"), "dependencies")
    if data["schema_version"] != 1 or not isinstance(data["tests"], dict):
        raise EnvironmentError("config_invalid", "dependencies")
    for name, dependency in data["tests"].items():
        fields(dependency, ("services", "role", "location", "resources"), name)
        if (not isinstance(dependency["services"], list) or len(set(dependency["services"])) != len(dependency["services"]) or
                not set(dependency["services"]) <= set((*BACKENDS, "kafka")) or
                dependency["role"] not in ("driver", "npm", "source", "target", "production", "auth") or
                dependency["location"] not in ("native", "docker", "both") or not isinstance(dependency["resources"], list)):
            raise EnvironmentError("config_invalid", "dependencies", name)
    result=dict(data["tests"])
    # Setup tests are derived from the same declarations; they have no second connection contract.
    for name, declaration in data["tests"].items():
        if "kafka" not in declaration["services"]:
            result[name+"__environment"]=declaration
    return result
