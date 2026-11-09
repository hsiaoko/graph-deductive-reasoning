#!/usr/bin/env python3
import argparse
import csv
import hashlib
from pathlib import Path


PILOTS = {
    ("/award/award_winner/awards_won./award/award_honor/award_winner",
     "/m/01sl1q", "/m/03_wj_"),
    ("/award/award_winning_work/awards_won./award/award_honor/award_winner",
     "/m/015g28", "/m/03mdt"),
}


def key(seed, row):
    rel, src, dst = row[:3]
    return hashlib.sha256(f"{seed}|{rel}|{src}|{dst}".encode()).hexdigest()


def read_derived(path):
    rows = []
    with path.open() as stream:
        for parts in csv.reader(stream, delimiter="\t"):
            if not parts or parts[0].startswith("#"):
                continue
            src, rel, dst, round_no = parts[:4]
            if rel.endswith("^-1") or (rel, src, dst) in PILOTS:
                continue
            rows.append((rel, src, dst, int(round_no)))
    return rows


def choose_stratum(rows, count, seed):
    chosen = []
    used_relations = set()
    for row in sorted(rows, key=lambda row: key(seed, row)):
        if row[0] in used_relations:
            continue
        chosen.append(row)
        used_relations.add(row[0])
        if len(chosen) == count:
            return chosen
    for row in sorted(rows, key=lambda row: key(seed, row)):
        if row not in chosen:
            chosen.append(row)
            if len(chosen) == count:
                break
    if len(chosen) != count:
        raise RuntimeError(f"stratum has only {len(chosen)} selectable goals")
    return chosen


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--closure", required=True, type=Path)
    parser.add_argument("--train", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--seed", default="20261105")
    args = parser.parse_args()
    rows = read_derived(args.closure)
    positives = []
    for name, predicate in (("round1", lambda r: r == 1),
                            ("round2", lambda r: r == 2),
                            ("deep", lambda r: r >= 3)):
        for row in choose_stratum([row for row in rows if predicate(row[3])],
                                  4, args.seed):
            positives.append((name, *row))

    all_facts = {(row[0], row[1], row[2]) for row in rows}
    by_relation = {}
    for row in rows:
        by_relation.setdefault(row[0], []).append(row)
    with args.train.open() as stream:
        for parts in csv.reader(stream, delimiter="\t"):
            if len(parts) >= 3:
                all_facts.add((parts[1], parts[0], parts[2]))

    negatives = []
    for stratum, rel, src, dst, round_no in positives[:4]:
        for other in sorted(by_relation[rel], key=lambda row: key(args.seed, row)):
            candidate = (rel, src, other[2])
            if candidate != (rel, src, dst) and candidate not in all_facts:
                negatives.append((stratum, *candidate, -1))
                break
        else:
            raise RuntimeError(f"cannot construct negative for {rel}")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", newline="") as stream:
        writer = csv.writer(stream, delimiter="\t", lineterminator="\n")
        writer.writerow(["goal_id", "split", "stratum", "expected", "round",
                         "relation", "src", "dst", "spec"])
        for index, row in enumerate(positives):
            stratum, rel, src, dst, round_no = row
            writer.writerow([f"p{index:02d}", "test", stratum, 1, round_no,
                             rel, src, dst, f"{rel}({src},{dst})"])
        for index, row in enumerate(negatives):
            stratum, rel, src, dst, round_no = row
            writer.writerow([f"n{index:02d}", "negative", stratum, 0, round_no,
                             rel, src, dst, f"{rel}({src},{dst})"])
    print(f"wrote {len(positives)} positive + {len(negatives)} negative goals")


if __name__ == "__main__":
    main()
