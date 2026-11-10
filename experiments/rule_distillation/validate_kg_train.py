#!/usr/bin/env python3
"""Validate original-graph-test rule-teacher training datasets."""

import argparse
import csv
import json
from pathlib import Path

import yaml


def triples_with_labels(path):
    with path.open() as stream:
        for line_no, row in enumerate(csv.reader(stream, delimiter="\t"), 1):
            if len(row) != 4 or row[3] not in ("0", "1"):
                raise RuntimeError(f"{path}:{line_no}: expected h/r/t/label")
            yield tuple(row[:3]), int(row[3])


def triples(path):
    with path.open() as stream:
        for line_no, row in enumerate(csv.reader(stream, delimiter="\t"), 1):
            if len(row) != 3:
                raise RuntimeError(f"{path}:{line_no}: expected h/r/t")
            yield tuple(row)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("roots", nargs="+", type=Path)
    args = parser.parse_args()
    for root in args.roots:
        manifest = json.loads((root / "manifest.json").read_text())
        test = set(triples(root / "test.txt"))
        if len(test) != manifest["original_test_triples"]:
            raise RuntimeError(f"{root}: test count mismatch")
        graph = Path(manifest["graph"])
        meta = yaml.safe_load((graph / "meta.yaml").read_text())
        if len(test) != int(meta["num_edges"]):
            raise RuntimeError(f"{root}: test is not the original graph")
        positive, negative = set(), set()
        labels = [0, 0]
        for triple, label in triples_with_labels(root / "train.txt"):
            target = positive if label else negative
            if triple in target:
                raise RuntimeError(f"{root}: duplicate training triple")
            target.add(triple)
            labels[label] += 1
        if positive & negative:
            raise RuntimeError(f"{root}: positive/negative conflict")
        if negative & test:
            raise RuntimeError(f"{root}: negative sample appears in original test graph")
        if len(positive) != manifest["positive_train_triples"] or \
                len(negative) != manifest["negative_train_triples"]:
            raise RuntimeError(f"{root}: train counts do not match manifest")
        if set(triples(root / "positives.txt")) != positive or \
                set(triples(root / "negatives.txt")) != negative:
            raise RuntimeError(f"{root}: split files differ from train.txt")
        with (root / "provenance.tsv").open() as stream:
            provenance_rows = sum(1 for _ in stream) - 1
        if provenance_rows != len(positive):
            raise RuntimeError(f"{root}: provenance count mismatch")
        if not (root / "rules").is_dir():
            raise RuntimeError(f"{root}: missing rules directory")
        result = {"root": str(root), "status": "PASS",
                  "train_rows": len(positive) + len(negative),
                  "positive": len(positive), "negative": len(negative),
                  "test": len(test), "provenance": provenance_rows,
                  "closure_complete": manifest["closure_complete"]}
        print(json.dumps(result), flush=True)


if __name__ == "__main__":
    main()
