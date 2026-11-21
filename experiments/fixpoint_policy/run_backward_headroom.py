#!/usr/bin/env python3
"""Quantify search headroom: backward chaining vs forward Deterministic.

Backward chaining is proof-directed and can derive a goal with very few facts,
but is incomplete on central predicates (large dependency cones). This runs
backward on the 36 test goals and tallies success rate and explored-fact count
so we can see how much search reduction is achievable in principle (the
"cost-reduction value" ceiling) and where backward fails.
"""
import argparse, csv, json, statistics, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / ".work_hdcv/backward_headroom"
CSR = "/ssd_data/zhuxk/graphs/KG/fb15k-237/immutable_csr"
RULES = ROOT / ".work_hdcv/fixpoint_rules640/rules640"
TEST_GOALS = ROOT / ".work_hdcv/fixpoint_q640/test_goals.tsv"


def yaml_summary(path):
    values = {}
    for line in path.read_text().splitlines():
        if ":" in line:
            k, v = line.split(":", 1)
            values[k.strip()] = v.strip()
    return values


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wall-limit", type=int, default=60)
    ap.add_argument("--max-calls", type=int, default=2000000)
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args()
    WORK.mkdir(parents=True, exist_ok=True)
    goals = list(csv.DictReader(TEST_GOALS.open(), delimiter="\t"))
    if args.limit:
        goals = goals[:args.limit]
    rows = []
    for goal in goals:
        d = WORK / goal["goal_id"]
        d.mkdir(parents=True, exist_ok=True)
        sp = d / "summary.yaml"
        if not sp.exists():
            cmd = ["./build/bin/gar_reason", "--mode", "agent", "--engine", "cpu",
                   "--csr", CSR, "--rules", str(RULES), "--goal", goal["spec"],
                   "--out", str(d), "--print", "0", "--star-order", "goal",
                   "--scorer", "exact", "--budget", "1000000000",
                   "--wall-limit", str(args.wall_limit),
                   "--max-candidates-per-rule", "100000", "--hdc-dim", "128",
                   "--policy", "deterministic", "--search-direction", "backward",
                   "--backward-max-calls", str(args.max_calls),
                   "--backward-max-depth", "256"]
            subprocess.run(cmd, cwd=ROOT, capture_output=True)
        if not sp.exists():
            continue
        s = yaml_summary(sp)
        rows.append({"goal": goal["goal_id"], "stratum": goal["stratum"],
                     "hit": s["goal_derived"] == "true",
                     "derived": int(s["derived"]),
                     "execution": float(s["execution_cost_ms"]) / 1000,
                     "termination": s["termination"]})
    succeeded = [r for r in rows if r["hit"]]
    summary = {
        "goals": len(rows),
        "backward_hit": len(succeeded) / max(1, len(rows)),
        "backward_derived_mean_all": statistics.mean(r["derived"] for r in rows) if rows else None,
        "backward_derived_mean_success": statistics.mean(r["derived"] for r in succeeded) if succeeded else None,
        "backward_execution_mean_success": statistics.mean(r["execution"] for r in succeeded) if succeeded else None,
        "fail_terminations": dict(__import__("collections").Counter(
            r["termination"] for r in rows if not r["hit"])),
    }
    (WORK / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    with (WORK / "rows.tsv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]), delimiter="\t",
                                lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    print(json.dumps(summary, indent=2), flush=True)


if __name__ == "__main__":
    main()