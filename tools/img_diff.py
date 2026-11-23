#!/usr/bin/env python3
"""Tolerance-based PNG comparison. Exit 0 if within tolerance, 1 if not, 2 if an image can't be read."""
import argparse
import sys

import numpy as np
from PIL import Image


def load_rgb(path):
    try:
        return np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)
    except (OSError, ValueError) as e:
        print(f"CANNOT READ {path}: {e}")
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--tol", type=int, default=0, help="max per-channel delta allowed")
    ap.add_argument("--max-frac", type=float, default=0.005, help="max fraction of pixels over tol")
    ap.add_argument("--diff-out", default=None, help="write an amplified difference image here")
    args = ap.parse_args()

    a, b = load_rgb(args.a), load_rgb(args.b)
    if a is None or b is None:
        return 2
    if a.shape != b.shape:
        print(f"SIZE MISMATCH {a.shape[1]}x{a.shape[0]} vs {b.shape[1]}x{b.shape[0]}")
        return 1

    delta = np.abs(a - b).max(axis=2)
    over = int((delta > args.tol).sum())
    total = delta.size
    frac = over / total
    if args.diff_out:
        Image.fromarray(np.clip(delta * 8, 0, 255).astype(np.uint8)).save(args.diff_out)
    print(f"over_tol={over}/{total} ({frac:.4%}) max_delta={int(delta.max())} tol={args.tol} max_frac={args.max_frac}")
    return 0 if frac <= args.max_frac else 1


if __name__ == "__main__":
    sys.exit(main())
