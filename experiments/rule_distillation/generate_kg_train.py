#!/usr/bin/env python3
"""Build positive/negative rule-distillation data and use the original KG as test."""

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import random
import struct

import yaml


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 << 20), b""):
            digest.update(block)
        return digest.hexdigest()


def dictionaries(graph):
    with (graph / "entities.tsv").open() as stream:
        entities = {name: int(i) for i, name in csv.reader(stream, delimiter="\t")}
    with (graph / "relations.tsv").open() as stream:
        relations = {name: int(i) for i, name in csv.reader(stream, delimiter="\t")}
    entity_names = [None] * len(entities)
    relation_names = [None] * len(relations)
    for name, i in entities.items():
        entity_names[i] = name
    for name, i in relations.items():
        relation_names[i] = name
    return entities, relations, entity_names, relation_names


def edges(graph):
    with (graph / "edges.bin").open("rb") as stream:
        while block := stream.read(12):
            if len(block) != 12:
                raise RuntimeError("truncated edges.bin")
            yield struct.unpack("<3I", block)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--graph", required=True, type=Path)
    parser.add_argument("--closure", required=True, type=Path)
    parser.add_argument("--rules", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--negative-ratio", type=int, default=1)
    parser.add_argument("--max-positive", type=int,
                        help="Positive cap; also permits an explicitly marked capped closure")
    args = parser.parse_args()
    if args.negative_ratio < 1:
        parser.error("--negative-ratio must be at least 1")
    chase_summary = yaml.safe_load((args.closure.parent / "summary.yaml").read_text())
    closure_complete = chase_summary.get("fixpoint") is True
    if not closure_complete and args.max_positive is None:
        raise RuntimeError("a partial closure requires an explicit --max-positive cap")

    entity_id, relation_id, entity_names, relation_names = dictionaries(args.graph)
    positives = []
    provenance = []
    with args.closure.open() as stream:
        for row in csv.reader(stream, delimiter="\t"):
            if not row or row[0].startswith("#"):
                continue
            h, r, t, round_no, rule_id = row[:5]
            if r not in relation_id:
                relation_names.append(r)
                relation_id[r] = len(relation_names) - 1
            positives.append((entity_id[h], relation_id[r], entity_id[t]))
            provenance.append((int(round_no), rule_id))
            if args.max_positive is not None and len(positives) >= args.max_positive:
                break
    if not positives:
        raise RuntimeError("the closure contains no positive labels")
    n = len(entity_names)
    r = len(relation_names)
    known = set()

    def key(triple):
        h, rel, t = triple
        return (h * r + rel) * n + t

    original_count = 0
    for triple in edges(args.graph):
        known.add(key(triple))
        original_count += 1
    for triple in positives:
        known.add(key(triple))

    rng = random.Random(args.seed)
    negatives = []
    negative_keys = set()
    for h, rel, t in positives:
        for _ in range(args.negative_ratio):
            for _attempt in range(1000):
                candidate = (rng.randrange(n), rel, t) if rng.getrandbits(1) else \
                            (h, rel, rng.randrange(n))
                candidate_key = key(candidate)
                if candidate_key not in known and candidate_key not in negative_keys:
                    negative_keys.add(candidate_key)
                    negatives.append(candidate)
                    break
            else:
                raise RuntimeError("cannot sample a unique negative after 1000 attempts")

    args.out.mkdir(parents=True, exist_ok=True)
    rules_link = args.out / "rules"
    if not rules_link.exists():
        target = os.path.relpath(args.rules.resolve(), args.out.resolve())
        rules_link.symlink_to(target, target_is_directory=True)
    elif not rules_link.is_symlink() or rules_link.resolve() != args.rules.resolve():
        raise RuntimeError(f"unexpected existing rules path: {rules_link}")

    def write_labelled(path, items, label):
        with path.open("w") as stream:
            for h, rel, t in items:
                stream.write(f"{entity_names[h]}\t{relation_names[rel]}\t{entity_names[t]}\t{label}\n")

    def write_plain(path, items):
        with path.open("w") as stream:
            for h, rel, t in items:
                stream.write(f"{entity_names[h]}\t{relation_names[rel]}\t{entity_names[t]}\n")

    write_plain(args.out / "positives.txt", positives)
    write_plain(args.out / "negatives.txt", negatives)
    with (args.out / "train.txt").open("w") as stream:
        for i, positive in enumerate(positives):
            for j in range(args.negative_ratio):
                negative = negatives[i * args.negative_ratio + j]
                for triple, label in ((positive, 1), (negative, 0)):
                    h, rel, t = triple
                    stream.write(f"{entity_names[h]}\t{relation_names[rel]}\t"
                                 f"{entity_names[t]}\t{label}\n")
    with (args.out / "provenance.tsv").open("w") as stream:
        writer = csv.writer(stream, delimiter="\t", lineterminator="\n")
        writer.writerow(["head", "relation", "tail", "round", "teacher_rule"])
        for (h, rel, t), (round_no, rule_id) in zip(positives, provenance):
            writer.writerow([entity_names[h], relation_names[rel], entity_names[t],
                             round_no, rule_id])
    with (args.out / "test.txt").open("w") as stream:
        for h, rel, t in edges(args.graph):
            stream.write(f"{entity_names[h]}\t{relation_names[rel]}\t{entity_names[t]}\n")
    manifest = {
        "protocol": "original_graph_test_rule_teacher_train_v1",
        "train_format": "head<TAB>relation<TAB>tail<TAB>label (1=deduced, 0=sampled)",
        "test_format": "head<TAB>relation<TAB>tail (all original KG edges)",
        "label_semantics": "rule-teacher distillation; not independent real-world GT",
        "positive_source": ("complete rule deduction on the original graph"
                            if closure_complete else
                            "capped partial rule deduction on the original graph"),
        "closure_complete": closure_complete,
        "data_status": "complete_closure" if closure_complete else "capped_partial_closure",
        "negative_source": "random head/tail corruption, excluding original and deduced positives",
        "negative_ratio": args.negative_ratio,
        "seed": args.seed,
        "original_test_triples": original_count,
        "positive_train_triples": len(positives),
        "negative_train_triples": len(negatives),
        "train_rows": len(positives) + len(negatives),
        "max_positive": args.max_positive,
        "graph": str(args.graph.resolve()),
        "closure": str(args.closure.resolve()),
        "rules": str(args.rules.resolve()),
        "graph_edges_sha256": sha256(args.graph / "edges.bin"),
        "closure_sha256": sha256(args.closure),
    }
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
