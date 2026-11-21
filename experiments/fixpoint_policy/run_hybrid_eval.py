#!/usr/bin/env python3
"""Evaluate the backward-first hybrid executor (36 goals x 3 seeds).

Hybrid runs proof-directed backward first (bounded call budget) and falls back
to forward Deterministic when backward fails, so it keeps 100% goal completion
while capturing backward's large search reduction on the goals it solves.
Compares execution / derived / hit against the forward Deterministic baseline.
"""
import argparse, csv, json, statistics, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / ".work_hdcv/hybrid640"
CSR = "/ssd_data/zhuxk/graphs/KG/fb15k-237/immutable_csr"
RULES = ROOT / ".work_hdcv/fixpoint_rules640/rules640"
TEST_GOALS = ROOT / ".work_hdcv/fixpoint_q640/test_goals.tsv"
SEEDS = (7, 13, 42)


def yaml_summary(path):
    values = {}
    for line in path.read_text().splitlines():
        if ":" in line:
            k, v = line.split(":", 1)
            values[k.strip()] = v.strip()
    return values


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--direction", default="hybrid",
                    choices=("hybrid", "forward", "backward"))
    ap.add_argument("--tag", default=None)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--backward-max-calls", type=int, default=100000)
    args = ap.parse_args()
    tag = args.tag or args.direction
    out_root = WORK / tag
    goals = list(csv.DictReader(TEST_GOALS.open(), delimiter="\t"))
    if args.limit:
        goals = goals[:args.limit]
    for goal in goals:
        for seed in SEEDS:
            d = out_root / goal["goal_id"] / f"seed{seed}"
            d.mkdir(parents=True, exist_ok=True)
            if (d / "summary.yaml").exists():
                continue
            cmd = ["./build/bin/gar_reason", "--mode", "agent", "--engine", "cpu",
                   "--csr", CSR, "--rules", str(RULES), "--goal", goal["spec"],
                   "--out", str(d), "--print", "0", "--star-order", "goal",
                   "--scorer", "exact", "--budget", "1000000000",
                   "--wall-limit", "900", "--max-candidates-per-rule", "100000",
                   "--hdc-dim", "128", "--policy", "deterministic",
                   "--policy-seed", str(seed), "--fair-interval", "0",
                   "--search-direction", args.direction,
                   "--backward-max-calls", str(args.backward_max_calls)]
            subprocess.run(cmd, cwd=ROOT, capture_output=True)
    per_goal = []
    for goal in goals:
        rows = []
        for seed in SEEDS:
            sp = out_root / goal["goal_id"] / f"seed{seed}" / "summary.yaml"
            if not sp.exists():
                continue
            s = yaml_summary(sp)
            rows.append((float(s["execution_cost_ms"]) / 1000, int(s["derived"]),
                         s["goal_derived"] == "true"))
        if len(rows) == len(SEEDS):
            per_goal.append((statistics.median(r[0] for r in rows),
                             statistics.median(r[1] for r in rows),
                             all(r[2] for r in rows)))
    summary = {
        "tag": tag, "direction": args.direction, "goals": len(per_goal),
        "hit": sum(p[2] for p in per_goal) / max(1, len(per_goal)),
        "execution": statistics.mean(p[0] for p in per_goal) if per_goal else None,
        "derived": statistics.mean(p[1] for p in per_goal) if per_goal else None,
    }
    (out_root / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2), flush=True)


if __name__ == "__main__":
    main()