#!/usr/bin/env python3
"""Create an observed-graph/teacher-label dataset from a complete GAR closure."""

import argparse
import csv
import hashlib
import json
from pathlib import Path

import yaml


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def read_triples(path):
    with path.open() as stream:
        return [tuple(row) for row in csv.reader(stream, delimiter="\t") if row]


def write_triples(path, triples):
    with path.open("w") as stream:
        for triple in sorted(triples):
            stream.write("\t".join(triple) + "\n")


def unit_hash(seed, text):
    digest = hashlib.sha256(f"{seed}|{text}".encode()).digest()
    return int.from_bytes(digest[:8], "little") / (1 << 64)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--observed", required=True, type=Path,
                        help="Observed h\\tr\\tt graph visible to students")
    parser.add_argument("--closure", required=True, type=Path,
                        help="Teacher closure_edges.tsv")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--valid-ratio", type=float, default=0.1)
    parser.add_argument("--test-ratio", type=float, default=0.1)
    parser.add_argument("--limit", type=int,
                        help="Deterministic development-only cap on teacher labels")
    args = parser.parse_args()
    if args.valid_ratio <= 0 or args.test_ratio <= 0 or \
            args.valid_ratio + args.test_ratio >= 1:
        parser.error("valid/test ratios must be positive and sum to less than 1")
    chase_summary = yaml.safe_load((args.closure.parent / "summary.yaml").read_text())
    if chase_summary.get("fixpoint") is not True:
        raise RuntimeError("teacher labels require a complete, uncapped closure")

    observed = read_triples(args.observed)
    observed_set = set(observed)
    labels = {}
    skipped_observed = 0
    with args.closure.open() as stream:
        for row in csv.reader(stream, delimiter="\t"):
            if not row or row[0].startswith("#"):
                continue
            h, r, t, round_no, rule_id = row[:5]
            triple = (h, r, t)
            if triple in observed_set:
                skipped_observed += 1
                continue
            labels.setdefault(triple, (int(round_no), rule_id))
    ordered = sorted(labels, key=lambda triple: unit_hash(
        args.seed, "label|" + "\t".join(triple)))
    if args.limit is not None:
        ordered = ordered[:args.limit]
    labels = {triple: labels[triple] for triple in ordered}

    splits = {"train": [], "valid": [], "test": []}
    groups = {"train": set(), "valid": set(), "test": set()}
    for triple, provenance in labels.items():
        h, r, _ = triple
        group = (h, r)
        value = unit_hash(args.seed, "query|" + "\t".join(group))
        if value < args.test_ratio:
            split = "test"
        elif value < args.test_ratio + args.valid_ratio:
            split = "valid"
        else:
            split = "train"
        splits[split].append((triple, provenance))
        groups[split].add(group)
    if groups["train"] & groups["valid"] or groups["train"] & groups["test"] or \
            groups["valid"] & groups["test"]:
        raise AssertionError("query-group split is not disjoint")

    args.out.mkdir(parents=True, exist_ok=True)
    write_triples(args.out / "observed.txt", observed)
    with (args.out / "provenance.tsv").open("w") as stream:
        writer = csv.writer(stream, delimiter="\t", lineterminator="\n")
        writer.writerow(["split", "head", "relation", "tail", "round", "teacher_rule"])
        for split in ("train", "valid", "test"):
            write_triples(args.out / f"{split}.txt", [item[0] for item in splits[split]])
            for (h, r, t), (round_no, rule_id) in splits[split]:
                writer.writerow([split, h, r, t, round_no, rule_id])
    manifest = {
        "protocol": "rule_distillation_query_group_v1",
        "label_semantics": "teacher GAR closure; imitation targets, not independent real-world GT",
        "observed_graph": "observed.txt",
        "student_graph_excludes_all_teacher_labels": True,
        "split_unit": "head/relation query group",
        "seed": args.seed,
        "valid_ratio": args.valid_ratio, "test_ratio": args.test_ratio,
        "observed_triples": len(observed_set),
        "teacher_labels": len(labels),
        "skipped_observed_labels": skipped_observed,
        "split_labels": {key: len(value) for key, value in splits.items()},
        "split_query_groups": {key: len(value) for key, value in groups.items()},
        "observed_sha256": sha256(args.observed),
        "closure_sha256": sha256(args.closure),
        "development_limit": args.limit,
    }
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
