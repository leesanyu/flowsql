# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Run the real flowsql --config entrypoint and verify mounted state across restart."""
import argparse
import json
import os
import pathlib
import socket
import subprocess
import time
import urllib.error
import urllib.request

parser = argparse.ArgumentParser()
parser.add_argument("--mode", choices=["native", "docker"], required=True)
parser.add_argument("--client-image", default="flowsql:kafka-auth")
parser.add_argument("--output")
args = parser.parse_args()
repo = pathlib.Path(__file__).resolve().parents[3]
root = pathlib.Path(args.output) if args.output else pathlib.Path("/tmp") / ("flowsql-kafka-deployment-" + str(time.time_ns()))
os.umask(0o077)
for directory in [root, root / "config", root / "state", root / "secrets", root / "runtime"]:
    directory.mkdir(parents=True, exist_ok=True)
os.chmod(root / "runtime", 0o700)
os.chmod(root / "secrets", 0o700)
master = os.urandom(32)
(root / "secrets/master.key").write_bytes(master)
os.chmod(root / "secrets/master.key", 0o600)
ports = []
while len(ports) < 3:
    with socket.socket() as endpoint:
        endpoint.bind(("127.0.0.1", 0))
        port = endpoint.getsockname()[1]
        if port not in ports:
            ports.append(port)
gateway, router, web = ports
base = root if args.mode == "native" else pathlib.Path("/opt/flowsql/kafka-smoke")
provider = {"db_path": str(base / "state/kafka.db"), "master_key_file": str(base / "secrets/master.key"),
            "runtime_directory": str(base / "runtime"), "files": {}, "environments": {}}
(root / "config/provider.json").write_text(json.dumps(provider))
plugins = [
    {"name": "libflowsql_gateway.so", "option": f"host=127.0.0.1;port={gateway}"},
    {"name": "libflowsql_router.so", "option": f"host=127.0.0.1;port={router};gateway=127.0.0.1:{gateway}"},
    {"name": "libflowsql_kafka.so", "option": "config_file=" + str(base / "config/provider.json")},
    {"name": "libflowsql_web.so", "option": f"host={'0.0.0.0' if args.mode == 'docker' else '127.0.0.1'};port={web};gateway=127.0.0.1:{gateway};db_path=:memory:;upload_dir=/tmp/kafka-smoke"},
]
# JSON is a YAML subset; this uses the same mapped plugin options as the deployment examples.
(root / "config/deploy.yaml").write_text(json.dumps({"services": [{"name": "kafka-smoke", "plugins": plugins}]}))
name = "flowsql-kafka-deployment-" + str(os.getpid())
command = [str(repo / "build/output/flowsql"), "--config", str(root / "config/deploy.yaml")]
if args.mode == "docker":
    command = ["docker", "run", "--rm", "--name", name, "--label", "flowsql.test.deployment=true", "-v", str(repo) + ":" + str(repo) + ":ro", "-v", str(repo / "build/output") + ":/opt/flowsql/bin:ro"]
    for folder in ["config", "secrets", "state"]:
        mount = str(root / folder) + ":" + str(base / folder)
        command += ["-v", mount + (":ro" if folder != "state" else "")]
    command += ["--tmpfs", str(base / "runtime") + ":mode=0700", args.client_image,
                "flowsql", "--config", str(base / "config/deploy.yaml")]


def post(operation, body=None, status=200):
    if args.mode == "docker":
        code = """import sys,json,urllib.request,urllib.error
operation,body=json.load(sys.stdin)
request=urllib.request.Request('http://127.0.0.1:{web}/api/channels/kafka/'+operation,data=json.dumps(body).encode(),headers={'Content-Type':'application/json'})
try: response=urllib.request.urlopen(request,timeout=5)
except urllib.error.HTTPError as error: response=error
print(json.dumps({'status':response.status,'body':json.loads(response.read())}))
""".replace("{web}", str(web))
        result = subprocess.run(["docker", "exec", "-i", name, "python3", "-c", code],
                                input=json.dumps([operation,body or {}]), capture_output=True, text=True, timeout=10)
        if result.returncode: raise OSError("container HTTP probe unavailable")
        report=json.loads(result.stdout)
        assert report["status"] == status, (operation,report["status"])
        return report["body"]
    request = urllib.request.Request(f"http://127.0.0.1:{web}/api/channels/kafka/{operation}",
                                     data=json.dumps(body or {}).encode(), headers={"Content-Type": "application/json"})
    try:
        response = urllib.request.urlopen(request, timeout=5)
    except urllib.error.HTTPError as error:
        response = error
    assert response.status == status, (operation, response.status)
    return json.loads(response.read())


secret = "deployment-secret-" + os.urandom(12).hex()
connection = {"name": "persisted", "bootstrap_servers": ["localhost:1"], "security_protocol": "SASL_PLAINTEXT",
              "sasl_mechanism": "PLAIN", "sasl": {"username": "smoke"},
              "credential_bindings": {"password": {"operation": "replace", "value": secret}}}
test_id = None
for generation in range(2):
    log_path = root / f"run-{generation}.log"
    with log_path.open("w") as log:
        process = subprocess.Popen(command, cwd=repo / "build/output", stdout=log, stderr=log)
        try:
            for attempt in range(200):
                assert process.poll() is None, "runtime exited; inspect " + str(log_path)
                try:
                    capabilities = post("definitions/query")
                    break
                except (OSError, AssertionError):
                    time.sleep(0.1)
            else:
                raise AssertionError("runtime did not become ready; inspect " + str(log_path))
            assert capabilities["client_version"] == "2.16.0"
            if args.mode == "docker":
                inspection = json.loads(subprocess.check_output(["docker", "inspect", name]))[0]
                mounts = {m["Destination"]: m for m in inspection["Mounts"]}
                assert not mounts[str(base / "secrets")]["RW"] and not mounts[str(base / "config")]["RW"]
                assert mounts[str(base / "state")]["RW"]
                subprocess.run(["docker", "exec", name, "python3", "-c",
                                "import os; assert os.stat('/opt/flowsql/kafka-smoke/runtime').st_mode & 0o777 == 0o700"], check=True)
            if generation == 0:
                post("connections/add", connection)
                post("channels/add", {"name": "events", "connection_name": "persisted", "role": "both",
                                      "topic": "events", "consumer_group": "smoke", "payload_kind": "structured",
                                      "payload_options": {"source": {"encoding":"json", "entity":"metric", "fields":[{"name":"value","path":"/v","type":"int64","nullable":False}]},
                                                          "sink": {"encoding":"json", "entity":"alert", "schema_version":1, "record_kind":"upsert", "key_fields":["id"], "revision_field":"revision"}}})
                test_id = post("tests/start", {"target": "channel", "name": "events", "timeout_ms": 1}, 202)["test_id"]
                for _ in range(100):
                    if post("tests/query", {"test_id": test_id})["status"] != "running":
                        break
                    time.sleep(0.05)
                else:
                    raise AssertionError("verification failed to release its lease")
            else:
                post("tests/query", {"test_id": test_id}, 404)
                update = dict(connection, expected_revision=1,
                              credential_bindings={"password": {"operation": "keep"}})
                post("connections/modify", update)
            definitions = post("connections/query", {"name": "persisted"})
            assert len(definitions["items"]) == 1 and secret not in json.dumps(definitions)
            assert definitions["items"][0]["revision"] == generation + 1
            channels = post("channels/query", {"name": "events"})["items"]
            assert len(channels) == 1 and channels[0]["role"] == "both" and not channels[0]["in_use"]
            assert channels[0]["payload_options"]["source"]["entity"] == "metric"
            assert channels[0]["payload_options"]["sink"]["revision_field"] == "revision"
            assert channels[0]["adapter_status"] == {"source":"ready", "sink":"ready"}
            assert capabilities["adapter_status"]["structured"]["sink"] == "available"
        finally:
            if args.mode == "docker":
                subprocess.run(["docker", "stop", "-t", "20", name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
            else:
                process.terminate()
            process.wait(timeout=30)
        assert secret not in log_path.read_text()
    assert (root / "secrets/master.key").read_bytes() == master
    assert secret.encode() not in (root / "state/kafka.db").read_bytes()
    print(f"PASS {args.mode} generation {generation}: config_file, persisted CRUD, encrypted secret, released leases", flush=True)
if args.mode == "docker":
    print("PASS: readonly config/secret mounts, persistent state mount and private tmpfs")
print("PASS: restart retains definitions and clears verification; evidence", root)
