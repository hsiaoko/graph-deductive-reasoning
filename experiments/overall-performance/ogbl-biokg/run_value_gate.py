#!/usr/bin/env python3
"""Life-or-death gate for the MC cost-to-go value route (Design 2).

The value label can only beat Deterministic if exploratory rollouts contain
trajectories cheaper than Det. This runs Det plus random rollouts over several
seeds on BioKG goals and reports min/median random derived vs Det. Gate passes
only if min random cost is stably below ~0.7x Det; otherwise the value route has
no better-than-teacher signal and is refuted before building infrastructure.
"""
import argparse, csv, statistics, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
WORK = ROOT / ".work_hdcv/overall-performance/ogbl-biokg-valuegate"
CSR = "/ssd_data/zhuxk/graphs/KG/ogbl-biokg/immutable_csr"
RULES = ROOT / ".work_hdcv/overall-performance/ogbl-biokg/rules"
GOALS = ROOT / ".work_hdcv/overall-performance/ogbl-biokg-50/goals.tsv"


def ys(p):
    d = {}
    for l in p.read_text().splitlines():
        if ":" in l:
            k, v = l.split(":", 1); d[k.strip()] = v.strip()
    return d


def run(spec, out, policy, seed):
    out.mkdir(parents=True, exist_ok=True)
    sp = out / "summary.yaml"
    if sp.exists():
        return ys(sp)
    cmd = ["./build/bin/gar_reason", "--mode", "agent", "--engine", "cpu",
           "--csr", CSR, "--rules", str(RULES), "--goal", spec,
           "--out", str(out), "--print", "0", "--star-order", "goal",
           "--scorer", "exact", "--budget", "1000000000",
           "--wall-limit", "300", "--max-candidates-per-rule", "10000000",
           "--hdc-dim", "128", "--policy", policy, "--policy-seed", str(seed),
           "--fair-interval", "0"]
    subprocess.run(cmd, cwd=ROOT, capture_output=True)
    return ys(sp) if sp.exists() else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--goals", type=int, default=12)
    ap.add_argument("--seeds", default="7,13,42,101,202,303,404,505")
    args = ap.parse_args()
    WORK.mkdir(parents=True, exist_ok=True)
    goals = list(csv.DictReader(GOALS.open(), delimiter="\t"))[:args.goals]
    seeds = [int(s) for s in args.seeds.split(",")]
    print(f"{'goal':5s} {'stratum':8s} {'Det':>9s} {'rand_min':>9s} "
          f"{'rand_med':>9s} {'min/Det':>8s} {'hit':>4s}", flush=True)
    pass_count = 0
    for g in goals:
        det = run(g["spec"], WORK / g["goal_id"] / "Det", "deterministic", 7)
        if not det:
            continue
        dd = int(det.get("derived", 0))
        rands = []
        for s in seeds:
            r = run(g["spec"], WORK / g["goal_id"] / f"rand{s}", "random", s)
            if r and r.get("goal_derived") == "true":
                rands.append(int(r.get("derived", 0)))
        if not rands:
            print(f"{g['goal_id']:5s} {g['stratum']:8s} {dd:9d} {'nohit':>9s}", flush=True)
            continue
        rmin, rmed = min(rands), statistics.median(rands)
        ratio = rmin / dd if dd else 0
        passed = ratio < 0.7
        pass_count += passed
        print(f"{g['goal_id']:5s} {g['stratum']:8s} {dd:9d} {rmin:9d} "
              f"{rmed:9.0f} {ratio:8.3f} {'PASS' if passed else 'fail':>4s}", flush=True)
    print(f"\nGATE: {pass_count}/{len(goals)} goals have random min <0.7x Det", flush=True)


if __name__ == "__main__":
    main()