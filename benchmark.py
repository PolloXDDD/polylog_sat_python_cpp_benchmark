#!/usr/bin/env python3
"""Reproducible sequential benchmark; stdlib only. Optional matplotlib plot.

Both implementations run in fresh processes, in alternated order. Graph time,
internal total time, process wall time and peak RSS are recorded separately.
Every SAT witness is checked against the original formula. Random SAT/UNSAT
labels come from an independent DPLL, never from the measured algorithm.
"""
import argparse
import csv
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import random
import statistics
import subprocess
import sys
import time
from validate import dpll, write_cnf
from config_graph import read_dimacs, verify_assignment

ROOT = Path(__file__).resolve().parent


def cases(random_sizes, seeds, gaps):
    inputs = ROOT / "instances"
    inputs.mkdir(exist_ok=True)
    result = []

    def add(family, n, clauses, seed=None, expected=None, **metadata):
        name = f"{family}_n{n}" + (f"_s{seed}" if seed is not None else "")
        path = inputs / (name + ".cnf")
        write_cnf(path, n, clauses)
        if expected is None:
            expected = "SAT" if dpll(clauses) else "UNSAT"
        result.append(dict(case=name, family=family, n=n, m=len(clauses), seed=seed,
                           mode="cnf", arguments=["cnf", str(path)], expected=expected,
                           sha256=hashlib.sha256(path.read_bytes()).hexdigest(), **metadata))

    for n in random_sizes:
        for seed in range(seeds):
            rng = random.Random(20261018 + n * 1000 + seed)
            clauses = [[v * rng.choice((-1, 1)) for v in rng.sample(range(1, n + 1), 3)]
                       for _ in range(round(4.26 * n))]
            add("random3sat", n, clauses, seed=seed)

    for n in (100, 1000, 10000):
        chain = [c for i in range(1, n) for c in ([-i, i+1], [i, -(i+1)])]
        add("chain_sat", n, chain, expected="SAT")
        add("chain_unsat", n, chain + [[1], [-n]], expected="UNSAT")

    for h in gaps:
        separated = [c for i in range(1, h+1) for c in ([-i, h+i], [i, -(h+i)])]
        add("separated_equalities", 2*h, separated, expected="SAT", pairs=h)
        interleaved = [c for i in range(1, h+1) for c in ([-(2*i-1), 2*i], [2*i-1, -2*i])]
        add("interleaved_equalities", 2*h, interleaved, expected="SAT", pairs=h)

    for modulus in (64, 256, 1024, 4096):
        bits = math.ceil(math.log2(modulus)) ** 2
        target = modulus - 1
        result.append(dict(case=f"mod_M{modulus}_q{bits}", family="modular", n=modulus,
                           m=None, seed=None, mode="mod", bits=bits, target=target,
                           arguments=["mod", str(modulus), str(bits), str(target)],
                           expected="ACCEPT", sha256=None))
    return result


def summarize(rows):
    groups = {}
    for row in rows:
        groups.setdefault((row["family"], row["n"], row["engine"]), []).append(row)
    summary = []
    for (family, n, engine), runs in sorted(groups.items()):
        good = [r for r in runs if r["correct"]]
        item = dict(family=family, n=n, engine=engine, runs=len(runs), correct=len(good),
                    unique_cases=len({r['case'] for r in runs}),
                    sat_cases=len({r['case'] for r in runs if r['status'] in ('SAT', 'ACCEPT')}),
                    unsat_cases=len({r['case'] for r in runs if r['status'] in ('UNSAT', 'REJECT')}),
                    unknown_runs=sum(r['status'] in ('UNKNOWN', 'TIMEOUT') for r in runs))
        for field in ("graph_seconds", "prepare_seconds", "total_seconds", "process_seconds",
                      "peak_rss_kib", "configurations", "peak_frontier", "merged_edges", "state_bits_bound"):
            values = [r[field] for r in good if r.get(field) is not None]
            item[field + "_median"] = statistics.median(values) if values else None
            if field in ("peak_frontier", "configurations", "peak_rss_kib", "graph_seconds"):
                item[field + "_max"] = max(values) if values else None
        summary.append(item)
    return summary


def write_csv(path, rows):
    fields = sorted(set().union(*(r.keys() for r in rows)))
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--repeats", type=int, default=3)
    p.add_argument("--seeds", type=int, default=8)
    p.add_argument("--random-sizes", type=int, nargs="+", default=[12, 18, 24, 30, 36])
    p.add_argument("--gaps", type=int, nargs="+", default=[8, 12, 16, 18])
    p.add_argument("--timeout", type=float, default=30, help="per-process watchdog; TIMEOUT stays inconclusive")
    p.add_argument("--cpu", type=int, help="optional CPU affinity, Linux")
    args = p.parse_args()
    if args.repeats < 1 or args.seeds < 1 or args.timeout <= 0 or any(n < 3 for n in args.random_sizes) or any(h < 1 for h in args.gaps):
        p.error("invalid benchmark size, count or timeout")
    if args.cpu is not None:
        os.sched_setaffinity(0, {args.cpu})
    binary = ROOT / "config_graph_cpp"
    compile_command = ["g++", "-O3", "-DNDEBUG", "-std=c++17", str(ROOT / "config_graph.cpp"), "-o", str(binary)]
    subprocess.run(compile_command, check=True)
    all_cases = cases(args.random_sizes, args.seeds, args.gaps)
    results_dir = ROOT / "results"
    results_dir.mkdir(exist_ok=True)
    cpu_model = "unknown"
    if Path("/proc/cpuinfo").exists():
        cpu_model = next((line.split(":", 1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines()
                          if line.startswith("model name")), "unknown")
    environment = dict(created_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                       python=sys.version, platform=platform.platform(), cpu=cpu_model,
                       affinity=sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None,
                       compiler=subprocess.check_output(["g++", "--version"], text=True).splitlines()[0],
                       compile_command=compile_command, parameters=vars(args),
                       timer="monotonic wall time; graph excludes parse/compile/verification; total includes parse, prepare, graph and witness check; process includes startup/output",
                       memory="Linux /proc/self/status VmHWM in KiB, current address space only; includes interpreter/runtime, input and graph; not baseline-subtracted; ru_maxrss fallback outside Linux",
                       ordering="sequential, Python/C++ order alternates by case and repetition; one unrecorded warm-up per engine",
                       source_sha256={name: hashlib.sha256((ROOT/name).read_bytes()).hexdigest()
                                      for name in ("config_graph.py", "config_graph.cpp", "benchmark.py", "validate.py")})
    (results_dir / "environment.json").write_text(json.dumps(environment, indent=2) + "\n")
    manifest = [{**c, "arguments": [c["arguments"][0], "instances/" + Path(c["arguments"][1]).name] if c["mode"] == "cnf" else c["arguments"]}
                for c in all_cases]
    (results_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    commands = {"python": [sys.executable, str(ROOT/"config_graph.py")], "cpp": [str(binary)]}
    for command in commands.values():
        subprocess.run(command + ["mod", "64", "36", "63"], stdout=subprocess.DEVNULL, check=True)
    rows, seen_metrics = [], {}
    started = time.perf_counter()
    with (results_dir / "raw.jsonl").open("w") as raw:
        for case_index, case in enumerate(all_cases):
            formula = read_dimacs(case["arguments"][1]) if case["mode"] == "cnf" else None
            print(f"[{case_index + 1}/{len(all_cases)}] {case['case']} expected={case['expected']}", flush=True)
            for repeat in range(args.repeats):
                engines = ("python", "cpp") if (case_index + repeat) % 2 == 0 else ("cpp", "python")
                for engine in engines:
                    start = time.perf_counter()
                    try:
                        completed = subprocess.run(commands[engine] + case["arguments"], capture_output=True,
                                                   text=True, timeout=args.timeout, check=True)
                        result = json.loads(completed.stdout)
                        elapsed = time.perf_counter() - start
                        if result["status"] in ("SAT", "ACCEPT"):
                            if formula:
                                assert len(result["witness"]) == formula[0] and verify_assignment(formula[1], result["witness"])
                            else:
                                residue = 0
                                assert len(result["witness"]) == case["bits"]
                                for bit in result["witness"]:
                                    residue = (2*residue + bit) % case["n"]
                                assert residue == case["target"]
                        result.pop("witness", None)
                    except subprocess.TimeoutExpired:
                        elapsed = time.perf_counter() - start
                        result = dict(status="TIMEOUT", reason="external_watchdog", engine=engine)
                    result.update(case=case["case"], family=case["family"], n=case["n"], m=case["m"], seed=case["seed"],
                                  repeat=repeat, expected=case["expected"], process_seconds=elapsed,
                                  correct=result["status"] == case["expected"])
                    if result["status"] not in ("UNKNOWN", "TIMEOUT"):
                        assert result["correct"], result
                        metrics = tuple(result[f] for f in ("configurations", "peak_frontier", "attempted_edges", "merged_edges"))
                        if case["case"] in seen_metrics:
                            assert metrics == seen_metrics[case["case"]], (case["case"], engine, metrics)
                        else:
                            seen_metrics[case["case"]] = metrics
                    rows.append(result)
                    raw.write(json.dumps(result) + "\n")
                    raw.flush()
    summary = summarize(rows)
    write_csv(results_dir / "runs.csv", rows)
    write_csv(results_dir / "summary.csv", summary)
    (results_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(dict(unique_cases=len(all_cases), runs=len(rows), correct=sum(r["correct"] for r in rows),
                          inconclusive=sum(r["status"] in ("UNKNOWN", "TIMEOUT") for r in rows),
                          benchmark_seconds=time.perf_counter() - started), indent=2))


if __name__ == "__main__":
    main()
