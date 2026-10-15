# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Runtime CTest guard, read-only doctor and exact selected/executed accounting."""
import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import uuid
import urllib.parse

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
from test_environment.config import BACKENDS, Config, EnvironmentError, STATE, dependencies
from test_environment.fixture import (deployment, fingerprint, kafka_matrix, lock, prepare_databases,
                                      prepare_kafka, private_write, probe_database, runtime_command, wait_databases)


def inventory():
    result = subprocess.run(["ctest", "--test-dir", str(REPO / "build"), "--show-only=json-v1"],
                            capture_output=True, text=True)
    if result.returncode:
        raise EnvironmentError("artifact_missing", "ctest_inventory", detail="configure cmake -B build src")
    tests=json.loads(result.stdout)["tests"]
    names = [item["name"] for item in tests]
    inventory.fixture_requirements={item["name"]:next((p["value"] for p in item.get("properties",[]) if p["name"]=="FIXTURES_REQUIRED"),[]) for item in tests}
    inventory.fixture_setups={fixture:item["name"] for item in tests for fixture in next((p["value"] for p in item.get("properties",[]) if p["name"]=="FIXTURES_SETUP"),[])}
    if len(names) != len(set(names)):
        raise EnvironmentError("config_invalid", "ctest_inventory", detail="duplicate registered test")
    return names


def select(names, mode, pattern=None):
    declared = dependencies()
    missing = set(declared) - set(names)
    if missing:
        raise EnvironmentError("artifact_missing", "ctest_inventory", detail="unregistered dependencies: " + ",".join(sorted(missing)))
    selected = [name for name in names if (mode == "all" or (name in declared) == (mode == "integration"))
                and (pattern is None or re.search(pattern, name))]
    for name in list(selected):
        setup=name+"__environment"
        if setup in names and setup not in selected:selected.append(setup)
    requirements=getattr(inventory,"fixture_requirements",{})
    setups=getattr(inventory,"fixture_setups",{})
    for name in list(selected):
        for fixture in requirements.get(name,[]):
            setup=setups.get(fixture)
            if setup and setup not in selected:selected.append(setup)
    return selected


def scrub(text, config=None):
    if config:
        for reference in [db["password_ref"] for db in config.data["databases"].values()] + [config.data["kafka"]["password_ref"]]:
            try:
                text = text.replace(config.secret(reference), "[REDACTED]")
            except EnvironmentError:
                pass
    return text


def kafka_probe(config, location, profile, manifest):
    try:
        identity=json.loads(Path(manifest).read_text())[profile]
    except (OSError,ValueError,KeyError):
        raise EnvironmentError("config_missing","kafka_manifest","kafka") from None
    connection=identity["connection"]
    if connection.get("security_protocol") in ("PLAINTEXT","SSL"):
        address_probe=["python3",str(REPO/"tools/test_environment/broker_addresses.py"),str(manifest),profile]
        try:
            response=subprocess.run(runtime_command(config,address_probe,location=location),capture_output=True,text=True,timeout=4)
            addresses=json.loads(response.stdout.strip())
        except (OSError,ValueError,subprocess.TimeoutExpired):
            raise EnvironmentError("network_unreachable","advertised","kafka") from None
        if response.returncode:
            raise EnvironmentError(addresses["category"],addresses["stage"],"kafka")
    if connection.get("sasl_mechanism")=="OAUTHBEARER":
        sasl=connection["sasl"]
        body=urllib.parse.urlencode({"grant_type":"client_credentials","client_id":sasl["client_id"],
                                   "client_secret":connection["credential_bindings"]["client_secret"]["value"]})
        private=STATE/"oauth-probes"/uuid.uuid4().hex
        private.mkdir(parents=True,mode=0o700)
        # Resolved secret remains in a private file; HTTP status is the only emitted response.
        curl_config='url = "'+sasl['token_endpoint']+'"\n'+'data = "'+body+'"\n'
        curl_config+='cacert = "'+identity['files'][sasl['ca_ref'].split(':',1)[1]]+'"\n'
        private_write(private/"curl.conf",curl_config)
        curl=["curl","--silent","--show-error","--connect-timeout","3","--max-time","3","--config",str(private/"curl.conf"),
              "-o",str(private/"response.json"),"--write-out","%{http_code}"]
        try:
            response=subprocess.run(runtime_command(config,curl,location=location),capture_output=True,text=True,timeout=4)
        except (OSError,subprocess.TimeoutExpired):
            raise EnvironmentError("token_service_unavailable","oauth","kafka") from None
        finally:
            (private/"response.json").unlink(missing_ok=True)
            (private/"curl.conf").unlink(missing_ok=True)
        if response.returncode or response.stdout.strip()!="200":
            category="authentication_failed" if response.stdout.strip() in ("401","403") else "token_service_unavailable"
            if response.returncode in (51,60):category="tls_validation_failed"
            raise EnvironmentError(category,"oauth","kafka")
    args = [str(REPO / "build/output/test_environment_kafka_probe"), str(manifest), profile]
    args = runtime_command(config, args, location=location)
    try:
        result = subprocess.run(args, capture_output=True, text=True, timeout=4)
    except subprocess.TimeoutExpired:
        raise EnvironmentError("network_unreachable", "metadata", "kafka") from None
    except OSError as error:
        raise EnvironmentError("execution_denied" if isinstance(error,PermissionError) else "artifact_missing", "metadata", "kafka") from None
    try:
        report = json.loads(result.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        category = "execution_denied" if "permission denied" in result.stderr.lower() else "artifact_missing"
        raise EnvironmentError(category, "metadata", "kafka") from None
    if result.returncode:
        code = report.get("code", report.get("error", {}).get("code", "CONFIG_INVALID"))
        mapping = {"AUTHENTICATION_FAILED":"authentication_failed", "TLS_VALIDATION_FAILED":"tls_validation_failed",
                   "UNSUPPORTED_MECHANISM":"mechanism_unavailable", "CLIENT_CAPABILITY_UNAVAILABLE":"mechanism_unavailable", "TOPIC_NOT_FOUND":"topic_missing",
                   "METADATA_DENIED":"metadata_denied", "KERBEROS_UNAVAILABLE":"kerberos_unavailable",
                   "TOKEN_SERVICE_UNAVAILABLE":"token_service_unavailable", "CREDENTIAL_EXPIRED":"credential_expired",
                   "BROKER_UNREACHABLE":"broker_address_unreachable", "ADVERTISED_BROKER_UNVERIFIED":"broker_address_unreachable",
                   "TOPIC_METADATA_FAILED":"metadata_denied", "KERBEROS_TICKET_FAILED":"kerberos_unavailable",
                   "OAUTH_TOKEN_ACQUISITION_FAILED":"token_service_unavailable", "NETWORK_UNREACHABLE":"network_unreachable", "CONNECTION_FAILED":"network_unreachable",
                   "TIMEOUT":"network_unreachable", "CONFIG_INVALID":"config_invalid"}
        category = "tls_validation_failed" if report.get("stage")=="tls" else mapping.get(code, "service_unavailable")
        if report.get("stage")=="credential":category="kerberos_unavailable"
        if code=="TIMEOUT" and report.get("checked_brokers"):
            category="broker_address_unreachable"
        raise EnvironmentError(category, "metadata", "kafka", "profile="+profile+"; code="+code)
    return {"category":"ready","backend":"kafka","profile":profile,"location":location,
            "read_permission":"unverified","write_permission":"unverified"}


def existing_kafka_manifest(config, location):
    if os.environ.get("FLOWSQL_TEST_KAFKA_MANIFEST"):
        return Path(os.environ["FLOWSQL_TEST_KAFKA_MANIFEST"])
    identity = STATE / config.data["project"] / ("kafka-"+location+"-identity.json")
    try:
        data=json.loads(identity.read_text())
        if data["config"] != fingerprint(config):
            raise EnvironmentError("config_invalid", "reuse", "kafka")
        return Path(data["manifest"])
    except (OSError, KeyError, ValueError):
        raise EnvironmentError("service_unavailable", "kafka_manifest", "kafka", "prepare integration fixture first") from None


def preflight(config, declarations, location, *, prepare=False, write=False):
    deadline = time.monotonic()+config.data["budgets"]["preparation_seconds"]
    db_roles = set()
    need_kafka=False
    for declaration in declarations:
        for service in declaration["services"]:
            if service=="kafka": need_kafka=True
            else:
                db_roles.add((service,declaration["role"]))
                if declaration["role"]=="source":db_roles.add((service,"target"))
    services=sorted({backend for backend,_ in db_roles})
    if prepare and services:
        prepare_databases(config,services,deadline,location)
        wait_databases(config,services,location,deadline)
    reports=[]
    for backend,role in sorted(db_roles):
        if time.monotonic()>=deadline:raise EnvironmentError("preparation_timeout","preflight",backend)
        result=probe_database(config,backend,role,location,write,min(3,max(.1,deadline-time.monotonic())))
        endpoint=config.data["databases"][backend]["endpoints"][location]
        reports.append(dict(result,role=role,location=location,endpoint=f"{endpoint['host']}:{endpoint['port']}"))
    if need_kafka:
        manifest=prepare_kafka(config,location,deadline) if prepare else existing_kafka_manifest(config,location)
        try: profiles=list(json.loads(manifest.read_text()))
        except (OSError,ValueError):raise EnvironmentError("config_missing","kafka_manifest","kafka") from None
        for profile in profiles:
            if time.monotonic()>=deadline:raise EnvironmentError("preparation_timeout","metadata","kafka")
            reports.append(kafka_probe(config,location,profile,manifest))
    return reports


def report_path(name):
    run=os.environ.get("FLOWSQL_TEST_RUN_ID","direct-"+str(os.getpid()))
    return STATE / "runs" / run / (name+".json")


def case(name, command):
    if command and "/" not in command[0] and (REPO/"build/output"/command[0]).exists():
        command=[str(REPO/"build/output"/command[0]),*command[1:]]
    declaration=dependencies().get(name)
    report={"name":name,"executed":False,"status":"failed"}
    config=None
    try:
        with contextlib.ExitStack() as stack:
            if declaration:
                config=Config()
                for resource in sorted(declaration["resources"]):stack.enter_context(lock(config.data["project"]+"-"+resource))
                location=declaration["location"]
                if location=="both":location=os.environ.get("FLOWSQL_TEST_LOCATION","docker")
                report["location"]=location
                # Real matrices/deployment have their own isolated preparation and lifecycle budget.
                if not name.startswith(("test_kafka_real_","test_kafka_deployment_")):
                    preflight(config,[declaration],location,prepare=True,write=True)
                workspace=STATE / "workspaces" / (name+"-"+uuid.uuid4().hex[:8])
                workspace.mkdir(parents=True,mode=0o700)
                environment=dict(os.environ,FLOWSQL_TEST_CONFIG=str(config.path),FLOWSQL_TEST_LOCATION=location,FLOWSQL_TEST_CASE=name,FLOWSQL_TEST_WORKSPACE=str(workspace))
                if "kafka" not in declaration["services"] and not name.endswith("__environment"):
                    command=runtime_command(config,command,location=location,env=environment)
            else:
                environment={k:v for k,v in os.environ.items() if not k.startswith("FLOWSQL_KAFKA_")}
            if not declaration:
                workspace=STATE / "workspaces" / (name+"-"+uuid.uuid4().hex[:8])
                workspace.mkdir(parents=True,mode=0o700)
                environment["FLOWSQL_TEST_WORKSPACE"]=str(workspace)
            environment["FLOWSQL_TEST_CASE"]=name
            result=subprocess.run(command,env=environment,capture_output=True,text=True,
                                  cwd=REPO/"build/output" if declaration else None)
            report["executed"]=True
            output=scrub(result.stdout+result.stderr,config)
            private_write(report_path(name).with_suffix(".log"),output)
            print(output,end="",flush=True)
            if result.returncode:
                denied=bool(re.search(r"Operation not permitted|Permission denied",output,re.I))
                raise EnvironmentError("execution_denied" if denied else "test_failed","case",name)
            if re.search(r"\[SKIP\]|\bSKIP:|skipped\s*[:=]\s*[1-9]",output,re.I):
                report["status"]="skipped"
                raise EnvironmentError("test_failed","coverage",name,"environment skip is not a pass")
            report["status"]="passed"
    except EnvironmentError as error:
        report["error"]=error.result()
        print(json.dumps(error.result()),flush=True)
    except (OSError,ValueError) as error:
        report["error"]={"category":"execution_denied" if isinstance(error,PermissionError) else "artifact_missing","stage":"case"}
        print(json.dumps(report["error"]),flush=True)
    private_write(report_path(name),json.dumps(report))
    return 0 if report["status"]=="passed" else 1


def artifacts():
    result={}
    for path in sorted((REPO/"build/output").iterdir()):
        if path.is_file() and (path.suffix==".so" or os.access(path,os.X_OK)):
            result[path.name]=hashlib.sha256(path.read_bytes()).hexdigest()
    return result


def sources():
    listing=subprocess.run(["git","ls-files","--cached","--others","--exclude-standard","src","tools","config/testing"],
                           cwd=REPO,capture_output=True,text=True,check=True)
    return {name:hashlib.sha256((REPO/name).read_bytes()).hexdigest() for name in sorted(set(listing.stdout.splitlines()))
            if (REPO/name).is_file()}


def run(mode, config_path, location, pattern, jobs):
    names=select(inventory(),mode,pattern)
    if not names:raise EnvironmentError("config_invalid","selection",detail="no matching tests")
    run_id=time.strftime("%Y%m%dT%H%M%SZ",time.gmtime())+"-"+uuid.uuid4().hex[:8]
    evidence=REPO / "evidence/test-environment" / run_id
    evidence.mkdir(parents=True)
    environment=dict(os.environ,FLOWSQL_TEST_RUN_ID=run_id,FLOWSQL_TEST_LOCATION=location)
    summary={"run_id":run_id,"mode":mode,"location":location,"selected":names,"executed":[],"passed":[],"failed":[],"skipped":[],"not_executed":names.copy()}
    config=None
    before=artifacts()
    source_before=sources()
    (evidence/"sources-before.json").write_text(json.dumps(source_before,indent=2))
    (evidence/"artifacts-before.json").write_text(json.dumps(before,indent=2))
    try:
        declared=dependencies()
        selected=[declared[name] for name in names if name in declared]
        if selected:
            config=Config(config_path)
            # Freeze the public runtime config for this run. References remain private external bindings.
            frozen=STATE / "runs" / run_id / "environment.json"
            data=json.loads(json.dumps(config.data))
            for db in data["databases"].values():
                if db["password_ref"].startswith("file:"):
                    db["password_ref"]="file:"+str(config.secret_path(db["password_ref"]))
            if data["kafka"]["password_ref"].startswith("file:"):
                data["kafka"]["password_ref"]="file:"+str(config.secret_path(data["kafka"]["password_ref"]))
            # Managed identity refers to the canonical config, so preserve its paths instead of changing identity.
            private_write(frozen,json.dumps(data))
            environment["FLOWSQL_TEST_CONFIG"]=str(frozen)
            summary["configuration"]={"schema_version":1,"project":config.data["project"]}
            preflight(config,[d for d in selected if "kafka" not in d["services"]],location,prepare=True,write=True)
        expression="^("+"|".join(re.escape(name) for name in names)+")$"
        result=subprocess.run(["ctest","--test-dir",str(REPO/"build"),"-R",expression,"--output-on-failure",
                               "--output-junit",str(evidence/"ctest.xml"),"-j",str(jobs)],
                              env=environment,capture_output=True,text=True)
        (evidence/"ctest.log").write_text(scrub(result.stdout+result.stderr,config))
        print(scrub(result.stdout+result.stderr,config),end="",flush=True)
        actual={}
        for path in (STATE/"runs"/run_id).glob("*.json"):
            if path.name=="environment.json":continue
            item=json.loads(path.read_text());actual[item["name"]]=item
        for name in names:
            item=actual.get(name)
            if not item:continue
            if item["executed"]:summary["executed"].append(name)
            summary[item["status"]].append(name)
            if item["executed"] and name in summary["not_executed"]:summary["not_executed"].remove(name)
        success=result.returncode==0 and summary["passed"]==names and len(summary["executed"])==len(names)
    except EnvironmentError as error:
        summary["environment_error"]=error.result();success=False
        print(json.dumps(error.result()),flush=True)
    after=artifacts()
    (evidence/"artifacts-after.json").write_text(json.dumps(after,indent=2))
    source_after=sources()
    (evidence/"sources-after.json").write_text(json.dumps(source_after,indent=2))
    if before!=after or source_before!=source_after:
        summary["environment_error"]={"category":"artifact_missing","stage":"artifact_changed","detail":"build outputs changed during verification; rebuild then rerun"}
        success=False
    summary["counts"]={key:len(summary[key]) for key in ("selected","executed","passed","failed","skipped","not_executed")}
    summary["status"]="passed" if success else "failed"
    (evidence/"summary.json").write_text(json.dumps(summary,indent=2))
    print(json.dumps({"status":summary["status"],"counts":summary["counts"],"evidence":str(evidence)}),flush=True)
    return 0 if success else 1


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("mode",choices=("unit","doctor","integration","all","case","kafka","deployment"))
    parser.add_argument("--config")
    parser.add_argument("--location",choices=("native","docker"),default=os.environ.get("FLOWSQL_TEST_LOCATION","docker"))
    parser.add_argument("--filter")
    parser.add_argument("--jobs",type=int,default=1)
    parser.add_argument("--name")
    arguments,command=parser.parse_known_args()
    if command[:1]==["--"]:command=command[1:]
    try:
        if arguments.mode=="case":return case(arguments.name,command)
        if arguments.mode=="doctor":
            config=Config(arguments.config)
            selected=[value for name,value in dependencies().items() if not arguments.filter or re.search(arguments.filter,name)]
            if not selected:raise EnvironmentError("config_invalid","selection")
            print(json.dumps(preflight(config,selected,arguments.location),indent=2));return 0
        if arguments.mode in ("kafka","deployment"):
            config=Config(arguments.config)
            output=REPO/"evidence/test-environment"/os.environ.get("FLOWSQL_TEST_RUN_ID","direct")/(arguments.mode+"-"+arguments.location+"-"+uuid.uuid4().hex[:8])
            print(json.dumps((kafka_matrix if arguments.mode=="kafka" else deployment)(config,arguments.location,output)));return 0
        return run(arguments.mode,arguments.config,arguments.location,arguments.filter,arguments.jobs)
    except EnvironmentError as error:
        print(json.dumps(error.result()),flush=True);return 1

if __name__=="__main__":raise SystemExit(main())
