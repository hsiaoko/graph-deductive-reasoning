#!/usr/bin/env python3
import argparse
import csv
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / ".work_hdcv/fixpoint_goal24"
OUT = WORK / "runs"
CSR = "/ssd_data/zhuxk/graphs/KG/fb15k-237/immutable_csr"
RULES = ROOT / ".work_hdcv/full_policy_matrix/rules512"
GT = ROOT / ".work_hdcv/full_policy_matrix/gt_closure512"
ANCHORS = ROOT / ".work_hdcv/fixpoint_policy/goals.tsv"
ADDITIONAL = 12
SALT = "20261106"
BOUNDED = ROOT / "experiments/fixpoint_policy/run_bounded.py"
FIELDS = ["goal_id", "split", "stratum", "expected", "round", "relation",
          "src", "dst", "spec"]


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def select_goals():
    assert ADDITIONAL > 0 and ADDITIONAL % 3 == 0
    per_stratum = ADDITIONAL // 3
    with ANCHORS.open() as stream:
        anchors = [row for row in csv.DictReader(stream, delimiter="\t")
                   if row["split"] == "test"]
    existing = {(row["relation"], row["src"], row["dst"]) for row in anchors}
    groups = {"round1": [], "round2": [], "deep": []}
    with (GT / "closure_edges.tsv").open() as stream:
        for row in csv.reader(stream, delimiter="\t"):
            if not row or row[0].startswith("#"):
                continue
            src, relation, dst, round_text = row[:4]
            if (relation, src, dst) in existing:
                continue
            round_no = int(round_text)
            stratum = "round1" if round_no == 1 else (
                "round2" if round_no == 2 else "deep")
            token = f"{SALT}|{relation}|{src}|{dst}".encode()
            key = hashlib.sha256(token).hexdigest()
            groups[stratum].append({
                "split": "test", "stratum": stratum, "expected": "1",
                "round": str(round_no), "relation": relation, "src": src,
                "dst": dst, "spec": f"{relation}({src},{dst})", "hash": key,
            })
    selected = list(anchors)
    used_relations = {row["relation"] for row in anchors}
    next_id = len(anchors)
    for stratum in ("round1", "round2", "deep"):
        candidates = sorted(groups[stratum],
                            key=lambda row: (row["hash"], row["relation"],
                                             row["src"], row["dst"]))
        chosen = []
        for prefer_new_relation in (True, False):
            for row in candidates:
                if row in chosen:
                    continue
                if prefer_new_relation and row["relation"] in used_relations:
                    continue
                chosen.append(row)
                used_relations.add(row["relation"])
                if len(chosen) == per_stratum:
                    break
            if len(chosen) == per_stratum:
                break
        assert len(chosen) == per_stratum, f"not enough goals in {stratum}"
        for row in chosen:
            row["goal_id"] = f"p{next_id:02d}"
            next_id += 1
            selected.append(row)
    WORK.mkdir(parents=True, exist_ok=True)
    with (WORK / "goals.tsv").open("w") as stream:
        writer = csv.DictWriter(stream, fieldnames=FIELDS, delimiter="\t",
                                extrasaction="ignore", lineterminator="\n")
        writer.writeheader()
        writer.writerows(selected)
    metadata = {
        "selection_salt": SALT,
        "anchors": str(ANCHORS),
        "additional_goals": ADDITIONAL,
        "gt_sha256": sha256(GT / "closure_edges.tsv"),
        "goals_sha256": sha256(WORK / "goals.tsv"),
        "counts": {name: sum(row["stratum"] == name for row in selected)
                   for name in ("round1", "round2", "deep")},
        "binary_sha256": sha256(ROOT / "build/bin/gar_reason"),
    }
    (WORK / "selection.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(f"frozen {len(selected)} goals at {WORK / 'goals.tsv'}", flush=True)


def run(name, command):
    directory = OUT / name
    result = directory / "resource.json"
    if result.exists():
        saved = json.loads(result.read_text())
        print(f"skip {name}: {saved['status']}", flush=True)
        return
    directory.mkdir(parents=True, exist_ok=True)
    bounded = ["python3", str(BOUNDED), "--log", str(directory / "run.log"),
               "--result", str(result), "--timeout-sec", "1000",
               "--memory-gb", "256", "--system-memory-gb", "256", "--",
               *map(str, command)]
    print(f"run {name}", flush=True)
    subprocess.run(bounded, cwd=ROOT, check=False)
    saved = json.loads(result.read_text())
    if saved["status"].startswith("oom"):
        raise RuntimeError(f"stop experiment after {name}: {saved['status']}")


def agent_command(goal, output):
    return ["./build/bin/gar_reason", "--mode", "agent", "--csr", CSR,
            "--rules", str(RULES), "--goal", goal["spec"], "--out", output,
            "--print", "0", "--executor", "fixpoint", "--fixpoint-star-order",
            "goal", "--scorer", "exact", "--budget", "1000000000",
            "--wall-limit", "900", "--max-candidates-per-rule", "100000"]


def main():
    global WORK, OUT, ANCHORS, ADDITIONAL, SALT
    parser = argparse.ArgumentParser()
    parser.add_argument("--phase", choices=("goals", "chase", "fixpoint", "all"),
                        default="all")
    parser.add_argument("--arm", choices=("det", "random", "all"), default="all")
    parser.add_argument("--work", type=Path, default=WORK)
    parser.add_argument("--anchors", type=Path, default=ANCHORS)
    parser.add_argument("--additional", type=int, default=ADDITIONAL)
    parser.add_argument("--salt", default=SALT)
    args = parser.parse_args()
    WORK = args.work.resolve()
    OUT = WORK / "runs"
    ANCHORS = args.anchors.resolve()
    ADDITIONAL = args.additional
    SALT = args.salt
    if args.phase in ("goals", "all") or not (WORK / "goals.tsv").exists():
        select_goals()
    if args.phase == "goals":
        return
    with (WORK / "goals.tsv").open() as stream:
        goals = list(csv.DictReader(stream, delimiter="\t"))
    with ANCHORS.open() as stream:
        anchor_count = sum(row["split"] == "test"
                           for row in csv.DictReader(stream, delimiter="\t"))
    assert len(goals) == anchor_count + ADDITIONAL
    if args.phase in ("chase", "all"):
        for rep in range(1, 4):
            run(f"chase/rep{rep}", [
                "./build/bin/gar_reason", "--mode", "full", "--engine", "cpu",
                "--csr", CSR, "--rules", RULES, "--out", OUT / f"chase/rep{rep}",
                "--threads", "1", "--print", "0", "--max-rounds", "1000",
                "--max-derivations", "1000000000", "--max-matches",
                "1000000000", "--no-matches"])
    if args.phase in ("fixpoint", "all"):
        for goal in goals:
            if args.arm in ("det", "all"):
                for rep in range(1, 4):
                    name = f"fixpoint/{goal['goal_id']}/det/rep{rep}"
                    run(name, agent_command(goal, OUT / name) + [
                        "--policy", "deterministic", "--policy-seed", "42"])
            if args.arm in ("random", "all"):
                for seed in (7, 13, 42):
                    name = f"fixpoint/{goal['goal_id']}/random/seed{seed}"
                    run(name, agent_command(goal, OUT / name) + [
                        "--policy", "random", "--policy-seed", str(seed)])


if __name__ == "__main__":
    main()
