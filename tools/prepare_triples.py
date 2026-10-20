#!/usr/bin/env python3
"""Convert TSV / N-Triples triple files to gar_miner's TripleGraph format.

Outputs to <out>:
    entities.tsv    id \t entity_name
    relations.tsv   id \t relation_name
    edges.bin       uint32 triples [src, rel, dst] * m
    meta.yaml       counts

Usage:
    # FB15k-237 (tab-separated, no header)
    python3 tools/prepare_triples.py --format tsv \
        --in Release/train.txt Release/valid.txt --out data/fb15k237

    # OGB biokg (tab-separated with header row)
    python3 tools/prepare_triples.py --format tsv --skip-header \
        --in biokg/train.tsv --out data/ogbl_biokg

    # DBpedia N-Triples (only URI objects are kept, names are shortened
    # to the local part after the last '/' or '#')
    python3 tools/prepare_triples.py --format nt \
        --in mappingbased_objects_en.ttl --out data/dbpedia

    # OGB biokg (root dir containing raw/ and mapping/)
    python3 tools/prepare_triples.py --format ogb-biokg \
        --in biokg --out data/ogbl_biokg
"""

import argparse
import csv
import gzip
import os
import re
import struct
import sys

NT_RE = re.compile(r"^<([^>]*)>\s+<([^>]*)>\s+(<([^>]*)>|\".*\")\s*\.\s*$")


def local_name(uri: str) -> str:
    for sep in ("/", "#"):
        if sep in uri:
            uri = uri.rsplit(sep, 1)[-1]
    return uri


def iter_tsv(path, skip_header):
    with open(path, encoding="utf-8", errors="replace") as f:
        for i, line in enumerate(f):
            if i == 0 and skip_header:
                continue
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 3:
                continue
            yield parts[0], parts[1], parts[2]


def iter_nt(path, full_names):
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = NT_RE.match(line)
            if not m or m.group(3) is None:
                continue  # skip literals / malformed
            s, p, o = m.group(1), m.group(2), m.group(4)
            if not full_names:
                s, p, o = local_name(s), local_name(p), local_name(o)
            yield s, p, o


def read_gz_csv(path):
    with gzip.open(path, "rt", encoding="utf-8", errors="replace") as f:
        yield from csv.reader(f)


def iter_ogb_biokg(root):
    """OGB biokg layout: raw/relations/<src_type>___<rel>___<dst_type>/edge.csv.gz
    with per-type entity indices; mapping/<type>_entidx2name.csv.gz gives names.
    Entities are namespaced as '<type>:<name>'."""
    mapping_dir = os.path.join(root, "mapping")
    names = {}
    for fn in os.listdir(mapping_dir):
        m = re.match(r"(.+)_entidx2name\.csv\.gz$", fn)
        if not m:
            continue
        ent_type = m.group(1)
        table = []
        for i, row in enumerate(read_gz_csv(os.path.join(mapping_dir, fn))):
            if i == 0:
                continue  # header
            table.append(row[1] if len(row) > 1 else row[0])
        names[ent_type] = table
    rel_dir = os.path.join(root, "raw", "relations")
    for d in sorted(os.listdir(rel_dir)):
        m = re.match(r"(.+)___(.+)___(.+)$", d)
        if not m:
            continue
        src_type, rel, dst_type = m.groups()
        src_names = names.get(src_type, [])
        dst_names = names.get(dst_type, [])
        for row in read_gz_csv(os.path.join(rel_dir, d, "edge.csv.gz")):
            if len(row) < 2:
                continue
            si, di = int(row[0]), int(row[1])
            s = src_names[si] if si < len(src_names) else str(si)
            o = dst_names[di] if di < len(dst_names) else str(di)
            yield f"{src_type}:{s}", rel, f"{dst_type}:{o}"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inputs", nargs="+", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--format", choices=["tsv", "nt", "ogb-biokg"], required=True)
    ap.add_argument("--skip-header", action="store_true")
    ap.add_argument("--full-names", action="store_true",
                    help="nt format: keep full URIs instead of local names")
    args = ap.parse_args()

    triples = []
    for path in args.inputs:
        if args.format == "tsv":
            it = iter_tsv(path, args.skip_header)
        elif args.format == "nt":
            it = iter_nt(path, args.full_names)
        else:
            it = iter_ogb_biokg(path)
        triples.extend(it)
        print(f"  {path}: cumulative {len(triples)} triples")

    entity_id = {}
    relation_id = {}
    edges = set()
    for s, p, o in triples:
        si = entity_id.setdefault(s, len(entity_id))
        pi = relation_id.setdefault(p, len(relation_id))
        oi = entity_id.setdefault(o, len(entity_id))
        edges.add((si, pi, oi))

    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "entities.tsv"), "w") as f:
        for name, i in sorted(entity_id.items(), key=lambda kv: kv[1]):
            f.write(f"{i}\t{name}\n")
    with open(os.path.join(args.out, "relations.tsv"), "w") as f:
        for name, i in sorted(relation_id.items(), key=lambda kv: kv[1]):
            f.write(f"{i}\t{name}\n")
    with open(os.path.join(args.out, "edges.bin"), "wb") as f:
        for s, r, d in sorted(edges):
            f.write(struct.pack("<3I", s, r, d))
    with open(os.path.join(args.out, "meta.yaml"), "w") as f:
        f.write(f"num_entities: {len(entity_id)}\n")
        f.write(f"num_relations: {len(relation_id)}\n")
        f.write(f"num_edges: {len(edges)}\n")

    print(f"done: |V|={len(entity_id)} |R|={len(relation_id)} |E|={len(edges)} -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
