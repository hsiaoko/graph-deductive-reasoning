#!/usr/bin/env python3
"""Extract numeric vertex attributes from DBpedia mappingbased_literals.

Joins literals against a TripleGraph's entities.tsv (local names), keeps the
top-N properties by coverage, and writes one TSV per attribute plus a
meta.tsv manifest — the input layout of `kg_loader add-vertex-attrs`.

Output <out>:
    meta.tsv            attr_name \t type(Int|Double) \t n_rows
    <attr>.tsv          vid \t value     (sorted by vid)

Numeric xsd types map to Int (integral) or Double; xsd:date / gYear /
gYearMonth literals contribute an Int column "<attr>__year".

Usage:
    python3 tools/prepare_dbpedia_literals.py \
        --ttl mappingbased_literals_en.ttl \
        --graph /ssd_data/zhuxk/graphs/KG/dbpedia/triple_graph \
        --out /ssd_data/zhuxk/graphs/KG/dbpedia/literal_attrs \
        [--top 60] [--min-rows 500]
"""

import argparse
import os
import re
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from prepare_triples import local_name  # noqa: E402

LIT_RE = re.compile(
    r'^<([^>]*)>\s+<([^>]*)>\s+"((?:[^"\\]|\\.)*)"(?:\^\^<([^>]*)>|@[a-zA-Z-]+)?\s*\.\s*$'
)

INT_TYPES = {
    "integer", "int", "long", "short", "byte", "nonNegativeInteger",
    "nonPositiveInteger", "positiveInteger", "negativeInteger",
    "unsignedLong", "unsignedInt", "unsignedShort", "unsignedByte",
}
DOUBLE_TYPES = {"double", "float", "decimal"}
DATE_TYPES = {"date", "gYear", "gYearMonth"}


def xsd_local(uri):
    return local_name(uri) if uri else ""


def parse_value(kind, text):
    """Returns (col_type, value) or None."""
    if kind in INT_TYPES:
        try:
            return "Int", int(text)
        except ValueError:
            return None
    if kind in DOUBLE_TYPES:
        try:
            return "Double", float(text)
        except ValueError:
            return None
    if kind in DATE_TYPES:
        m = re.match(r"^(-?\d+)", text)
        if m:
            return "Int", int(m.group(1))
        return None
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ttl", required=True)
    ap.add_argument("--graph", required=True, help="TripleGraph directory")
    ap.add_argument("--out", required=True)
    ap.add_argument("--top", type=int, default=60)
    ap.add_argument("--min-rows", type=int, default=500)
    args = ap.parse_args()

    # entity local name -> vid
    ent2id = {}
    with open(os.path.join(args.graph, "entities.tsv"), encoding="utf-8") as f:
        for line in f:
            vid, name = line.rstrip("\n").split("\t", 1)
            ent2id[name] = int(vid)
    print(f"[literals] {len(ent2id)} entities in dictionary", flush=True)

    # attr -> (col_type, {vid: value})  (first literal wins per entity)
    cols = {}
    n_lines = n_matched = 0
    with open(args.ttl, encoding="utf-8", errors="replace") as f:
        for line in f:
            n_lines += 1
            if n_lines % 2_000_000 == 0:
                print(f"[literals] {n_lines} lines, {n_matched} matched",
                      flush=True)
            m = LIT_RE.match(line)
            if not m:
                continue
            s, p, text, dtype = m.groups()
            kind = xsd_local(dtype)
            suffix = ""
            if kind in DATE_TYPES:
                suffix = "__year"
            parsed = parse_value(kind, text)
            if parsed is None:
                continue
            vid = ent2id.get(local_name(s))
            if vid is None:
                continue
            attr = local_name(p) + suffix
            col_type, value = parsed
            col = cols.get(attr)
            if col is None:
                col = cols.setdefault(attr, [col_type, {}])
            if col[0] != col_type or vid in col[1]:
                continue
            col[1][vid] = value
            n_matched += 1

    ranked = sorted(cols.items(), key=lambda kv: -len(kv[1][1]))
    ranked = [kv for kv in ranked if len(kv[1][1]) >= args.min_rows]
    ranked = ranked[: args.top]

    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "meta.tsv"), "w", encoding="utf-8") as mf:
        for attr, (col_type, values) in ranked:
            safe = re.sub(r"[^A-Za-z0-9_.-]", "_", attr)
            with open(os.path.join(args.out, safe + ".tsv"), "w",
                      encoding="utf-8") as cf:
                for vid in sorted(values):
                    cf.write(f"{vid}\t{values[vid]}\n")
            mf.write(f"{safe}\t{col_type}\t{len(values)}\n")
            print(f"[literals] {safe}: {col_type}, {len(values)} rows",
                  flush=True)
    print(f"[literals] wrote {len(ranked)} attribute columns to {args.out}",
          flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
