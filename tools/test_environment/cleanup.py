# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Explicit Docker cleanup and temporary-project lifetime, guarded by ownership."""
import contextlib
import fcntl
import json

from .config import EnvironmentError, STATE, identifier
from .fixture import command

OWNER = "flowsql.test.project"


@contextlib.contextmanager
def project_activity(project, *, exclusive=False):
    identifier(project, "cleanup.project")
    STATE.mkdir(parents=True, exist_ok=True, mode=0o700)
    STATE.chmod(0o700)
    # Separate from preparation/resource locks: shared readers include CTest children.
    with (STATE / (project + ".activity.lock")).open("a") as handle:
        try:
            fcntl.flock(handle, (fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH) | fcntl.LOCK_NB)
        except BlockingIOError:
            raise EnvironmentError("resource_busy", "cleanup", detail="test project is active") from None
        yield


def inspect_resources(kind):
    # Inspect only public fields. Container Env and private Compose files contain secrets.
    formats = {
        "containers": ('{"id":{{json .Id}},"name":{{json .Name}},"state":{{json .State.Status}},'
                       '"labels":{{json .Config.Labels}},"mounts":{{json .Mounts}}}'),
        "volumes": '{"name":{{json .Name}},"labels":{{json .Labels}}}',
        "networks": ('{"id":{{json .Id}},"name":{{json .Name}},"labels":{{json .Labels}},'
                     '"containers":{{json .Containers}}}'),
    }
    listing = {"containers": ["ps", "-aq"], "volumes": ["volume", "ls", "-q"],
               "networks": ["network", "ls", "-q"]}[kind]
    identities = command(["docker", *listing]).splitlines()
    if not identities:
        return []
    inspect = [] if kind == "containers" else [kind[:-1]]
    output = command(["docker", *inspect, "inspect", "--format", formats[kind], *identities])
    return [json.loads(line) for line in output.splitlines() if line]


def project_resources(project):
    identifier(project, "cleanup.project")
    all_containers = inspect_resources("containers")
    resources = {"containers": all_containers, "volumes": inspect_resources("volumes"),
                 "networks": inspect_resources("networks")}
    owned = {}
    for kind, items in resources.items():
        owned[kind] = []
        for item in items:
            labels = item.get("labels") or {}
            if labels.get(OWNER) != project:
                continue
            compose = labels.get("com.docker.compose.project")
            if compose and compose != project.replace("_", "-"):
                raise EnvironmentError("config_invalid", "cleanup_ownership", detail="conflicting Compose project")
            owned[kind].append(item)
    container_ids = {item["id"] for item in owned["containers"]}
    volume_names = {item["name"] for item in owned["volumes"]}
    for item in all_containers:
        if item["id"] not in container_ids and any(
                mount.get("Type") == "volume" and mount.get("Name") in volume_names
                for mount in item.get("mounts", [])):
            raise EnvironmentError("resource_busy", "cleanup_references", detail="volume referenced by another project")
    for item in owned["networks"]:
        if set(item.get("containers") or {}) - container_ids:
            raise EnvironmentError("resource_busy", "cleanup_references", detail="network referenced by another project")
    return {"containers": sorted(container_ids), "volumes": sorted(volume_names),
            "networks": sorted(item["id"] for item in owned["networks"])}


def cleanup_project(project, *, apply=False):
    """Preview by default; apply only to this exact labelled Docker project."""
    with project_activity(project, exclusive=True):
        plan = project_resources(project)
        if apply:
            for kind, prefix in (("containers", ["rm", "-f"]), ("volumes", ["volume", "rm"]),
                                 ("networks", ["network", "rm"])):
                if plan[kind]:
                    command(["docker", *prefix, "--", *plan[kind]])
            remaining = project_resources(project)
            if any(remaining.values()):
                raise EnvironmentError("service_unavailable", "cleanup_remaining", detail="owned resources remain")
        return {"project": project, "applied": apply, **plan}


@contextlib.contextmanager
def temporary_project(project):
    """Enter before preparation so partial preparation failures also reclaim resources."""
    try:
        with project_activity(project):
            yield
    finally:
        cleanup_project(project, apply=True)
