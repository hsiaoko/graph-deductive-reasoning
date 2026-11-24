#!/usr/bin/env python3
"""Formal UNIFIED640 eval of the cosine-feature learned model vs Deterministic.

50 goals x 3 seeds, per-goal median across seeds then sum (total time / total
derived). Supports blend weights (handcrafted f + learned relevance). Reports
total derived, mean wall, hit, and win count vs Det.
"""
import argparse, csv, statistics, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CSR = "/ssd_data/zhuxk/graphs/KG/ogbl-biokg/immutable_csr"
RULES = ROOT / ".work_hdcv/overall-performance/ogbl-biokg/rules"


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
    ap.add_argument("--goals-tsv", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--seeds", default="7,13,42")
    ap.add_argument("--limit", type=int, default=50)
    ap.add_argument("--handcrafted", type=float, default=0.0)
    ap.add_argument("--relevance", type=float, default=1.0)
    ap.add_argument("--tag", default="learned")
    args = ap.parse_args()
    work = Path(args.work)
    goals = list(csv.DictReader(open(args.goals_tsv), delimiter="\t"))[:args.limit]
    seeds = [int(s) for s in args.seeds.split(",")]
    det_med, lrn_med, det_wall, lrn_wall, wins, hits = [], [], [], [], 0, 0
    for g in goals:
        dvals, lvals, dwalls, lwalls, lhits = [], [], [], [], []
        for s in seeds:
            det = run(g["spec"], work / g["goal_id"] / "Det" / f"s{s}",
                      "deterministic", s)
            lrn = run(g["spec"], work / g["goal_id"] / args.tag / f"s{s}",
                      "learned", s, args.model, args.handcrafted, args.relevance)
            if det:
                dvals.append(int(det.get("derived", 0)))
                dwalls.append(float(det.get("execution_cost_ms", 0)) / 1000)
            if lrn:
                lvals.append(int(lrn.get("derived", 0)))
                lwalls.append(float(lrn.get("execution_cost_ms", 0)) / 1000)
                lhits.append(lrn.get("goal_derived") == "true")
        if len(dvals) == len(seeds) and len(lvals) == len(seeds):
            dm, lm = statistics.median(dvals), statistics.median(lvals)
            det_med.append(dm); lrn_med.append(lm)
            det_wall.append(statistics.median(dwalls))
            lrn_wall.append(statistics.median(lwalls))
            wins += lm < dm
            hits += all(lhits)
    n = len(det_med)
    summary = {
        "tag": args.tag, "handcrafted": args.handcrafted, "relevance": args.relevance,
        "goals": n, "seeds": seeds,
        "total_derived_det": sum(det_med), "total_derived_learned": sum(lrn_med),
        "mean_derived_det": statistics.mean(det_med) if det_med else 0,
        "mean_derived_learned": statistics.mean(lrn_med) if lrn_med else 0,
        "derived_ratio": statistics.mean(lrn_med) / statistics.mean(det_med) if det_med else 0,
        "mean_exec_det": statistics.mean(det_wall) if det_wall else 0,
        "mean_exec_learned": statistics.mean(lrn_wall) if lrn_wall else 0,
        "wins": wins, "hit": hits / n if n else 0,
    }
    (work / f"summary_{args.tag}.json").write_text(
        __import__("json").dumps(summary, indent=2) + "\n")
    print(__import__("json").dumps(summary, indent=2), flush=True)


if __name__ == "__main__":
    main()