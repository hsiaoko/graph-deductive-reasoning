#!/usr/bin/env python3
"""Freeze the standard FB15K-237 split and build its train-only GAR graph."""

import argparse
import csv
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
EXPECTED = {"train": 272115, "valid": 17535, "test": 20466}


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def read(path):
    with path.open() as stream:
        return {tuple(row) for row in csv.reader(stream, delimiter="\t")}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path,
                        default=Path("/ssd_data/zhuxk/graphs/KG/fb15k-237/Release"))
    parser.add_argument("--out", type=Path,
                        default=ROOT / ".work_hdcv/fb15k_link_prediction")
    args = parser.parse_args()
    source = args.source.resolve()
    out = args.out.resolve()
    data = out / "data"
    data.mkdir(parents=True, exist_ok=True)
    manifest = {"dataset": "FB15K-237", "protocol": "standard_transductive_v1",
                "source": str(source), "splits": {}}
    split_facts = {}
    for split, expected in EXPECTED.items():
        src = source / f"{split}.txt"
        dst = data / f"{split}.txt"
        shutil.copyfile(src, dst)
        facts = read(dst)
        if len(facts) != expected:
            raise RuntimeError(f"{split}: {len(facts)} unique triples, expected {expected}")
        split_facts[split] = facts
        manifest["splits"][split] = {
            "path": str(dst), "triples": expected, "sha256": sha256(dst),
        }
    for left, right in (("train", "valid"), ("train", "test"),
                        ("valid", "test")):
        if split_facts[left] & split_facts[right]:
            raise RuntimeError(f"{left}/{right} triple leakage")
    manifest["disjoint_splits"] = True
    manifest["graph_visible_to_training"] = "data/train.txt"
    manifest["validation_use"] = "model/rule/scoring selection only"
    manifest["test_use"] = "final evaluation only"
    manifest["evaluation"] = {
        "queries": "all test triples, head and tail prediction",
        "ranking": "filtered, ties are counted as ahead of the target",
        "metrics": ["mr", "mrr", "hits@1", "hits@3", "hits@10"],
    }

    triple_graph = out / "triple_graph_train"
    csr = out / "immutable_csr_train"
    subprocess.run([
        "python3", str(ROOT / "tools/prepare_triples.py"), "--format", "tsv",
        "--in", str(data / "train.txt"), "--out", str(triple_graph),
    ], cwd=ROOT, check=True)
    subprocess.run([
        str(ROOT / "build/bin/kg_loader"), "convert", "--graph",
        str(triple_graph), "--out", str(csr),
    ], cwd=ROOT, check=True)
    manifest["train_triple_graph"] = str(triple_graph)
    manifest["train_immutable_csr"] = str(csr)
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({"out": str(out), "splits": EXPECTED,
                      "train_csr": str(csr)}, indent=2))


if __name__ == "__main__":
    main()
