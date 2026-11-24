#!/usr/bin/env python3
"""Evaluate the MC cost-to-go value model vs Deterministic on held-out goals.

The value model is a drop-in for LearnedPolicy: it predicts cheapness
exp(-cost-to-go/scale), so argmax cheapness == argmin predicted remaining cost.
Reports per-goal median derived/wall across seeds, total derived, wins, hit.
"""
import argparse, csv, json, statistics, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CSR = "/ssd_data/zhuxk/graphs/KG/ogbl-biokg/immutable_csr"
RULES = ROOT / ".work_hdcv/overall-performance/ogbl-biokg/rules"
TEST_GOALS = ROOT / ".work_hdcv/overall-performance/ogbl-biokg-50/goals.tsv"


def ys(p):
    d = {}
    for l in p.read_text().splitlines():
        if ":" in l:
            k, v = l.split(":", 1); d[k.strip()] = v.strip()
    return d


def run(spec, out, policy, seed, model=None, handcrafted=0.0, relevance=1.0):
    out.mkdir(parents=True, exist_ok=True)
    sp = out / "summary.yaml"
    if sp.exists():
        return ys(sp)
    cmd = ["./build/bin/gar_reason", "--mode", "agent", "--engine", "cpu",
           "--csr", CSR, "--rules", str(RULES), "--goal", spec,
           "--out", str(out), "--print", "0", "--star-order", "goal",
           "--scorer", "exact", "--budget", "1000000000",
           "--wall-limit", "900", "--max-candidates-per-rule", "10000000",
           "--hdc-dim", "128", "--policy", policy, "--policy-seed", str(seed),
           "--fair-interval", "0"]
    if model:
        cmd += ["--relevance-model", str(model),
                "--learned-handcrafted-weight", str(handcrafted),
                "--learned-relevance-weight", str(relevance)]
    subprocess.run(cmd, cwd=ROOT, capture_output=True)
    return ys(sp) if sp.exists() else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--seeds", default="7,13,42")
    ap.add_argument("--limit", type=int, default=50)
    ap.add_argument("--arms", default="pure,blend",
                    help="comma list: pure (hw0/rel1), blend (hw0.5/rel0.5)")
    args = ap.parse_args()
    work = Path(args.work)
    goals = list(csv.DictReader(TEST_GOALS.open(), delimiter="\t"))[:args.limit]
    seeds = [int(s) for s in args.seeds.split(",")]
    arm_cfg = {"pure": (0.0, 1.0), "blend": (0.5, 0.5)}
    arms = [a for a in args.arms.split(",") if a in arm_cfg]
    det_med, det_wall = [], []
    arm_med = {a: [] for a in arms}
    arm_wall = {a: [] for a in arms}
    arm_wins = {a: 0 for a in arms}
    arm_hit = {a: 0 for a in arms}
    for g in goals:
        dvals, dwalls = [], []
        av = {a: [] for a in arms}
        aw = {a: [] for a in arms}
        ah = {a: [] for a in arms}
        for s in seeds:
            det = run(g["spec"], work / g["goal_id"] / "Det" / f"s{s}",
                      "deterministic", s)
            if det:
                dvals.append(int(det.get("derived", 0)))
                dwalls.append(float(det.get("execution_cost_ms", 0)) / 1000)
            for a in arms:
                hw, rel = arm_cfg[a]
                r = run(g["spec"], work / g["goal_id"] / a / f"s{s}",
                        "learned", s, args.model, hw, rel)
                if r:
                    av[a].append(int(r.get("derived", 0)))
                    aw[a].append(float(r.get("execution_cost_ms", 0)) / 1000)
                    ah[a].append(r.get("goal_derived") == "true")
        if len(dvals) != len(seeds):
            continue
        dm = statistics.median(dvals)
        det_med.append(dm); det_wall.append(statistics.median(dwalls))
        for a in arms:
            if len(av[a]) == len(seeds):
                am = statistics.median(av[a])
                arm_med[a].append(am); arm_wall[a].append(statistics.median(aw[a]))
                arm_wins[a] += am < dm
                arm_hit[a] += all(ah[a])
    n = len(det_med)
    summary = {"goals": n, "seeds": seeds,
               "total_derived_det": sum(det_med),
               "mean_exec_det": statistics.mean(det_wall) if det_wall else 0}
    for a in arms:
        if arm_med[a]:
            summary[a] = {
                "total_derived": sum(arm_med[a]),
                "derived_ratio": statistics.mean(arm_med[a]) / statistics.mean(det_med),
                "mean_exec": statistics.mean(arm_wall[a]),
                "wins": arm_wins[a], "hit": arm_hit[a] / n,
            }
    (work / "summary_value.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2), flush=True)


if __name__ == "__main__":
    main()