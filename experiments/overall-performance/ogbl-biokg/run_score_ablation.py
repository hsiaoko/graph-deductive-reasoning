#!/usr/bin/env python3
"""Handcrafted-score component ablation (paper §4).

f = alpha*rel_fam + beta*rel_evd - lambda*dcost. rel_fam = gamma^d(p0) from the
dependency graph; rel_evd = witnessed-premise share (exact scorer) or HDC set
estimate (hdc scorer); dcost = residual cost of unwitnessed premises. This sweeps
extreme weightings (alpha:beta:lambda) to isolate which component drives
Deterministic's search reduction. UNIFIED640 protocol: per-goal 3-seed median,
summed across goals. The admissibility guard stays active in every arm.
"""
import argparse, csv, json, statistics, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CSR = "/ssd_data/zhuxk/graphs/KG/ogbl-biokg/immutable_csr"
RULES = ROOT / ".work_hdcv/overall-performance/ogbl-biokg/rules"
TEST_GOALS = ROOT / ".work_hdcv/overall-performance/ogbl-biokg-50/goals.tsv"

# name -> (alpha, beta, lambda)
ARMS = {
    "full": (1, 1, 1),
    "dg_only": (1, 0, 0),
    "evd_only": (0, 1, 0),
    "cost_only": (0, 0, 1),
    "no_cost": (1, 1, 0),
    "no_evd": (1, 0, 1),
    "no_dg": (0, 1, 1),
    "null": (0, 0, 0),
}


def ys(p):
    d = {}
    for l in p.read_text().splitlines():
        if ":" in l:
            k, v = l.split(":", 1); d[k.strip()] = v.strip()
    return d


def run(spec, out, seed, arm, scorer):
    out.mkdir(parents=True, exist_ok=True)
    sp = out / "summary.yaml"
    if sp.exists():
        return ys(sp)
    a, b, lam = ARMS[arm]
    cmd = ["./build/bin/gar_reason", "--mode", "agent", "--engine", "cpu",
           "--csr", CSR, "--rules", str(RULES), "--goal", spec,
           "--out", str(out), "--print", "0", "--star-order", "goal",
           "--scorer", scorer, "--budget", "1000000000",
           "--wall-limit", "900", "--max-candidates-per-rule", "10000000",
           "--hdc-dim", "128", "--policy", "deterministic", "--policy-seed", str(seed),
           "--fair-interval", "0", "--alpha", str(a), "--beta", str(b),
           "--lambda", str(lam), "--gamma", "0.5"]
    subprocess.run(cmd, cwd=ROOT, capture_output=True)
    return ys(sp) if sp.exists() else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    ap.add_argument("--seeds", default="7,13,42")
    ap.add_argument("--limit", type=int, default=12)
    ap.add_argument("--scorer", default="exact")
    ap.add_argument("--arms", default=",".join(ARMS))
    args = ap.parse_args()
    work = Path(args.work)
    goals = list(csv.DictReader(TEST_GOALS.open(), delimiter="\t"))[:args.limit]
    seeds = [int(s) for s in args.seeds.split(",")]
    arms = [a for a in args.arms.split(",") if a in ARMS]
    per_arm = {a: {"d": [], "w": [], "wins": 0, "hit": 0} for a in arms}
    full_med = {}
    for g in goals:
        med = {}
        for a in arms:
            dv, wv, hv = [], [], []
            for s in seeds:
                r = run(g["spec"], work / g["goal_id"] / a / f"s{s}", s, a, args.scorer)
                if r:
                    dv.append(int(r.get("derived", 0)))
                    wv.append(float(r.get("execution_cost_ms", 0)) / 1000)
                    hv.append(r.get("goal_derived") == "true")
            if len(dv) == len(seeds):
                med[a] = (statistics.median(dv), statistics.median(wv), all(hv))
        if "full" in med:
            full_med[g["goal_id"]] = med["full"][0]
        for a in arms:
            if a in med:
                per_arm[a]["d"].append(med[a][0])
                per_arm[a]["w"].append(med[a][1])
                per_arm[a]["hit"] += med[a][2]
                if a != "full" and "full" in med:
                    per_arm[a]["wins"] += med[a][0] < med["full"][0]
        done = " ".join(f"{a}={med[a][0]}" for a in arms if a in med)
        print(f"{g['goal_id']:5s} {g['stratum']:7s} {done}", flush=True)
    n = len(per_arm["full"]["d"]) if "full" in per_arm else 0
    fd = sum(per_arm["full"]["d"]) if "full" in per_arm else 1
    fw = sum(per_arm["full"]["w"]) if "full" in per_arm else 1
    summary = {"scorer": args.scorer, "goals": n, "seeds": seeds, "arms": {}}
    print(f"\n{'arm':10s} {'α:β:λ':8s} {'总派生':>11s} {'Δ派生':>8s} {'总wall_s':>9s} "
          f"{'Δwall':>8s} {'wins/full':>9s} {'hit':>6s}", flush=True)
    for a in arms:
        pa = per_arm[a]
        if not pa["d"]:
            continue
        sd, sw = sum(pa["d"]), sum(pa["w"])
        ab = ":".join(map(str, ARMS[a]))
        summary["arms"][a] = {"weights": ARMS[a], "total_derived": sd,
                              "total_wall": sw, "hit": pa["hit"], "wins": pa["wins"]}
        print(f"{a:10s} {ab:8s} {sd:11d} {100*(sd/fd-1):+7.1f}% {sw:9.2f} "
              f"{100*(sw/fw-1):+7.1f}% {pa['wins']:>5d}/{n:<3d} {pa['hit']}/{n}", flush=True)
    (work / f"summary_{args.scorer}.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()