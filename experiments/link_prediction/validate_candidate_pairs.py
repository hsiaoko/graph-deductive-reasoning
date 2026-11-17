#!/usr/bin/env python3
"""Validate one-positive/nine-negative candidate groups against the original graph."""

import argparse
import csv
import json
from pathlib import Path
import struct


def names(path):
    with path.open() as stream:
        rows = [(int(i), name) for i, name in csv.reader(stream, delimiter="\t")]
    return {name: i for i, name in rows}


def edges(path):
    with path.open("rb") as stream:
        while block := stream.read(12):
            if len(block) != 12:
                raise RuntimeError(f"truncated edge file: {path}")
            yield struct.unpack("<3I", block)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--graph", required=True, type=Path)
    parser.add_argument("--data", required=True, type=Path)
    args = parser.parse_args()
    manifest = json.loads((args.data / "manifest.json").read_text())
    entity_id = names(args.graph / "entities.tsv")
    relation_id = names(args.graph / "relations.tsv")
    outgoing = [set() for _ in entity_id]
    incoming = [set() for _ in entity_id]
    for h, r, t in edges(args.graph / "edges.bin"):
        outgoing[h].add((t, r))
        incoming[t].add((h, r))
    current_group = None
    rows = []
    positive = 0
    negative = 0
    groups = 0

    def validate_group(group_rows):
        nonlocal positive, negative, groups
        expected = manifest["candidates_per_group"]
        if len(group_rows) != expected:
            raise RuntimeError(f"group {current_group}: {len(group_rows)} rows, expected {expected}")
        if sum(int(row[5]) for row in group_rows) != 1:
            raise RuntimeError(f"group {current_group}: expected exactly one positive")
        center = int(current_group)
        seen = set()
        for _, _, h, r, t, label in group_rows:
            h_id, r_id, t_id = entity_id[h], relation_id[r], entity_id[t]
            key = (h_id, r_id, t_id)
            if key in seen:
                raise RuntimeError(f"group {current_group}: duplicate candidate")
            seen.add(key)
            if label == "1":
                if (t_id, r_id) not in outgoing[h_id]:
                    raise RuntimeError(f"group {current_group}: positive is not in original graph")
            else:
                if (t_id, r_id) in outgoing[h_id]:
                    raise RuntimeError(f"group {current_group}: negative is an original edge")
            if h_id != center and t_id != center:
                raise RuntimeError(f"group {current_group}: candidate misses center")
        positive += 1
        negative += len(group_rows) - 1
        groups += 1

    with (args.data / "test.txt").open() as stream:
        for row in csv.reader(stream, delimiter="\t"):
            if not row or row[0].startswith("#"):
                continue
            if len(row) != 6:
                raise RuntimeError(f"bad candidate row: {row}")
            if current_group is None:
                current_group = row[0]
            if row[0] != current_group:
                validate_group(rows)
                rows = []
                current_group = row[0]
            rows.append(row)
    if rows:
        validate_group(rows)
    if groups != manifest["groups"] or positive != manifest["positive"] or \
            negative != manifest["negative"]:
        raise RuntimeError("candidate counts do not match manifest")
    print(json.dumps({"status": "PASS", "groups": groups, "positive": positive,
                      "negative": negative, "rows": positive + negative,
                      "skipped_isolated": manifest["skipped_isolated"],
                      "skipped_insufficient_candidates":
                          manifest["skipped_insufficient_candidates"]}, indent=2))


if __name__ == "__main__":
    main()
