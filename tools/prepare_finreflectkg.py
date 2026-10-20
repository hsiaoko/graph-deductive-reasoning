#!/usr/bin/env python3
"""Prepare FinReflectKG parquet shards for gar_miner's TripleGraph.

Reads the FinReflectKG parquet shards, dictionary-encodes entities and
relations to uint32, deduplicates triples, and writes:

    <out>/entities.tsv    id \t entity_name
    <out>/relations.tsv   id \t relation_name
    <out>/edges.bin       uint32 triples [src, rel, dst] * m
    <out>/meta.yaml       counts

Usage:
    python3 tools/prepare_finreflectkg.py \
        --src /path/to/FinReflectKG/data --out data/finreflectkg \
        [--shards N] [--max-rows M]
"""

import argparse
import glob
import os
import struct
import sys

import pyarrow.parquet as pq


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, help="directory with train-*.parquet shards")
    ap.add_argument("--out", required=True, help="output directory")
    ap.add_argument("--shards", type=int, default=0, help="use only the first N shards (0 = all)")
    ap.add_argument("--max-rows", type=int, default=0, help="row cap across shards (0 = no cap)")
    args = ap.parse_args()

    shards = sorted(glob.glob(os.path.join(args.src, "train-*.parquet")))
    if not shards:
        print(f"no parquet shards found in {args.src}", file=sys.stderr)
        return 1
    if args.shards > 0:
        shards = shards[: args.shards]
    print(f"reading {len(shards)} shard(s)")

    entity_id = {}
    relation_id = {}
    edges = set()
    rows = 0
    for shard in shards:
        table = pq.read_table(shard, columns=["entity", "relationship", "target"])
        for ent, rel, tgt in zip(
            table.column("entity").to_pylist(),
            table.column("relationship").to_pylist(),
            table.column("target").to_pylist(),
        ):
            if ent is None or rel is None or tgt is None:
                continue
            s = entity_id.setdefault(ent, len(entity_id))
            r = relation_id.setdefault(rel, len(relation_id))
            d = entity_id.setdefault(tgt, len(entity_id))
            edges.add((s, r, d))
            rows += 1
            if args.max_rows and rows >= args.max_rows:
                break
        print(f"  {os.path.basename(shard)}: rows so far {rows}, "
              f"entities {len(entity_id)}, relations {len(relation_id)}, deduped edges {len(edges)}")
        if args.max_rows and rows >= args.max_rows:
            break

    os.makedirs(args.out, exist_ok=True)
    inv_entities = sorted(entity_id.items(), key=lambda kv: kv[1])
    inv_relations = sorted(relation_id.items(), key=lambda kv: kv[1])
    with open(os.path.join(args.out, "entities.tsv"), "w") as f:
        for name, i in inv_entities:
            f.write(f"{i}\t{name}\n")
    with open(os.path.join(args.out, "relations.tsv"), "w") as f:
        for name, i in inv_relations:
            f.write(f"{i}\t{name}\n")
    with open(os.path.join(args.out, "edges.bin"), "wb") as f:
        for s, r, d in sorted(edges):
            f.write(struct.pack("<3I", s, r, d))
    with open(os.path.join(args.out, "meta.yaml"), "w") as f:
        f.write(f"num_entities: {len(entity_id)}\n")
        f.write(f"num_relations: {len(relation_id)}\n")
        f.write(f"num_edges: {len(edges)}\n")
        f.write(f"rows_read: {rows}\n")
        f.write(f"shards: {len(shards)}\n")

    print(f"done: |V|={len(entity_id)} |R|={len(relation_id)} |E|={len(edges)} -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
