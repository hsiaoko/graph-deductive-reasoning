#!/usr/bin/env python3
"""Turn a complete GAR closure into filtered link-prediction rankings."""

import argparse
import csv
import json
from pathlib import Path
import time

import yaml


def read_tsv(path):
    with path.open() as stream:
        for row in csv.reader(stream, delimiter="\t"):
            if row:
                yield row


def metrics(ranks):
    return {
        "mr": sum(ranks) / len(ranks),
        "mrr": sum(1.0 / rank for rank in ranks) / len(ranks),
        "hits@1": sum(rank <= 1 for rank in ranks) / len(ranks),
        "hits@3": sum(rank <= 3 for rank in ranks) / len(ranks),
        "hits@10": sum(rank <= 10 for rank in ranks) / len(ranks),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--data", required=True, type=Path,
                        help="Directory containing standard train/valid/test.txt")
    parser.add_argument("--rules", required=True, type=Path)
    parser.add_argument("--closure", required=True, type=Path,
                        help="closure_edges.tsv from a complete chase")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--score", choices=("first-proof-confidence", "uniform",
                                            "confidence-per-round"),
                        default="first-proof-confidence")
    parser.add_argument("--limit-eval", type=int)
    args = parser.parse_args()

    chase_summary_path = args.closure.parent / "summary.yaml"
    chase_summary = yaml.safe_load(chase_summary_path.read_text())
    if chase_summary.get("fixpoint") is not True:
        raise RuntimeError("full ranking requires a complete, uncapped chase closure")

    confidence = {}
    for rule_yaml in args.rules.glob("*/rule.yaml"):
        rule = yaml.safe_load(rule_yaml.read_text())
        confidence[rule_yaml.parent.name] = float(rule["rule"]["quality"]["confidence"])

    entities = set()
    relations = set()
    splits = {}
    known_forward = {}
    known_backward = {}
    for split in ("train", "valid", "test"):
        triples = []
        for h, r, t in read_tsv(args.data / f"{split}.txt"):
            entities.update((h, t))
            relations.add(r)
            triples.append((h, r, t))
            known_forward.setdefault((h, r), set()).add(t)
            known_backward.setdefault((t, r), set()).add(h)
        splits[split] = triples
    num_nodes = len(entities)

    forward = {}
    backward = {}
    num_rows = 0
    for row in read_tsv(args.closure):
        if row[0].startswith("#"):
            continue
        h, r, t, round_text, rule_id = row[:5]
        if r not in relations:
            continue
        conf = confidence[rule_id]
        round_no = int(round_text)
        if args.score == "first-proof-confidence":
            score = conf
        elif args.score == "confidence-per-round":
            score = conf / round_no
        else:
            score = 1.0
        current = forward.setdefault((h, r), {})
        if t not in current or score > current[t]:
            current[t] = score
            reverse = backward.setdefault((t, r), {})
            reverse[h] = score
        num_rows += 1

    def rank_one(h, r, t, candidates, known):
        scores = candidates.get((h, r), {})
        target_score = scores.get(t, 0.0)
        filtered = known.get((h, r), set()).copy()
        filtered.discard(t)
        eligible = {v: score for v, score in scores.items() if v not in filtered}
        higher = sum(v != t and score >= target_score
                     for v, score in eligible.items())
        zero_ties = 0
        if target_score <= 0.0:
            target_is_derived = t in eligible
            zero_ties = (num_nodes - len(filtered) - len(eligible) -
                         (0 if target_is_derived else 1))
        rank = 1 + higher + zero_ties
        return rank, target_score, len(eligible)

    args.out.mkdir(parents=True, exist_ok=True)
    test = splits["test"]
    if args.limit_eval is not None:
        test = test[:args.limit_eval]
    ranks = []
    covered = 0
    started = time.monotonic()
    with (args.out / "ranking.tsv").open("w") as stream:
        writer = csv.writer(stream, delimiter="\t", lineterminator="\n")
        writer.writerow(["direction", "head", "relation", "tail",
                         "target_score", "rank", "eligible_derived_candidates"])
        for index, (h, r, t) in enumerate(test):
            for direction, candidates, known in (
                    ("tail", forward, known_forward),
                    ("head", backward, known_backward)):
                query_h, target = (h, t) if direction == "tail" else (t, h)
                rank, score, count = rank_one(query_h, r, target, candidates, known)
                ranks.append(rank)
                covered += score > 0.0
                writer.writerow([direction, h, r, t, f"{score:.17g}", rank, count])
    result = {"score": args.score, "queries": len(ranks),
              "closure_rows": num_rows, "nodes": num_nodes,
              "target_coverage": covered / len(ranks),
              "evaluation_wall_sec": time.monotonic() - started,
              **metrics(ranks)}
    (args.out / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
