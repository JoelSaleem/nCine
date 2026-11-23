#!/usr/bin/env python3
"""Compare a perf sample against baseline; flag regressions over a percent threshold.

Exit 0 ok, 1 regression (unless --advisory), 2 nothing comparable (always fails: an empty
baseline or sample means the harness is broken, not that performance is fine)."""
import argparse
import json
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("baseline")
    ap.add_argument("sample")
    ap.add_argument("--pct", type=float, default=10.0)
    ap.add_argument("--advisory", action="store_true", help="report regressions without failing")
    args = ap.parse_args()
    with open(args.baseline) as f:
        base = json.load(f)
    with open(args.sample) as f:
        samp = json.load(f)

    compared = 0
    worst = 0.0
    bad = []
    for name, sv in sorted(samp.items()):
        b = base.get(name, {}).get("frame_ms_avg", 0.0)
        s = sv.get("frame_ms_avg", 0.0)
        if b <= 0 or s <= 0:
            continue
        compared += 1
        delta = (s - b) / b * 100.0
        worst = max(worst, delta)
        if delta > args.pct:
            bad.append(f"{name}: {b:.3f}->{s:.3f}ms (+{delta:.1f}%)")

    if compared == 0:
        print(f"NO COMPARABLE DATA: baseline has {len(base)} entries, sample has {len(samp)}")
        return 2
    for line in bad:
        print("REGRESSION", line)
    print(f"compared={compared} worst_delta={worst:.1f}% threshold={args.pct}%{' (advisory)' if args.advisory else ''}")
    if args.advisory:
        return 0
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
