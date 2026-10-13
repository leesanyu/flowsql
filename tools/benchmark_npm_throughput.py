#!/usr/bin/env python3
"""Run the frozen NPM matrix; alternate versions within each independent-process round."""

import argparse
import csv
import hashlib
import io
import json
import statistics
import subprocess
from pathlib import Path


def throughput_cases():
    cases = []
    for pattern, rows, iterations in (("rst", 512, 200), ("ack", 30000, 10)):
        for family in ("ipv4", "ipv6", "mixed"):
            for mode in ("basic", "session"):
                cases.append(dict(id=f"{pattern}-{family}-{mode}", args=[
                    str(rows), str(iterations), family, pattern, "16", str(rows), "0", mode, "1"
                ], gate_percent=20 if pattern == "rst" else 10))
    representatives = [
        ("batch-1-short", 1, 2000, "ipv4", "rst", 12, 1, 0, "basic", 1),
        ("batch-32-long", 32, 200, "mixed", "rst", 256, 32, 0, "session", 1),
        ("batch-4096-short", 4096, 16, "mixed", "rst", 12, 4096, 0, "session", 1),
        ("ack-increasing", 512, 100, "mixed", "ack", 16, 512, 1, "session", 1),
        ("ack-long-many", 512, 100, "ipv6", "ack", 256, 512, 1, "basic", 1),
        ("ack-few-keys", 512, 100, "mixed", "ack", 12, 32, 0, "session", 1),
        ("periodic-not-due", 512, 100, "mixed", "ack", 16, 512, 1, "periodic", 1),
        ("periodic-dense", 32, 100, "mixed", "ack", 16, 32, 1000000, "periodic", 1),
        ("periodic-cross", 32, 40, "mixed", "ack", 16, 32, 10000000, "periodic", 1),
        ("session-payload", 512, 100, "mixed", "payload", 16, 512, 1, "session", 1),
    ]
    representatives.extend([
        ("labels-zero-candidates", 512, 100, "mixed", "ack", 16, 512, 1, "labels-zero", 1),
        ("labels-dense-admission", 512, 100, "mixed", "rst", 16, 512, 1, "labels-dense", 1),
        ("session-bidirectional", 512, 100, "mixed", "bidir", 16, 256, 1, "session", 1),
        ("session-multi-address", 512, 100, "mixed", "multiaddr", 16, 512, 1, "session", 1),
    ])
    for protocol in ("dns", "http1", "tls", "icmp"):
        for family in ("mixed", "ipv6"):
            representatives.append((f"{protocol}-{family}", 32, 30, family, protocol, 16, 32, 1, "basic", 1))
    for count in (1, 8, 64, 256):
        representatives.append((f"source-{count}", 512, 10, "mixed", "ack", 16, 512, 1, "basic", count))
        for probe in ("domain", "progress"):
            representatives.append((f"{probe}-{count}", 100, 100, "ipv4", probe, 16, 100, 1, "basic", count))
    for row in representatives:
        cases.append(dict(id=row[0], args=[str(value) for value in row[1:]], gate_percent=-3))
    return cases


def run(command):
    result = subprocess.run(command, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(f"{command!r}: exit {result.returncode}\n{result.stdout}\n{result.stderr}")
    return result.stdout, result.stderr


def parse_measurement(stdout):
    lines = [line for line in stdout.splitlines() if line.startswith("measurement,")]
    if len(lines) != 2:
        raise ValueError(f"expected header and one measurement, got {len(lines)}")
    return next(csv.DictReader(io.StringIO("\n".join(lines))))


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def parse_pairs(line, start=0):
    cells=line.split(",")[start:]
    return {cells[i]:cells[i+1] for i in range(0,len(cells)-1,2)}


def evaluate_gates(result):
    gates=[]
    for case in result["matrix"]:
        pair={item["label"]:item for item in result["summaries"] if item["case"]==case["id"]}
        if "baseline" not in pair or "optimized" not in pair:
            continue
        baseline=pair["baseline"]["primary_pps"]["median"]
        optimized=pair["optimized"]["primary_pps"]["median"]
        percent=100*(optimized/baseline-1)
        gates.append(dict(case=case["id"],metric="primary_pps",baseline=baseline,optimized=optimized,
                          change_percent=percent,minimum_percent=case["gate_percent"],passed=percent>=case["gate_percent"]))
    costs=[]
    for family in ("ipv4","ipv6","mixed"):
        groups={label:[item for item in result["runs"] if item["case"]==f"benchmark_npm_family_cost-{family}" and item["label"]==label]
                for label in ("baseline","optimized")}
        if not all(groups.values()): continue
        summaries={}
        for label,items in groups.items():
            rows=[]
            for item in items:
                lines=item["stdout"].splitlines()
                phase=next(line for line in lines if line.startswith("phase,table_and_indexes,"))
                end=next(line for line in lines if line.startswith("owned_snapshot_heap_bytes,"))
                row=parse_pairs(end)
                row["table_heap_delta"]=parse_pairs(phase,2)["heap_delta"]
                rows.append(row)
                if row["limit_rejected"]!="1" or row["budget_returned"]!="1":
                    raise ValueError("cost probe failed the frozen capacity/budget assertions")
            summary={}
            for field in ("table_heap_delta","owned_snapshot_heap_bytes","tracked_peak","input_peak","rss_peak_bytes"):
                values=[float(row[field]) for row in rows]
                summary[field]=dict(median=statistics.median(values),min=min(values),max=max(values))
            summaries[label]=summary
            costs.append(dict(family=family,label=label,**summary))
        for metric,maximum in (("table_heap_delta",0),("owned_snapshot_heap_bytes",-20)):
            baseline=summaries["baseline"][metric]["median"];optimized=summaries["optimized"][metric]["median"]
            percent=100*(optimized/baseline-1)
            gates.append(dict(case=f"cost-{family}",metric=metric,baseline=baseline,optimized=optimized,
                              change_percent=percent,maximum_percent=maximum,passed=percent<=maximum))
    expected = {case["id"] for case in throughput_cases()}
    measured = {gate["case"] for gate in gates if gate["metric"] == "primary_pps"}
    samples_complete = all(
        len([run for run in result["runs"] if run["case"] == case and run["label"] == label]) >= 7
        for case in expected for label in ("baseline", "optimized")
    )
    result["frozen_acceptance_complete"] = (measured == expected and len(costs) == 6 and
        len(gates) == 52 and samples_complete)
    result["cost_summaries"]=costs
    result["gates"]=gates
    result["all_measured_gates_passed"]=bool(gates) and all(gate["passed"] for gate in gates)
    return gates


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", action="append", help="LABEL=Release build directory")
    parser.add_argument("--cpu", type=int, default=2)
    parser.add_argument("--repeats", type=int, default=7)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cases", help="comma separated frozen case IDs; default all")
    parser.add_argument("--include-historical", action="store_true")
    parser.add_argument("--include-cost", action="store_true")
    parser.add_argument("--analyze", type=Path, help="evaluate a completed result without rerunning processes")
    parser.add_argument("--iteration-multiplier", type=int, default=1, help="same replay extension for both versions, capped at 10000 iterations")
    parser.add_argument("--probe-rows", type=int, help="extend only domain/progress operation repetition, keeping the source count unchanged")
    args = parser.parse_args()
    if args.repeats < 7:
        parser.error("the frozen acceptance contract requires at least seven independent processes")
    if args.analyze:
        result = json.loads(args.analyze.read_text())
        evaluate_gates(result)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + "\n")
        failed = [gate for gate in result["gates"] if not gate["passed"]]
        print(f"evaluated {len(result['gates'])} measured gates; {len(failed)} require investigation")
        return
    if not args.build:
        parser.error("--build is required for measurement")
    if args.iteration_multiplier < 1 or (args.probe_rows is not None and not 1 <= args.probe_rows <= 30000):
        parser.error("invalid replay refinement")
    builds = [tuple(value.split("=", 1)) for value in args.build]
    cases = throughput_cases()
    if args.cases:
        requested = set(args.cases.split(","))
        cases = [case for case in cases if case["id"] in requested]
        if requested != {case["id"] for case in cases}:
            parser.error("unknown frozen case ID")
    for case in cases:
        case["frozen_args"] = list(case["args"])
        case["args"][1] = str(min(10000, int(case["args"][1]) * args.iteration_multiplier))
        if args.probe_rows is not None and case["args"][3] in ("domain", "progress"):
            case["args"][0] = str(args.probe_rows)
    result = dict(cpu=args.cpu, repeats=args.repeats, builds=dict(builds), matrix=cases, runs=[], summaries=[],
                  replay_refinement=dict(iteration_multiplier=args.iteration_multiplier, probe_rows=args.probe_rows))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    for case in cases:
        for repeat in range(args.repeats):
            # Reverse the order each round so paired versions are alternated.
            for label, directory in (builds if repeat % 2 == 0 else list(reversed(builds))):
                binary = Path(directory) / "output" / "benchmark_npm_throughput"
                command = ["taskset", "-c", str(args.cpu), str(binary), *case["args"]]
                stdout, stderr = run(command)
                row = parse_measurement(stdout)
                result["runs"].append(dict(case=case["id"], label=label, repeat=repeat,
                    command=command, binary_sha256=sha256(binary), measurement=row, stderr=stderr))
                args.output.write_text(json.dumps(result, indent=2) + "\n")
        for label, _ in builds:
            runs = [item["measurement"] for item in result["runs"]
                    if item["case"] == case["id"] and item["label"] == label]
            for field in ("foreground_rows", "business_digest", "wire_fixture_digest"):
                if len({row[field] for row in runs}) != 1:
                    raise ValueError(f"{case['id']} {label}: nondeterministic {field}")
            summary = dict(case=case["id"], label=label,
                foreground_rows=runs[0]["foreground_rows"], business_digest=runs[0]["business_digest"],
                wire_fixture_digest=runs[0]["wire_fixture_digest"])
            for field in ("primary_pps", "historical_pps", "end_to_end_pps", "cpu_ms", "flush_ms",
                          "heap_phase_peak", "rss_peak_bytes", "input_capacity"):
                values = [float(row[field]) for row in runs]
                summary[field] = dict(median=statistics.median(values), min=min(values), max=max(values))
            for field in ("process_ms", "release_ms", "validation_ms", "decode_ms", "input_release_ms"):
                if all(field in row for row in runs):
                    values = [float(row[field]) for row in runs]
                    summary[field] = dict(median=statistics.median(values), min=min(values), max=max(values))
            result["summaries"].append(summary)
        if len(builds) == 2:
            pair = result["summaries"][-2:]
            if any(pair[0][field] != pair[1][field] for field in
                   ("foreground_rows", "business_digest", "wire_fixture_digest")):
                raise ValueError(f"{case['id']}: business output or packet fixture differs across versions")
        args.output.write_text(json.dumps(result, indent=2) + "\n")
        print(f"completed {case['id']} ({args.repeats} independent processes per version)", flush=True)
    for name, enabled in (("benchmark_npm_basic", args.include_historical),
                          ("benchmark_npm_family_cost", args.include_cost)):
        if not enabled:
            continue
        for family in ("ipv4", "ipv6", "mixed"):
            for repeat in range(args.repeats):
                for label, directory in (builds if repeat % 2 == 0 else list(reversed(builds))):
                    binary = Path(directory) / "output" / name
                    arguments = ["512", "200", family] if name == "benchmark_npm_basic" else [family]
                    command = ["taskset", "-c", str(args.cpu), str(binary), *arguments]
                    stdout, stderr = run(command)
                    result["runs"].append(dict(case=f"{name}-{family}", label=label, repeat=repeat,
                        command=command, binary_sha256=sha256(binary), stdout=stdout, stderr=stderr))
            args.output.write_text(json.dumps(result, indent=2) + "\n")
            print(f"completed {name}-{family}", flush=True)
    evaluate_gates(result)
    args.output.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
