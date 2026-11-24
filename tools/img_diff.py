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
    ap.add_argument("--max-blob", type=int, default=0,
                    help="also pass when the largest connected region of differing pixels is at most this big "
                         "and no more than --blob-max-frac of the pixels differ (rasterization noise, not geometry)")
    ap.add_argument("--blob-max-frac", type=float, default=0.001)
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
    if frac <= args.max_frac:
        print(f"over_tol={over}/{total} ({frac:.4%}) max_delta={int(delta.max())} tol={args.tol} max_frac={args.max_frac}")
        return 0
    blob = largest_blob(delta > args.tol) if args.max_blob > 0 else 0
    print(f"over_tol={over}/{total} ({frac:.4%}) max_delta={int(delta.max())} largest_blob={blob} tol={args.tol} max_frac={args.max_frac}")
    return 0 if (args.max_blob > 0 and blob <= args.max_blob and frac <= args.blob_max_frac) else 1


def largest_blob(mask):
    """Size of the largest 8-connected region of True pixels."""
    seen = np.zeros_like(mask)
    height, width = mask.shape
    largest = 0
    for y, x in zip(*np.nonzero(mask)):
        if seen[y, x]:
            continue
        seen[y, x] = True
        stack, size = [(y, x)], 0
        while stack:
            cy, cx = stack.pop()
            size += 1
            for ny in range(max(cy - 1, 0), min(cy + 2, height)):
                for nx in range(max(cx - 1, 0), min(cx + 2, width)):
                    if mask[ny, nx] and not seen[ny, nx]:
                        seen[ny, nx] = True
                        stack.append((ny, nx))
        largest = max(largest, size)
    return largest


if __name__ == "__main__":
    sys.exit(main())
