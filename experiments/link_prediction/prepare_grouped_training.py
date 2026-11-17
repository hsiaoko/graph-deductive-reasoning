#!/usr/bin/env python3
"""Prepare at most one million signed training rows and grouped candidate tests."""

import argparse
import csv
import json
from pathlib import Path
import random
import shutil


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--train", required=True, type=Path,
                        help="Signed h/r/t/label training file")
    parser.add_argument("--test", required=True, type=Path,
                        help="Grouped candidate test file")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--max-samples", type=int, default=1_000_000)
    parser.add_argument("--seed", type=int, default=42)
    args = parser.parse_args()
    if args.max_samples < 2:
        parser.error("--max-samples must be at least 2")
    by_label = {0: [], 1: []}
    with args.train.open() as stream:
        for row in csv.reader(stream, delimiter="\t"):
            if len(row) != 4 or row[3] not in ("0", "1"):
                raise RuntimeError(f"bad signed training row: {row}")
            by_label[int(row[3])].append(row)
    rng = random.Random(args.seed)
    per_label = args.max_samples // 2
    selected = []
    for label in (0, 1):
        rows = by_label[label]
        if len(rows) > per_label:
            indices = sorted(rng.sample(range(len(rows)), per_label))
            rows = [rows[i] for i in indices]
        selected.extend(rows)
    rng.shuffle(selected)
    args.out.mkdir(parents=True, exist_ok=True)
    with (args.out / "train.txt").open("w") as stream:
        for row in selected:
            stream.write("\t".join(row) + "\n")
    shutil.copyfile(args.test, args.out / "test.txt")
    manifest = {
        "protocol": "max_one_million_signed_train_grouped_test_v1",
        "max_samples": args.max_samples, "seed": args.seed,
        "train_rows": len(selected),
        "train_positive": sum(int(row[3]) for row in selected),
        "train_negative": sum(1 - int(row[3]) for row in selected),
        "train_source": str(args.train.resolve()),
        "grouped_test_source": str(args.test.resolve()),
        "test_semantics": "one positive plus nine negative candidates per vertex",
    }
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
