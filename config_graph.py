#!/usr/bin/env python3
"""Configuration reachability from Section 3 of Kaoru's 2026-10-01 paper.

The engine merges identical (certificate position, verifier memory) states.
The CNF adapter is an explicit additional verifier, not an implementation of
an unspecified holographic SAT verifier. See README.md for the distinction.
No variable-count limit. Standard library only.
"""
from __future__ import annotations

import argparse
from array import array
import heapq
import json
import time
from pathlib import Path
from typing import Callable, Hashable, TypeVar

State = TypeVar("State", bound=Hashable)


def explore(length: int, start: State, step: Callable[[int, State, int], State | None],
            accept: Callable[[State], bool], *, witness: bool = True,
            timeout: float = 0, max_frontier: int = 0) -> dict:
    """Exact layered reachability for a deterministic, one-pass verifier.

    step(i, memory, bit) contracts deterministic work between successive reads.
    None rejects a branch. Every part of memory affecting future computation
    MUST be in State; i is already part of the configuration via its layer.
    Limits are optional and yield UNKNOWN, never a false REJECT.
    """
    if length < 0 or timeout < 0 or max_frontier < 0:
        raise ValueError("length and resource limits must be nonnegative")
    begin = time.perf_counter()
    current = [start]
    parents = []
    widths = [1]
    stats = dict(configurations=1, attempted_edges=0, valid_edges=0,
                 merged_edges=0, peak_frontier=1, completed_layers=0)

    def finish(status, certificate=None, reason=None):
        return dict(status=status, witness=certificate, reason=reason,
                    graph_seconds=time.perf_counter() - begin,
                    widths=widths, **stats)

    for i in range(length):
        if timeout and time.perf_counter() - begin >= timeout:
            return finish("UNKNOWN", reason="timeout")
        index = {}
        following = []
        links = array("Q") if witness else None
        for parent_index, state in enumerate(current):
            if timeout and parent_index % 1024 == 0 and time.perf_counter() - begin >= timeout:
                return finish("UNKNOWN", reason="timeout")
            for bit in (0, 1):
                stats["attempted_edges"] += 1
                child = step(i, state, bit)
                if child is None:
                    continue
                stats["valid_edges"] += 1
                if child in index:
                    stats["merged_edges"] += 1
                    continue
                if max_frontier and len(following) >= max_frontier:
                    return finish("UNKNOWN", reason="max_frontier")
                index[child] = len(following)
                following.append(child)
                stats["configurations"] += 1
                if links is not None:
                    links.append((parent_index << 1) | bit)
        current = following
        # The dictionary is only needed during construction of this layer.
        del index
        if links is not None:
            parents.append(links)
        widths.append(len(current))
        stats["peak_frontier"] = max(stats["peak_frontier"], len(current))
        stats["completed_layers"] = i + 1
        if not current:
            return finish("REJECT")
    for final_index, state in enumerate(current):
        if accept(state):
            bits = None
            if witness:
                bits = [0] * length
                for i in range(length - 1, -1, -1):
                    link = parents[i][final_index]
                    bits[i] = link & 1
                    final_index = link >> 1
            return finish("ACCEPT", bits)
    return finish("REJECT")


def explore_lengths(lengths, factory, **kwargs):
    """Enumerate allowed certificate lengths as specified in the paper.

    factory(L) returns (start, step, accept). A fixed-length verifier uses explore.
    Returns all attempts, retaining UNKNOWN if no accepting length was found.
    """
    attempts = []
    for length in lengths:
        start, step, accept = factory(length)
        result = explore(length, start, step, accept, **kwargs)
        attempts.append(dict(length=length, **result))
        if result["status"] == "ACCEPT":
            return dict(status="ACCEPT", length=length, witness=result["witness"], attempts=attempts)
    status = "UNKNOWN" if any(r["status"] == "UNKNOWN" for r in attempts) else "REJECT"
    return dict(status=status, length=None, witness=None, attempts=attempts)


def read_dimacs(path: str | Path):
    n = declared = None
    clauses, pending = [], []
    for line_number, raw in enumerate(Path(path).read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("c"):
            continue
        if line.startswith("p"):
            parts = line.split()
            if n is not None or len(parts) != 4 or parts[:2] != ["p", "cnf"]:
                raise ValueError(f"invalid DIMACS header at line {line_number}")
            n, declared = int(parts[2]), int(parts[3])
            if min(n, declared) < 0:
                raise ValueError("negative DIMACS size")
            continue
        if n is None:
            raise ValueError("missing p cnf header")
        for word in line.split():
            literal = int(word)
            if literal == 0:
                clauses.append(pending)
                pending = []
            elif abs(literal) > n:
                raise ValueError(f"literal {literal} exceeds declared variable count")
            else:
                pending.append(literal)
    if n is None or pending or len(clauses) != declared:
        raise ValueError("missing header, unterminated clause, or clause-count mismatch")
    return n, clauses


def prepare_cnf(n: int, clauses):
    """Compile a left-to-right assignment verifier into clause-status masks.

    One live bit records whether a clause has already been satisfied. On its
    last variable it must be true, then its slot is released and can be reused.
    This forgetting is sound because that clause can no longer affect acceptance.
    """
    if n < 0:
        raise ValueError("negative variable count")
    normalized = []
    empty = False
    for clause in clauses:
        if any(lit == 0 or abs(lit) > n for lit in clause):
            raise ValueError("invalid literal")
        literals = set(clause)
        if not literals:
            empty = True
        elif not any(-lit in literals for lit in literals):
            normalized.append(sorted(literals, key=lambda lit: (abs(lit), lit)))
    starts = [[] for _ in range(n)]
    ends = [[] for _ in range(n)]
    for j, clause in enumerate(normalized):
        starts[abs(clause[0]) - 1].append(j)
        ends[abs(clause[-1]) - 1].append(j)
    positive, negative, close = [0] * n, [0] * n, [0] * n
    free, assigned = [], [0] * len(normalized)
    slots = 0
    for i in range(n):
        for j in starts[i]:
            if free:
                slot = heapq.heappop(free)
            else:
                slot = slots
                slots += 1
            assigned[j] = slot
            bit = 1 << slot
            for literal in normalized[j]:
                masks = positive if literal > 0 else negative
                masks[abs(literal) - 1] |= bit
            close[abs(normalized[j][-1]) - 1] |= bit
        for j in ends[i]:
            heapq.heappush(free, assigned[j])
    return positive, negative, close, slots, len(normalized), empty


def verify_assignment(clauses, bits):
    return all(any(bool(bits[abs(lit) - 1]) == (lit > 0) for lit in clause)
               for clause in clauses)


def solve_cnf(n: int, clauses, **kwargs):
    begin = time.perf_counter()
    positive, negative, close, width, count, empty = prepare_cnf(n, clauses)
    preparation = time.perf_counter() - begin

    def step(i, state, bit):
        satisfied = state | (positive[i] if bit else negative[i])
        ending = close[i]
        if satisfied & ending != ending:
            return None
        return satisfied ^ ending

    result = explore(0 if empty else n, 0, step, lambda _: not empty, **kwargs)
    result["status"] = {"ACCEPT": "SAT", "REJECT": "UNSAT", "UNKNOWN": "UNKNOWN"}[result["status"]]
    result.update(variables=n, clauses=len(clauses), normalized_clauses=count,
                  state_bits_bound=width, prepare_seconds=preparation,
                  mode="cnf", engine="python")
    if result["status"] == "SAT" and result["witness"] is not None:
        if not verify_assignment(clauses, result["witness"]):
            raise AssertionError("internal error: certificate does not satisfy original CNF")
        result["witness_verified"] = True
    else:
        result["witness_verified"] = None
    result["total_seconds"] = time.perf_counter() - begin
    return result


def solve_modulus(modulus: int, bits: int, target: int, **kwargs):
    """Control: does a bits-long binary string have the requested remainder?"""
    if modulus <= 0 or bits < 0 or not 0 <= target < modulus:
        raise ValueError("require modulus > 0, bits >= 0, 0 <= target < modulus")
    result = explore(bits, 0, lambda i, r, b: (2 * r + b) % modulus,
                     lambda r: r == target, **kwargs)
    result.update(mode="mod", engine="python", modulus=modulus, bits=bits,
                  target=target, state_bits_bound=(modulus - 1).bit_length(),
                  prepare_seconds=0.0)
    if result["witness"] is not None:
        r = 0
        for bit in result["witness"]:
            r = (2 * r + bit) % modulus
        if r != target:
            raise AssertionError("invalid modular witness")
        result["witness_verified"] = True
    else:
        result["witness_verified"] = None
    return result


def peak_rss_kib():
    # Linux VmHWM belongs to the current address space, avoiding a pre-exec
    # ru_maxrss floor inherited from the Python benchmark launcher.
    status = Path("/proc/self/status")
    if status.exists():
        for line in status.read_text().splitlines():
            if line.startswith("VmHWM:"):
                return int(line.split()[1])
    try:
        import resource
        import sys
        rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        return rss / 1024 if sys.platform == "darwin" else rss
    except ImportError:
        return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="mode", required=True)
    cnf = sub.add_parser("cnf", help="solve a DIMACS CNF")
    cnf.add_argument("file")
    mod = sub.add_parser("mod", help="bounded-memory verifier control")
    mod.add_argument("modulus", type=int)
    mod.add_argument("bits", type=int)
    mod.add_argument("target", type=int)
    for p in (cnf, mod):
        p.add_argument("--timeout", type=float, default=0, help="graph seconds; 0 = unlimited")
        p.add_argument("--max-frontier", type=int, default=0, help="optional; 0 = unlimited")
        p.add_argument("--decision-only", action="store_true", help="omit witness parent storage")
        p.add_argument("--trace", action="store_true", help="include every layer width")
        p.add_argument("--output", help="also write JSON result to this file")
    args = parser.parse_args()
    if args.timeout < 0 or args.max_frontier < 0:
        parser.error("resource limits must be nonnegative")
    start = time.perf_counter()
    options = dict(witness=not args.decision_only, timeout=args.timeout, max_frontier=args.max_frontier)
    try:
        if args.mode == "cnf":
            n, clauses = read_dimacs(args.file)
            result = solve_cnf(n, clauses, **options)
        else:
            result = solve_modulus(args.modulus, args.bits, args.target, **options)
    except (ValueError, OSError) as error:
        parser.error(str(error))
    result["total_seconds"] = time.perf_counter() - start
    result["peak_rss_kib"] = peak_rss_kib()
    if not args.trace:
        result.pop("widths", None)
    line = json.dumps(result, separators=(",", ":"))
    if args.output:
        Path(args.output).write_text(line + "\n", encoding="utf-8")
    print(line)


if __name__ == "__main__":
    main()
