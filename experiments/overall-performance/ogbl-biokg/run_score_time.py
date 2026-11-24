#!/usr/bin/env python3
"""Time-focused scoring comparison: random no-score baseline vs full formula vs
three single-component variants (evaluation-cost / evidence / goal-relevance).

The handcrafted-score ablation showed derived is weight-insensitive; this asks
whether computing the score saves WALL time vs not computing it at all. The
no-score baseline is uniform-random action selection (ignores f entirely). The
four scored arms reuse the deterministic ablation runs (Det is seed-invariant);
random is seed-dependent so it runs 3 seeds and takes the per-goal median.
"""
import argparse, csv, json, statistics, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CSR = "/ssd_data/zhuxk/graphs/KG/ogbl-biokg/immutable_csr"
RULES = ROOT / ".work_hdcv/overall-performance/ogbl-biokg/rules"
TEST_GOALS = ROOT / ".work_hdcv/overall-performance/ogbl-biokg-50/goals.tsv"
ABL = ROOT / ".work_hdcv/overall-performance/ogbl-biokg-score-abl50"

# display name -> (abl50 arm dir, alpha, beta, lambda)
SCORED = {
    "full": ("full", 1, 1, 1),
    "cost": ("cost_only", 0, 0, 1),
    "evidence": ("evd_only", 0, 1, 0),
    "goal_rel": ("dg_only", 1, 0, 0),
}


def ys(p):
    d = {}
    for l in p.read_text().splitlines():
        if ":" in l:
            k, v = l.split(":", 1); d[k.strip()] = v.strip()
    return d


def run_random(spec, out, seed, wall):
    out.mkdir(parents=True, exist_ok=True)
    sp = out / "summary.yaml"
    if sp.exists():
        return ys(sp)
    cmd = ["./build/bin/gar_reason", "--mode", "agent", "--engine", "cpu",
           "--csr", CSR, "--rules", str(RULES), "--goal", spec,
           "--out", str(out), "--print", "0", "--star-order", "goal",
           "--scorer", "exact", "--budget", "1000000000",
           "--wall-limit", str(wall), "--max-candidates-per-rule", "10000000",
           "--hdc-dim", "128", "--policy", "uniform-random",
           "--policy-seed", str(seed), "--fair-interval", "0"]
    subprocess.run(cmd, cwd=ROOT, capture_output=True)
    return ys(sp) if sp.exists() else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    ap.add_argument("--seeds", default="7,13,42")
    ap.add_argument("--limit", type=int, default=50)
    ap.add_argument("--per-stratum", type=int, default=0)
    ap.add_argument("--wall-limit", type=int, default=900)
    args = ap.parse_args()
    work = Path(args.work)
    all_goals = list(csv.DictReader(TEST_GOALS.open(), delimiter="\t"))
    if args.per_stratum > 0:
        goals, seen = [], {}
        for g in all_goals:
            st = g["stratum"]
            if seen.get(st, 0) < args.per_stratum:
                goals.append(g); seen[st] = seen.get(st, 0) + 1
    else:
        goals = all_goals[:args.limit]
    seeds = [int(s) for s in args.seeds.split(",")]
    arms = ["random"] + list(SCORED)
    tot = {a: {"d": [], "w": [], "hit": 0} for a in arms}
    for g in goals:
        # random: 3-seed median
        rv, rw, rh = [], [], []
        for s in seeds:
            r = run_random(g["spec"], work / g["goal_id"] / "random" / f"s{s}",
                           s, args.wall_limit)
            if r:
                rv.append(int(r.get("derived", 0)))
                rw.append(float(r.get("execution_cost_ms", 0)) / 1000)
                rh.append(r.get("goal_derived") == "true")
        if len(rv) == len(seeds):
            tot["random"]["d"].append(statistics.median(rv))
            tot["random"]["w"].append(statistics.median(rw))
            tot["random"]["hit"] += all(rh)
        # scored arms: reuse abl50 deterministic s7 (seed-invariant)
        for name, (adir, a, b, lam) in SCORED.items():
            p = ABL / g["goal_id"] / adir / "s7" / "summary.yaml"
            if p.exists():
                s = ys(p)
                tot[name]["d"].append(int(s.get("derived", 0)))
                tot[name]["w"].append(float(s.get("execution_cost_ms", 0)) / 1000)
                tot[name]["hit"] += s.get("goal_derived") == "true"
        rd = tot["random"]["d"][-1] if tot["random"]["d"] else 0
        print(f"{g['goal_id']:5s} {g['stratum']:7s} random={rd:.0f}", flush=True)
    n = len(tot["full"]["d"])
    rd = sum(tot["random"]["d"]); rw = sum(tot["random"]["w"])
    print(f"\ngoals={n} (random 3-seed median; scored arms Det seed-invariant)")
    print(f"{'arm':10s} {'总派生':>11s} {'Δ派生vs随机':>12s} {'总wall_s':>9s} "
          f"{'Δwall vs随机':>12s} {'hit':>6s}")
    summary = {"goals": n, "arms": {}}
    for a in arms:
        d = sum(tot[a]["d"]); w = sum(tot[a]["w"])
        summary["arms"][a] = {"total_derived": d, "total_wall": w, "hit": tot[a]["hit"]}
        print(f"{a:10s} {d:11d} {100*(d/rd-1):+11.2f}% {w:9.2f} "
              f"{100*(w/rw-1):+11.2f}% {tot[a]['hit']}/{n}")
    (work / "summary_time.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()