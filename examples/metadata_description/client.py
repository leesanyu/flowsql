#!/usr/bin/env python3
# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Ordinary HTTP object discovery; --execute explicitly checks and runs a simple SQL call."""
import argparse
import json
import re
import urllib.request


def run(web_url, input_name="input", output_name="description_demo", execute=False):
    """Read actual objects; optionally execute once into a new named DataFrame."""
    def request(path, payload=None):
        body = None if payload is None else json.dumps(payload).encode()
        req = urllib.request.Request(web_url.rstrip("/") + "/api/" + path, data=body,
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=30) as response:
            value = json.load(response)
        return value["data"] if path.startswith("semantics/v1/") else value

    sources = request("channels/dataframe")["channels"]
    operators = request("operators/list?type=builtin")["operators"]
    if not any(item["name"] == input_name for item in sources):
        raise ValueError("input is absent from the DataFrame directory")
    operator = next(item for item in operators
                    if item["category"] == "builtin" and item["name"] == "passthrough")
    locator = {"kind": "dataframe", "name": input_name}
    source = request("semantics/v1/describe", {"target": {"kind": "channel", "locator": locator}})
    description = request("semantics/v1/describe", {
        "target": {"kind": "operator", "name": "builtin.passthrough"}})
    report = {"source": source, "operator": description, "executed": False}
    if not execute:
        return report
    if not all(re.fullmatch(r"[a-z_][a-z0-9_]*", name) for name in (input_name, output_name)):
        raise ValueError("this simple SQL example requires lowercase identifiers with underscores")
    if any(item["name"] == output_name for item in sources):
        raise ValueError("choose a new output name; the example does not replace an existing DataFrame")
    if str(operator["active"]) != "1":
        raise ValueError("activate builtin.passthrough through the existing operator manager first")
    if source["structure"]["state"] != "known":
        raise ValueError("explicitly load the input through its existing channel entry first")
    checked = request("semantics/v1/operators/check", {
        "operator": {"category": "builtin", "name": "passthrough"}, "parameters": {},
        "inputs": [{"input_index": 0, "locator": locator}],
        "output_locator": {"kind": "dataframe", "name": output_name}})
    report["preflight"] = checked
    if checked["check"]["state"] == "incompatible":
        raise ValueError("the actual input is incompatible with the operator contract")
    result = request("tasks/batch/execute", {
        "sql_text": f"SELECT * FROM dataframe.{input_name} USING builtin.passthrough INTO dataframe.{output_name}",
        "mode": "sync"})
    report.update(executed=True, result=result, uses=[], dictionaries=[])
    seen = set()
    for check in result.get("contract_checks", []):
        if not check.get("use_ref"):
            continue
        use = request("semantics/v1/resolve", {"exact_reference": check["use_ref"]})
        report["uses"].append(use)
        for output in use["document"]["outputs"]:
            refs = [output["dictionary_ref"]] if output.get("dictionary_ref") else []
            refs += output.get("required_context_refs", [])
            for context in output.get("semantic_contexts", []):
                refs += context.get("required_context_refs", [])
            for reference in refs:
                if reference not in seen:
                    seen.add(reference)
                    report["dictionaries"].append(request("semantics/v1/resolve", {"exact_reference": reference}))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--web-url", required=True)
    parser.add_argument("--input", default="input")
    parser.add_argument("--output", default="description_demo")
    parser.add_argument("--execute", action="store_true",
                        help="explicitly preflight and execute SQL into a new named DataFrame")
    args = parser.parse_args()
    print(json.dumps(run(args.web_url, args.input, args.output, args.execute), ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
