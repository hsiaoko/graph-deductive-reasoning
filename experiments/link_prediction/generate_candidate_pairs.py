#!/usr/bin/env python3
"""Generate ten candidate pairs per non-isolated vertex (one positive, nine negatives)."""

import argparse
import csv
import json
from pathlib import Path
import random
import struct


def names(path):
    with path.open() as stream:
        rows = [(int(i), name) for i, name in csv.reader(stream, delimiter="\t")]
    return [name for _, name in sorted(rows)]


def edges(path):
    with path.open("rb") as stream:
        while block := stream.read(12):
            if len(block) != 12:
                raise RuntimeError(f"truncated edge file: {path}")
            yield struct.unpack("<3I", block)


def sha256(path):
    import hashlib
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--graph", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--candidates", type=int, default=10)
    args = parser.parse_args()
    if args.candidates < 2:
        parser.error("--candidates must be at least 2")
    entity_names = names(args.graph / "entities.tsv")
    relation_names = names(args.graph / "relations.tsv")
    outgoing = [set() for _ in entity_names]
    incoming = [set() for _ in entity_names]
    original_edges = 0
    for h, r, t in edges(args.graph / "edges.bin"):
        outgoing[h].add((t, r))
        incoming[t].add((h, r))
        original_edges += 1

    args.out.mkdir(parents=True, exist_ok=True)
    stats = {
        "vertices": len(entity_names), "groups": 0, "rows": 0,
        "skipped_isolated": 0, "skipped_insufficient_candidates": 0,
    }
    with (args.out / "test.txt").open("w") as stream:
        stream.write("# group_id\tcenter\tsrc\trelation\tdst\tlabel\n")
        for center in range(len(entity_names)):
            direction = 1 if outgoing[center] else -1 if incoming[center] else 0
            if direction == 0:
                stats["skipped_isolated"] += 1
                continue
            positive_pool = outgoing[center] if direction == 1 else incoming[center]
            rng = random.Random(args.seed + center)
            positive_other, relation = rng.choice(sorted(positive_pool))
            if direction == 1:
                positive = (center, relation, positive_other)
            else:
                positive = (positive_other, relation, center)
            used = set(positive_pool)
            candidates = [positive]
            attempts = 0
            while len(candidates) < args.candidates and attempts < 1000:
                attempts += 1
                other = rng.randrange(len(entity_names))
                if (other, relation) in used:
                    continue
                used.add((other, relation))
                if direction == 1:
                    candidates.append((center, relation, other))
                else:
                    candidates.append((other, relation, center))
            if len(candidates) < args.candidates:
                stats["skipped_insufficient_candidates"] += 1
                continue
            for h, r, t in candidates:
                label = int((h, r, t) == positive)
                stream.write(f"{center}\t{center}\t{entity_names[h]}\t"
                             f"{relation_names[r]}\t{entity_names[t]}\t{label}\n")
            stats["groups"] += 1
            stats["rows"] += len(candidates)
    manifest = {
        "protocol": "one_positive_nine_negative_candidates_v1",
        "schema": "group_id, center, src, relation, dst, label",
        "candidates_per_group": args.candidates,
        "positive_per_group": 1,
        "negative_per_group": args.candidates - 1,
        "positive_source": "one original edge per non-isolated vertex",
        "negative_source": "random endpoint, excluding all original edges of that vertex",
        "seed": args.seed,
        "original_edges": original_edges,
        "graph_edges_sha256": sha256(args.graph / "edges.bin"),
        "label_semantics": "original-graph held-out candidates, not rule-teacher labels",
        **stats,
    }
    manifest["positive"] = stats["groups"]
    manifest["negative"] = stats["groups"] * (args.candidates - 1)
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
