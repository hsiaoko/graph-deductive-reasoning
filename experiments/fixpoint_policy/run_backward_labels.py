#!/usr/bin/env python3
"""Generate backward-success labels for the learned strategy selector (path C).

Runs proof-directed backward search on each training goal (bounded call budget)
and records whether it derives the goal. Pairs with gar_goal_features output to
train a goal-level classifier predicting backward success.
"""
import argparse, csv, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CSR = "/ssd_data/zhuxk/graphs/KG/fb15k-237/immutable_csr"
RULES = ROOT / ".work_hdcv/fixpoint_rules640/rules640"


def yaml_summary(path):
    values = {}
    for line in path.read_text().splitlines():
        if ":" in line:
            k, v = line.split(":", 1)
            values[k.strip()] = v.strip()
    return values


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--goals-tsv", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--max-calls", type=int, default=100000)
    ap.add_argument("--wall-limit", type=int, default=30)
    args = ap.parse_args()
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    goals = list(csv.DictReader(open(args.goals_tsv), delimiter="\t"))
    rows = []
    for g in goals:
        d = work / g["goal_id"]
        d.mkdir(parents=True, exist_ok=True)
        sp = d / "summary.yaml"
        if not sp.exists():
            cmd = ["./build/bin/gar_reason", "--mode", "agent", "--engine", "cpu",
                   "--csr", CSR, "--rules", str(RULES), "--goal", g["spec"],
                   "--out", str(d), "--print", "0", "--star-order", "goal",
                   "--scorer", "exact", "--budget", "1000000000",
                   "--wall-limit", str(args.wall_limit),
                   "--max-candidates-per-rule", "100000", "--hdc-dim", "128",
                   "--policy", "deterministic", "--search-direction", "backward",
                   "--backward-max-calls", str(args.max_calls)]
            subprocess.run(cmd, cwd=ROOT, capture_output=True)
        if not sp.exists():
            continue
        s = yaml_summary(sp)
        rows.append({"goal_id": g["goal_id"],
                     "backward_hit": int(s["goal_derived"] == "true"),
                     "backward_derived": int(s["derived"])})
    with open(args.out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]), delimiter="\t",
                           lineterminator="\n")
        w.writeheader()
        w.writerows(rows)
    hits = sum(r["backward_hit"] for r in rows)
    print(f"labeled {len(rows)} goals; backward success {hits}/{len(rows)}",
          flush=True)


if __name__ == "__main__":
    main()