#!/usr/bin/env python3
import argparse
import csv
import hashlib
import json
from pathlib import Path
import shutil

import run_goal24 as base

ROOT = base.ROOT
WORK = ROOT / ".work_hdcv/fixpoint_rules640"
RULES512 = ROOT / ".work_hdcv/full_policy_matrix/rules512"
ALL_RULES = Path("/ssd_data/zhuxk/graphs/KG/fb15k-237/rules")
RULES640 = WORK / "rules640"
GOALS36 = ROOT / ".work_hdcv/fixpoint_goal36/goals.tsv"
SALT = "20261106-rules640"


def select_rules():
    existing = {path.name for path in RULES512.iterdir() if path.is_dir()}
    available = {path.name for path in ALL_RULES.iterdir() if path.is_dir()}
    assert len(existing) == 512 and len(available) == 1652
    assert existing <= available
    new = sorted(available - existing,
                 key=lambda rule_id: hashlib.sha256(
                     f"{SALT}|{rule_id}".encode()).hexdigest())[:128]
    assert len(new) == 128 and not (set(new) & existing)
    selected = sorted(existing | set(new))
    assert len(selected) == 640
    WORK.mkdir(parents=True, exist_ok=True)
    RULES640.mkdir(exist_ok=True)
    for rule_id in selected:
        link = RULES640 / rule_id
        if not link.exists():
            link.symlink_to(ALL_RULES / rule_id)
        assert link.is_dir()
    shutil.copyfile(GOALS36, WORK / "goals.tsv")
    ids_text = "\n".join(selected) + "\n"
    (WORK / "rule_ids.txt").write_text(ids_text)
    metadata = {
        "selection_salt": SALT,
        "base_rules": 512, "added_rules": 128, "total_rules": 640,
        "added_rule_ids": new,
        "rule_ids_sha256": hashlib.sha256(ids_text.encode()).hexdigest(),
        "goals_sha256": base.sha256(WORK / "goals.tsv"),
        "binary_sha256": base.sha256(ROOT / "build/bin/gar_reason"),
    }
    (WORK / "selection.json").write_text(json.dumps(metadata, indent=2) + "\n")
    base.RULES = RULES640
    base.WORK = WORK
    base.OUT = WORK / "runs"
    print(f"frozen {len(selected)} rules and 36 goals at {WORK}", flush=True)


def chase():
    for rep in range(1, 4):
        base.run(f"chase/rep{rep}", [
            "./build/bin/gar_reason", "--mode", "full", "--engine", "cpu",
            "--csr", base.CSR, "--rules", RULES640,
            "--out", base.OUT / f"chase/rep{rep}", "--threads", "1",
            "--print", "0", "--max-rounds", "1000", "--max-derivations",
            "1000000000", "--max-matches", "1000000000", "--no-matches"])


def fixpoint():
    with (WORK / "goals.tsv").open() as stream:
        goals = list(csv.DictReader(stream, delimiter="\t"))
    assert len(goals) == 36
    for goal in goals:
        for rep in range(1, 4):
            name = f"fixpoint/{goal['goal_id']}/det/rep{rep}"
            base.run(name, base.agent_command(goal, base.OUT / name) + [
                "--policy", "deterministic", "--policy-seed", "42"])
        for seed in (7, 13, 42):
            name = f"fixpoint/{goal['goal_id']}/random/seed{seed}"
            base.run(name, base.agent_command(goal, base.OUT / name) + [
                "--policy", "random", "--policy-seed", str(seed)])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--phase", choices=("rules", "chase", "fixpoint", "all"),
                        default="all")
    args = parser.parse_args()
    select_rules()
    if args.phase == "rules":
        return
    if args.phase in ("chase", "all"):
        chase()
    if args.phase in ("fixpoint", "all"):
        fixpoint()


if __name__ == "__main__":
    main()
