#!/usr/bin/env python3
"""Independent truth-table checks plus Python/C++ differential checks."""
import itertools
import json
from pathlib import Path
import random
import subprocess
import tempfile
import time
import config_graph as cg

ROOT = Path(__file__).resolve().parent


def write_cnf(path, n, clauses):
    path.write_text(f"p cnf {n} {len(clauses)}\n" +
                    "".join(" ".join(map(str, c)) + " 0\n" for c in clauses), encoding="utf-8")


def brute_force(n, clauses):
    for values in itertools.product((False, True), repeat=n):
        if all(any(values[abs(lit) - 1] == (lit > 0) for lit in c) for c in clauses):
            return True
    return False


def dpll(clauses):
    """Independent reference; no configuration masks or merge logic."""
    clauses = [set(c) for c in clauses if not any(-v in c for v in c)]

    def simplify(formula, literal):
        return [c - {-literal} for c in formula if literal not in c]

    def search(formula):
        while True:
            if not formula:
                return True
            if any(not c for c in formula):
                return False
            unit = next((next(iter(c)) for c in formula if len(c) == 1), None)
            if unit is None:
                break
            formula = simplify(formula, unit)
        variable = min(abs(v) for v in min(formula, key=len))
        return search(simplify(formula, variable)) or search(simplify(formula, -variable))
    return search(clauses)


def main():
    started = time.perf_counter()
    binary = ROOT / "config_graph_cpp"
    if not binary.exists():
        subprocess.run(["g++", "-O3", "-DNDEBUG", "-std=c++17", str(ROOT / "config_graph.cpp"), "-o", str(binary)], check=True)
    checked = dict(exhaustive_two_variable=0, random_truth_table=0, wide_masks=0,
                   edge_cases=0, modular=0, parser_rejections=0, resource_limits=0)
    statuses = {"SAT": 0, "UNSAT": 0}
    compare = ["status", "witness", "configurations", "attempted_edges", "valid_edges",
               "merged_edges", "peak_frontier", "completed_layers", "widths", "state_bits_bound"]
    with tempfile.TemporaryDirectory() as temp:
        cnf_path = Path(temp) / "case.cnf"

        def check(n, clauses, category):
            expected = "SAT" if brute_force(n, clauses) else "UNSAT"
            assert dpll(clauses) == (expected == "SAT")
            py = cg.solve_cnf(n, clauses)
            write_cnf(cnf_path, n, clauses)
            cpp = json.loads(subprocess.check_output([str(binary), "cnf", str(cnf_path), "--trace"], text=True))
            assert py["status"] == cpp["status"] == expected, (n, clauses, py, cpp)
            for field in compare:
                assert py[field] == cpp[field], (field, n, clauses, py, cpp)
            if expected == "SAT":
                assert cg.verify_assignment(clauses, py["witness"])
                assert cg.verify_assignment(clauses, cpp["witness"])
            checked[category] += 1
            statuses[expected] += 1

        # Every subset of all 9 canonical non-tautological clauses on 2 vars.
        possible = [[(i + 1) * sign for i, sign in enumerate(signs) if sign]
                    for signs in itertools.product((-1, 0, 1), repeat=2)]
        for subset in range(1 << len(possible)):
            check(2, [c for i, c in enumerate(possible) if subset >> i & 1], "exhaustive_two_variable")

        rng = random.Random(20261018)
        for n in range(1, 13):
            for _ in range(25):
                clauses = []
                for j in range(rng.randrange(0, 6 * n + 1)):
                    width = min(3, n) if n >= 3 else rng.randint(1, n)
                    clauses.append([v * rng.choice((-1, 1)) for v in rng.sample(range(1, n + 1), width)])
                check(n, clauses, "random_truth_table")

        for count in (63, 64, 65, 127, 128, 129, 257):
            check(4, [[1, -4]] * count + [[-1, 4]] * count, "wide_masks")
            check(4, [[1, -4]] * count + [[1], [-4], [-1, 4]], "wide_masks")

        for n, clauses in [(0, []), (0, [[]]), (7, []), (2, [[1, 1], [-1, 2, 2]]),
                           (2, [[1, -1], [2, -2]]), (3, [[]]), (1, [[1], [-1]]),
                           (6, [[1, 6], [-1, -6], [2, 5], [-2, -5]])]:
            check(n, clauses, "edge_cases")

        for modulus in (1, 2, 3, 7, 16, 31, 65, 257):
            for bits in (0, 1, 3, 7):
                for target in sorted({0, modulus // 2, modulus - 1}):
                    py = cg.solve_modulus(modulus, bits, target)
                    cpp = json.loads(subprocess.check_output([str(binary), "mod", str(modulus), str(bits), str(target), "--trace"], text=True))
                    expected = "ACCEPT" if any(x % modulus == target for x in range(1 << bits)) else "REJECT"
                    assert py["status"] == cpp["status"] == expected
                    for field in compare:
                        assert py[field] == cpp[field], (field, modulus, bits, target)
                    checked["modular"] += 1

        for bad in ["1 0\n", "p cnf 1 1\n2 0\n", "p cnf 1 1\n1\n",
                    "p cnf 1 2\n1 0\n", "p cnf 1 0\np cnf 1 0\n",
                    "p cnf -1 0\n", "p sat 1 0\n"]:
            cnf_path.write_text(bad)
            try:
                cg.read_dimacs(cnf_path)
                raise AssertionError("Python accepted invalid DIMACS")
            except ValueError:
                pass
            cpp = subprocess.run([str(binary), "cnf", str(cnf_path)], capture_output=True)
            assert cpp.returncode != 0
            checked["parser_rejections"] += 1

        # Multiline clauses, comments, blank lines, unused declared variables.
        cnf_path.write_text("c comment\n\np cnf 4 2\n1\n-2 0 3 0\n")
        assert cg.read_dimacs(cnf_path) == (4, [[1, -2], [3]])
        cpp = json.loads(subprocess.check_output([str(binary), "cnf", str(cnf_path)], text=True))
        assert cpp["status"] == "SAT" and cg.verify_assignment([[1, -2], [3]], cpp["witness"])
        checked["edge_cases"] += 1

        for flags, kwargs in [(["--max-frontier", "1"], {"max_frontier": 1}),
                              (["--timeout", "0.000000000001"], {"timeout": 1e-12})]:
            py = cg.solve_modulus(17, 8, 3, **kwargs)
            cpp = json.loads(subprocess.check_output([str(binary), "mod", "17", "8", "3"] + flags, text=True))
            assert py["status"] == cpp["status"] == "UNKNOWN"
            checked["resource_limits"] += 1
        varied = cg.explore_lengths(range(5), lambda length: (0, lambda i, r, b: 2*r+b, lambda r: r == 7))
        assert varied["status"] == "ACCEPT" and varied["length"] == 3 and varied["witness"] == [1, 1, 1]

    report = dict(passed=True, checks=checked, total_checks=sum(checked.values()) + 1,
                  sat_statuses=statuses, variable_length_test=True,
                  seconds=time.perf_counter() - started,
                  scope="truth tables + independent DPLL + identical Python/C++ states, transitions and witnesses")
    (ROOT / "validation.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
