# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Only owned resources are prepared; user containers and databases are never adopted."""
import html
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import socket
import subprocess
import tarfile
import time
import urllib.request

from .config import BACKENDS, Config, EnvironmentError, REPO, STATE


def prepare_state():
    try:
        STATE.mkdir(parents=True, exist_ok=True, mode=0o700)
        STATE.chmod(0o700)
    except OSError:
        raise EnvironmentError("permission_denied", "state", detail="private state directory required") from None


def private_write(path, text):
    path = Path(path)
    if STATE in path.parents:
        prepare_state()
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(descriptor, "w") as output:
        output.write(text)
    path.chmod(0o600)


def command(args, *, timeout=60, input=None, env=None):
    try:
        result = subprocess.run(args, input=input, env=env, text=True, capture_output=True, timeout=timeout)
    except PermissionError:
        raise EnvironmentError("execution_denied", "command") from None
    except FileNotFoundError:
        raise EnvironmentError("artifact_missing", "command", detail=Path(args[0]).name) from None
    except subprocess.TimeoutExpired:
        raise EnvironmentError("preparation_timeout", "command") from None
    if result.returncode:
        private_write(STATE / "last-command.log", result.stdout + result.stderr)
        if any(word in (result.stdout+result.stderr).lower() for word in ("access denied for user", "authentication failed", "password authentication failed")):
            raise EnvironmentError("authentication_failed", "initialize")
        denied = "permission denied" in result.stderr.lower() or "operation not permitted" in result.stderr.lower()
        raise EnvironmentError("execution_denied" if denied else "service_unavailable", "command",
                               detail=Path(args[0]).name + " failed; inspect private fixture logs")
    return result.stdout


@contextlib.contextmanager
def lock(name):
    prepare_state()
    with (STATE / (name + ".lock")).open("w") as handle:
        fcntl.flock(handle, fcntl.LOCK_EX)
        yield


def initialize_credentials(config):
    if not config.data["managed"]:
        return
    references = [db["password_ref"] for db in config.data["databases"].values()]
    references.append(config.data["kafka"]["password_ref"])
    for reference in references:
        kind, value = reference.split(":", 1)
        if kind == "file":
            path = config.secret_path(reference)
            # Never write caller-owned secret references outside our state tree.
            if STATE not in path.parents:
                config.secret(reference)
            elif not path.exists():
                private_write(path, secrets.token_hex(24))
        config.secret(reference)


def fingerprint(config):
    # Persist only public configuration identity. Secret equality is verified through authentication.
    data=json.loads(json.dumps(config.data))
    for item in [*data["databases"].values(),data["kafka"]]:
        if item["password_ref"].startswith("file:"):
            item["password_ref"]="file:"+str(config.secret_path(item["password_ref"]))
    return hashlib.sha256(json.dumps(data, sort_keys=True).encode()).hexdigest()


def compose_document(config, publish=True):
    d = config.data
    project = d["project"].replace("_", "-")
    services = {}
    root = STATE / d["project"]
    for backend in BACKENDS:
        db = d["databases"][backend]
        password = config.secret(db["password_ref"])
        endpoint = db["endpoints"]["native"]
        internal = db["endpoints"]["docker"]["port"]
        environment = {}
        service = {"image": d["images"][backend], "labels": {"flowsql.test.project": d["project"]},
                   "ports": [f"{endpoint['host']}:{endpoint['port']}:{internal}"], "environment": environment,
                   "volumes": [f"{backend}:/var/lib/" + {"mysql": "mysql", "postgres": "postgresql/data",
                                                        "clickhouse": "clickhouse"}[backend]],
                   "networks": {"test": {"aliases": [db["endpoints"]["docker"]["host"]]}}}
        if not publish:service.pop("ports")
        if backend == "mysql":
            environment.update(MYSQL_ROOT_PASSWORD=password, MYSQL_USER=db["user"], MYSQL_PASSWORD=password,
                               MYSQL_DATABASE=db["roles"]["driver"])
            sql = "\n".join(f"CREATE DATABASE IF NOT EXISTS `{name}`; GRANT ALL ON `{name}`.* TO '{db['user']}'@'%';"
                            for name in set(db["roles"].values()))
            sql += "\nGRANT ALL ON `baseline_t73_%`.* TO '" + db["user"] + "'@'%';"
            private_write(root / "mysql-init.sql", sql)
            service["volumes"].append(str(root / "mysql-init.sql") + ":/docker-entrypoint-initdb.d/test.sql:ro")
        elif backend == "postgres":
            environment.update(POSTGRES_USER=db["user"], POSTGRES_PASSWORD=password, POSTGRES_DB=db["roles"]["driver"])
            sql = "\n".join(f'CREATE DATABASE "{name}";' for name in set(db["roles"].values())
                            if name != db["roles"]["driver"])
            private_write(root / "postgres-init.sql", sql)
            service["volumes"].append(str(root / "postgres-init.sql") + ":/docker-entrypoint-initdb.d/test.sql:ro")
        else:
            environment.update(CLICKHOUSE_USER=db["user"], CLICKHOUSE_PASSWORD=password,
                               CLICKHOUSE_DB=db["roles"]["driver"], CLICKHOUSE_DEFAULT_ACCESS_MANAGEMENT="1")
            private_write(root / "clickhouse-init.sql", "\n".join(
                f"CREATE DATABASE IF NOT EXISTS `{name}`;" for name in set(db["roles"].values())))
            private_write(root / "clickhouse-client.xml", '<config><host>127.0.0.1</host><user>' + db['user'] +
                          '</user><password>' + html.escape(password) + '</password></config>')
            service["volumes"].append(str(root / "clickhouse-client.xml") + ":/root/test-client.xml:ro")
            service["volumes"].append(str(root / "clickhouse-init.sql") + ":/docker-entrypoint-initdb.d/test.sql:ro")
            # Match existing FlowSQL supported column budget without editing production configuration.
            private_write(root / "clickhouse-profile.xml", '<clickhouse><profiles><default>'
                          '<max_block_size>65536</max_block_size><max_query_size>1048576</max_query_size>'
                          '<max_ast_elements>200000</max_ast_elements></default></profiles></clickhouse>')
            service["volumes"].append(str(root / "clickhouse-profile.xml") + ":/etc/clickhouse-server/users.d/test.xml:ro")
        for public_file in (root / (backend + "-init.sql"), root / (backend + "-profile.xml")):
            if not public_file.exists(): continue
            public_file.chmod(0o644)  # Initialization SQL/profile contain no credentials; service UID must read them.
        service["healthcheck"] = {"test": {"mysql": ["CMD", "mysqladmin", "ping", "-h", "127.0.0.1"],
                                                    "postgres": ["CMD", "pg_isready", "-U", db["user"], "-d", db["roles"]["driver"]],
                                                    "clickhouse": ["CMD", "wget", "-q", "--spider", "http://127.0.0.1:8123/ping"]}[backend],
                                  "interval": "1s", "timeout": "2s", "retries": 60}
        services[backend] = service
    return {"name": project, "services": services, "volumes": {b: {"labels":{"flowsql.test.project":d["project"]}} for b in BACKENDS},
            "networks": {"test": {"name": project + "-network", "labels": {"flowsql.test.project": d["project"]}}}}


def prepare_databases(config, services, deadline=None, location="docker"):
    deadline = deadline or time.monotonic() + config.data["budgets"]["preparation_seconds"]
    if not config.data["managed"]:
        return
    root = STATE / config.data["project"]
    root.mkdir(parents=True, exist_ok=True, mode=0o700)
    with lock(config.data["project"]):
        initialize_credentials(config)
        identity = root / "identity.json"
        current = fingerprint(config)
        if identity.exists() and json.loads(identity.read_text())["config"] != current:
            raise EnvironmentError("config_invalid", "reuse", detail="fixture configuration changed; choose a new project or explicitly reset owned resources")
        project = config.data["project"].replace("_", "-")
        for backend in services:
            existing = subprocess.run(["docker", "inspect", project+"-"+backend+"-1"], capture_output=True,text=True)
            if existing.returncode == 0 and json.loads(existing.stdout)[0]["Config"]["Labels"].get("flowsql.test.project") != config.data["project"]:
                raise EnvironmentError("config_invalid", "ownership",backend,"same-name container is not owned")
            volume = subprocess.run(["docker", "volume", "inspect", project+"_"+backend],capture_output=True,text=True)
            if volume.returncode == 0:
                labels=json.loads(volume.stdout)[0].get("Labels") or {}
                owned=labels.get("flowsql.test.project")==config.data["project"]
                known=identity.exists() and labels.get("com.docker.compose.project")==project
                if not owned and not known:
                    raise EnvironmentError("config_invalid","ownership",backend,"same-name volume is not owned")
        prepare_network(config)
        compose = root / "compose.json"
        published = location == "native"
        if compose.exists():
            published = published or any("ports" in service for service in json.loads(compose.read_text())["services"].values())
        document = compose_document(config,publish=published)
        private_write(compose, json.dumps(document))
        # A private Compose project gives different names, ports and volumes from user deployments.
        command(["docker", "compose", "-f", str(compose), "up", "-d", "--pull", "never", *services],
                timeout=max(.1,deadline-time.monotonic()))
        for backend in services:
            name = config.data["project"].replace("_", "-") + "-" + backend + "-1"
            info = json.loads(command(["docker", "inspect", name]))[0]
            if info.get("Config", {}).get("Labels", {}).get("flowsql.test.project") != config.data["project"]:
                raise EnvironmentError("config_invalid", "ownership", backend)
            if info["State"].get("Health", {}).get("Status") == "unhealthy":
                command(["docker", "restart", name])
        # Init hooks only run on an empty volume. Reconcile owned database roles after interrupted initialization.
        for backend in services:
            name = config.data["project"].replace("_", "-") + "-" + backend + "-1"
            db = config.data["databases"][backend]
            if backend == "postgres":
                sql = "\n".join(f"SELECT 'CREATE DATABASE \"{role}\"' WHERE NOT EXISTS (SELECT FROM pg_database WHERE datname='{role}')\n\\gexec"
                                for role in set(db["roles"].values()))
                cli = ["sh", "-c", 'PGPASSWORD="$POSTGRES_PASSWORD" exec psql -h127.0.0.1 -v ON_ERROR_STOP=1 -U "$POSTGRES_USER" -d "$POSTGRES_DB"']
            elif backend == "mysql":
                sql = (root / "mysql-init.sql").read_text()
                # The entrypoint's temporary initialization server has no TCP listener.
                # Wait for the final server instead of racing its initial password setup.
                cli = ["sh", "-c", 'MYSQL_PWD="$MYSQL_ROOT_PASSWORD" exec mysql -h127.0.0.1 -uroot']
            else:
                sql = (root / "clickhouse-init.sql").read_text()
                cli = ["clickhouse", "client", "--config-file", "/root/test-client.xml", "--multiquery"]
            while True:
                try:
                    command(["docker", "exec", "-i", name, *cli], input=sql, timeout=max(.1, deadline-time.monotonic()))
                    break
                except EnvironmentError as error:
                    if error.category != "service_unavailable" or time.monotonic() >= deadline: raise
                    time.sleep(.2)
        private_write(identity, json.dumps({"config": current}))


def runtime_command(config, args, *, location, env=None, mounts=None, image=None):
    if location == "native":
        return list(args)
    project = config.data["project"].replace("_", "-")
    command_line = ["docker", "run", "--rm", "--network", project + "-network",
                    "--label", "flowsql.test.project=" + config.data["project"],
                    "-v", f"{REPO}:{REPO}:ro", "-v", f"{STATE}:{STATE}",
                    "-v", f"{REPO / 'evidence/test-environment'}:{REPO / 'evidence/test-environment'}",
                    "-v", f"{REPO / 'build/output'}:/opt/flowsql/bin:ro",
                    "-w", str(REPO / "build/output")]
    variables = {name:value for name,value in (env or {}).items() if name.startswith(("FLOWSQL_TEST_", "CTEST_"))}
    variables["FLOWSQL_TEST_STATE"]=str(STATE)
    for reference in [db["password_ref"] for db in config.data["databases"].values()] + [config.data["kafka"]["password_ref"]]:
        if reference.startswith("env:"):
            name=reference[4:]
            if name in os.environ: variables[name]=os.environ[name]
    if variables:
        environment_file=STATE / ("runtime-"+secrets.token_hex(5)+".env")
        private_write(environment_file,"\n".join(name+"="+value for name,value in variables.items()))
        command_line.extend(["--env-file",str(environment_file)])
    for source, destination in (mounts or []):
        command_line.extend(["-v", str(source) + ":" + str(destination)])
    return command_line + [image or config.data["images"]["runtime"], *map(str, args)]


def probe_database(config, backend, role, location, write=False, timeout=3):
    try:
        with socket.socket():pass
    except PermissionError:
        raise EnvironmentError("execution_denied", "socket", backend) from None
    env = dict(os.environ, FLOWSQL_TEST_CONFIG=str(config.path), FLOWSQL_TEST_LOCATION=location)
    probe = REPO / "build/output/test_environment_probe"
    if not probe.exists():
        raise EnvironmentError("artifact_missing", "probe", backend, "build test_environment_probe")
    args = [str(probe), backend, role] + (["write"] if write else [])
    args = runtime_command(config, args, location=location, env=env)
    try:
        result = subprocess.run(args, env=env, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        raise EnvironmentError("network_unreachable", "connect", backend) from None
    if not result.stdout.strip():
        category = "execution_denied" if "permission denied" in result.stderr.lower() else "artifact_missing"
        raise EnvironmentError(category, "probe", backend)
    try:
        report = json.loads(result.stdout.strip().splitlines()[-1])
    except ValueError:
        raise EnvironmentError("artifact_missing", "probe", backend, "invalid probe response") from None
    if result.returncode:
        private_write(STATE / ("probe-" + backend + ".log"), result.stderr)
        raise EnvironmentError(report["category"], report["stage"], backend)
    return report


def wait_databases(config, services, location="native", deadline=None):
    deadline = deadline or time.monotonic() + config.data["budgets"]["preparation_seconds"]
    pending = set(services)
    while pending:
        for backend in tuple(pending):
            try:
                probe_database(config, backend, "driver", location, timeout=min(3, max(.1, deadline - time.monotonic())))
                pending.remove(backend)
            except EnvironmentError as error:
                if error.category not in ("network_unreachable", "service_unavailable"):
                    raise
            if time.monotonic() >= deadline:
                raise EnvironmentError("preparation_timeout", "ready", backend)
        if pending:
            time.sleep(.2)


def kafka_distribution(config):
    kafka = config.data["kafka"]
    cache = STATE / "cache"
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / "kafka_2.13-4.1.0.tgz"
    with lock("kafka_cache"):
        destination = cache / "kafka_2.13-4.1.0"
        image = "apache/kafka@sha256:bff074a5d0051dbc0bbbcd25b045bb1fe84833ec0d3c7c965d1797dd289ec88f"
        if (destination / ".verified").exists():
            if (destination / ".verified").read_text() not in (image, kafka["distribution_sha512"]):
                raise EnvironmentError("config_invalid", "kafka_distribution", detail="cache provenance mismatch")
            return destination
        # A pinned official image provides the same distribution without a slow archive download.
        inspection = subprocess.run(["docker", "image", "inspect", image], capture_output=True)
        if inspection.returncode == 0:
            name = "flowsql-test-kafka-cache-" + secrets.token_hex(4)
            command(["docker", "create", "--name", name, "--label", "flowsql.test.cache=true", image])
            try:
                destination.mkdir(exist_ok=True)
                command(["docker", "cp", name + ":/opt/kafka/.", str(destination)], timeout=60)
                if not (destination / "libs/kafka-clients-4.1.0.jar").exists():
                    raise EnvironmentError("artifact_missing", "kafka_distribution")
                private_write(destination / ".verified", image)
            finally:
                command(["docker", "rm", name])
            return destination
        if not archive.exists():
            try:
                with urllib.request.urlopen(kafka["distribution_url"], timeout=30) as response:
                    with archive.open("wb") as output:
                        shutil.copyfileobj(response, output)
            except OSError:
                if archive.exists():
                    archive.unlink()
                raise EnvironmentError("artifact_missing", "kafka_distribution", detail="download failed; provide checksum-matching cache") from None
        checksum = hashlib.sha512(archive.read_bytes()).hexdigest()
        if checksum != kafka["distribution_sha512"]:
            raise EnvironmentError("config_invalid", "kafka_distribution", detail="SHA512 mismatch")
        destination = cache / "kafka_2.13-4.1.0"
        if not (destination / ".verified").exists():
            with tarfile.open(archive) as bundle:
                for item in bundle.getmembers():
                    if item.issym() or item.islnk() or not (cache / item.name).resolve().is_relative_to(cache.resolve()):
                        raise EnvironmentError("config_invalid", "kafka_distribution", detail="invalid archive member")
                bundle.extractall(cache)
            (destination / ".verified").write_text(checksum)
    return destination


def prepare_network(config):
    name = config.data["project"].replace("_", "-") + "-network"
    result = subprocess.run(["docker", "network", "inspect", name], capture_output=True, text=True)
    if result.returncode == 0:
        existing = json.loads(result.stdout)[0]
        if existing.get("Labels", {}).get("flowsql.test.project") != config.data["project"]:
            raise EnvironmentError("config_invalid", "network", detail="network is not owned by this test project")
    else:
        command(["docker", "network", "create", "--label", "flowsql.test.project=" + config.data["project"],
                 "--label", "com.docker.compose.project=" + config.data["project"].replace("_", "-"),
                 "--label", "com.docker.compose.network=test", name])
    return name


def kafka_matrix(config, location, output):
    initialize_credentials(config)
    distribution = kafka_distribution(config)
    script = REPO / "src/tests/test_kafka_connection_auth/real_auth_matrix.py"
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True, mode=0o700)
    private_output = STATE / config.data["project"] / ("kafka-" + location + "-" + secrets.token_hex(6))
    private_output.mkdir(parents=True, mode=0o700)
    args = ["/usr/bin/python3", str(script), "--config", str(config.path), "--kafka-home", str(distribution),
            "--mode", location, "--output", str(private_output)]
    if location == "docker":
        network = prepare_network(config)
        name = config.data["project"].replace("_", "-") + "-kafka-" + secrets.token_hex(4)
        matrix_host=config.data["kafka"]["hosts"]["docker"]+"-"+secrets.token_hex(4)
        args += ["--network", network, "--host", matrix_host, "--client-image", config.data["images"]["runtime"]]
        docker = shutil.which("docker")
        if not docker:
            raise EnvironmentError("artifact_missing", "docker")
        args = ["docker", "run", "--rm", "--name", name, "--network", network, "--network-alias",
                matrix_host, "--label", "flowsql.test.project=" + config.data["project"],
                "-v", f"{REPO}:{REPO}:ro", "-v", f"{STATE}:{STATE}",
                "-v", f"{private_output}:{private_output}", "-v", "/var/run/docker.sock:/var/run/docker.sock",
                "-v", f"{Path(docker).resolve()}:/usr/local/bin/docker:ro", "-w", str(REPO),
                config.data["images"]["harness"], *args]
    with (private_output / "matrix.log").open("w") as log:
        result = subprocess.run(args, stdout=log, stderr=log, timeout=900)
    secret = config.secret(config.data["kafka"]["password_ref"])
    for log in private_output.glob("*.log"):
        if log.name != "broker.log":
            (output / log.name).write_text(log.read_text().replace(secret, "[REDACTED]"))
    if result.returncode:
        raise EnvironmentError("test_failed", "kafka_matrix", "kafka", "inspect matrix.log")
    reports = json.loads((private_output / "results.json").read_text())
    (output / "results.json").write_text(json.dumps(reports, indent=2))
    if len(reports) != 21 or any(report[-1] != "passed" for report in reports):
        raise EnvironmentError("test_failed", "kafka_matrix", "kafka", "incomplete authentication matrix")
    return {"category": "ready", "location": location, "cases": len(reports),
            "read_permission": "unverified", "write_permission": "unverified"}


def prepare_kafka(config, location, deadline=None, invalid_advertised=False):
    deadline = deadline or time.monotonic() + config.data["budgets"]["preparation_seconds"]
    manifest_override = os.environ.get("FLOWSQL_TEST_KAFKA_MANIFEST")
    if manifest_override:
        path = Path(manifest_override).resolve()
        if not path.exists(): raise EnvironmentError("config_missing", "kafka_manifest", "kafka")
        return path
    identity = STATE / config.data["project"] / ("kafka-" + location + "-identity.json")
    with lock(config.data["project"] + "-kafka-" + location):
        if identity.exists():
            data = json.loads(identity.read_text())
            if data["config"] != fingerprint(config):
                raise EnvironmentError("config_invalid", "reuse", "kafka", "configuration changed; select a new project")
            alive = (subprocess.run(["docker", "inspect", data["container"]], capture_output=True).returncode == 0
                     if location == "docker" else Path("/proc/" + str(data["pid"])).exists())
            if alive and Path(data["manifest"]).exists(): return Path(data["manifest"])
        if not config.data["managed"]:
            raise EnvironmentError("config_missing", "kafka_manifest", "kafka", "external mode requires FLOWSQL_TEST_KAFKA_MANIFEST")
        initialize_credentials(config)
        distribution = kafka_distribution(config)
        root = STATE / config.data["project"] / ("kafka-service-" + location + "-" + secrets.token_hex(5))
        root.mkdir(parents=True, mode=0o700)
        args = ["/usr/bin/python3", str(REPO / "src/tests/test_kafka_connection_auth/real_auth_matrix.py"),
                "--config", str(config.path), "--kafka-home", str(distribution), "--mode", location,
                "--output", str(root), "--serve"]
        if invalid_advertised:args.append("--invalid-advertised")
        name = ""
        if location == "docker":
            network = prepare_network(config)
            name = config.data["project"].replace("_", "-") + "-kafka-service"
            args += ["--network", network]
            args = ["docker", "run", "--rm", "--name", name, "--network", network,
                    "--network-alias", config.data["kafka"]["hosts"]["docker"], "--label", "flowsql.test.project="+config.data["project"],
                    "-v", f"{REPO}:{REPO}:ro", "-v", f"{STATE}:{STATE}", config.data["images"]["harness"], *args]
        log = (root / "service.log").open("w")
        process = subprocess.Popen(args, stdout=log, stderr=log, start_new_session=True)
        log.close()
        manifest = root / "profiles.json"
        while not manifest.exists():
            if process.poll() is not None:
                raise EnvironmentError("service_unavailable", "kafka_prepare", "kafka", "inspect private service.log")
            if time.monotonic() >= deadline:
                if name: subprocess.run(["docker", "stop", "-t", "10", name], capture_output=True)
                process.terminate()
                raise EnvironmentError("preparation_timeout", "kafka_prepare", "kafka")
            time.sleep(.1)
        private_write(identity, json.dumps({"config":fingerprint(config), "pid":process.pid,"container":name,"manifest":str(manifest)}))
        return manifest


def deployment(config, location, output):
    root = STATE / config.data["project"] / ("deployment-" + location + "-" + secrets.token_hex(6))
    root.mkdir(parents=True, mode=0o700)
    script = REPO / "src/tests/test_kafka_connection_auth/deployment_smoke.py"
    args = ["python3", str(script), "--mode", location, "--client-image", config.data["images"]["runtime"],
            "--output", str(root)]
    with (root / "deployment.log").open("w") as log:
        result = subprocess.run(args, stdout=log, stderr=log, timeout=300)
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    (output / "deployment.log").write_text((root / "deployment.log").read_text())
    if result.returncode:
        raise EnvironmentError("test_failed", "deployment", "kafka", "inspect deployment.log")
    return {"category": "ready", "location": location, "generations": 2}


def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=("databases", "distribution", "kafka", "probe", "deployment", "kafka-service"))
    parser.add_argument("--config")
    parser.add_argument("--location", choices=("native", "docker"), default="docker")
    parser.add_argument("--output", default=str(REPO / "evidence/test-environment/kafka-fixture"))
    args = parser.parse_args()
    try:
        config = Config(args.config)
        if args.action == "probe":
            print(json.dumps([probe_database(config, b, "driver", args.location, False, 10) for b in BACKENDS]))
        elif args.action == "databases":
            deadline = time.monotonic() + config.data["budgets"]["preparation_seconds"]
            prepare_databases(config, list(BACKENDS), deadline, args.location)
            wait_databases(config, list(BACKENDS), args.location, deadline)
            print(json.dumps([probe_database(config, backend, role, args.location, True, 10)
                              for backend in BACKENDS for role in config.data["databases"][backend]["roles"]]))
        elif args.action == "distribution":
            print(kafka_distribution(config))
        elif args.action == "kafka-service":
            print(prepare_kafka(config,args.location))
        elif args.action == "deployment":
            print(json.dumps(deployment(config, args.location, args.output)))
        else:
            print(json.dumps(kafka_matrix(config, args.location, args.output)))
    except EnvironmentError as error:
        print(json.dumps(error.result()))
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
