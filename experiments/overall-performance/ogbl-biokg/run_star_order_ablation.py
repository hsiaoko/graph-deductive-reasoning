#!/usr/bin/env python3
"""Framework-structure ablation: goal star order vs input order, same Det policy.

Isolates the contribution of the goal-directed execution plan (the symbolic
structure of the AdaDeduce framework) from the policy. Both arms use the
Deterministic policy and the same executor; only --star-order differs. The
goal-order arm reuses the cosformal Det runs (3 seeds); the input-order arm is
run here. UNIFIED640 protocol: per-goal 3-seed median, summed across goals,
reported overall and per stratum.
"""
import argparse, csv, json, statistics, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CSR = "/ssd_data/zhuxk/graphs/KG/ogbl-biokg/immutable_csr"
RULES = ROOT / ".work_hdcv/overall-performance/ogbl-biokg/rules"
TEST_GOALS = ROOT / ".work_hdcv/overall-performance/ogbl-biokg-50/goals.tsv"
GOAL_ORDER = ROOT / ".work_hdcv/overall-performance/ogbl-biokg-cosformal"


def ys(p):
    d = {}
    for l in p.read_text().splitlines():
        if ":" in l:
            k, v = l.split(":", 1); d[k.strip()] = v.strip()
    return d


def run_input(spec, out, seed, wall):
    out.mkdir(parents=True, exist_ok=True)
    sp = out / "summary.yaml"
    if sp.exists():
        return ys(sp)
    cmd = ["./build/bin/gar_reason", "--mode", "agent", "--engine", "cpu",
           "--csr", CSR, "--rules", str(RULES), "--goal", spec,
           "--out", str(out), "--print", "0", "--star-order", "input",
           "--scorer", "exact", "--budget", "1000000000",
           "--wall-limit", str(wall), "--max-candidates-per-rule", "10000000",
           "--hdc-dim", "128", "--policy", "deterministic",
           "--policy-seed", str(seed), "--fair-interval", "0"]
    subprocess.run(cmd, cwd=ROOT, capture_output=True)
    return ys(sp) if sp.exists() else None


def med3(vals):
    return statistics.median(vals) if len(vals) == 3 else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    ap.add_argument("--seeds", default="7,13,42")
    ap.add_argument("--limit", type=int, default=50)
    ap.add_argument("--wall-limit", type=int, default=900)
    args = ap.parse_args()
    work = Path(args.work)
    goals = list(csv.DictReader(TEST_GOALS.open(), delimiter="\t"))[:args.limit]
    seeds = [int(s) for s in args.seeds.split(",")]
    strata = {}
    for g in goals:
        gd, gw, idd, iw = [], [], [], []
        for s in seeds:
            gp = GOAL_ORDER / g["goal_id"] / "Det" / f"s{s}" / "summary.yaml"
            if gp.exists():
                d = ys(gp); gd.append(int(d.get("derived", 0)))
                gw.append(float(d.get("execution_cost_ms", 0)) / 1000)
            r = run_input(g["spec"], work / g["goal_id"] / "input" / f"s{s}",
                          s, args.wall_limit)
            if r:
                idd.append(int(r.get("derived", 0)))
                iw.append(float(r.get("execution_cost_ms", 0)) / 1000)
        gm, im = med3(gd), med3(idd)
        gwm, iwm = med3(gw), med3(iw)
        if gm is None or im is None:
            continue
        st = g["stratum"]
        s = strata.setdefault(st, {"gd": 0, "id": 0, "gw": 0, "iw": 0, "n": 0})
        s["gd"] += gm; s["id"] += im; s["gw"] += gwm; s["iw"] += iwm; s["n"] += 1
        print(f"{g['goal_id']:5s} {st:7s} goal={gm:9d} input={im:9d} "
              f"input/goal={im/gm:5.2f}x", flush=True)
    print(f"\n=== goal star order vs input order (Det, UNIFIED640 sum-of-medians) ===")
    print(f"{'stratum':8s} {'n':>3s} {'goal派生':>11s} {'input派生':>11s} "
          f"{'input/goal':>10s} {'goal_wall':>9s} {'input_wall':>10s}")
    T = {"gd": 0, "id": 0, "gw": 0, "iw": 0, "n": 0}
    summary = {"strata": {}}
    for st in ("round1", "round2", "deep"):
        if st not in strata:
            continue
        s = strata[st]
        for k in T:
            T[k] += s[k]
        summary["strata"][st] = s
        print(f"{st:8s} {s['n']:3d} {s['gd']:11d} {s['id']:11d} "
              f"{s['id']/s['gd']:9.2f}x {s['gw']:9.2f} {s['iw']:10.2f}")
    summary["total"] = T
    print(f"{'TOTAL':8s} {T['n']:3d} {T['gd']:11d} {T['id']:11d} "
          f"{T['id']/T['gd']:9.2f}x {T['gw']:9.2f} {T['iw']:10.2f}")
    (work / "summary_star_order.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()