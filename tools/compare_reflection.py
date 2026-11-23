#!/usr/bin/env python3
"""Compare the shader reflection of two backends (NCINE_DUMP_REFLECTION dumps). Exit 0 if compatible.

Compatible means: same programs; same uniform blocks with the same sizes; every block member the first
dump lists exists in the second at the same offset and type; same loose uniforms (names, types, array
sizes) and samplers; same vertex attributes by name. Loose offsets and attribute locations are backend
specific and not compared; built-in inputs (gl_*) are ignored.
"""
import sys
from collections import defaultdict


def load(path):
    programs = defaultdict(lambda: defaultdict(set))
    current = None
    for line in open(path):
        kind, *rest = line.split()
        if kind == "program":
            current = programs[rest[0]]
            continue
        if kind == "attribute":
            if rest[0].startswith("gl_"):
                continue
            rest = rest[:1]  # name only
        current[kind].add(tuple(rest))
    return programs


def main():
    a, b = load(sys.argv[1]), load(sys.argv[2])
    problems = []
    if set(a) != set(b):
        problems.append(f"program sets differ: only first {sorted(set(a) - set(b))}, only second {sorted(set(b) - set(a))}")
    for name in sorted(set(a) & set(b)):
        for kind in ("block", "loose", "sampler", "attribute"):
            if a[name][kind] != b[name][kind]:
                problems.append(f"{name} {kind}: first {sorted(a[name][kind])} second {sorted(b[name][kind])}")
        missing = a[name]["member"] - b[name]["member"]
        if missing:
            problems.append(f"{name} members of the first missing or different in the second: {sorted(missing)}")
    for p in problems:
        print("MISMATCH", p)
    print(f"programs={len(set(a) & set(b))} problems={len(problems)}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
