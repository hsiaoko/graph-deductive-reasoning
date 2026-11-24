#!/usr/bin/env python3
"""Evaluate LearnedStar (learned goal/input star order) vs fixed orders + oracle.

Runs --star-order learned on the held-out test goals and compares total derived
(UNIFIED640 sum-of-medians) against goal order (cosformal Det), input order
(star-order ablation), and the best-of-two oracle upper bound.
"""
import argparse, csv, json, statistics, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CSR = "/ssd_data/zhuxk/graphs/KG/ogbl-biokg/immutable_csr"
RULES = ROOT / ".work_hdcv/overall-performance/ogbl-biokg/rules"
TEST = ROOT / ".work_hdcv/overall-performance/ogbl-biokg-50/goals.tsv"
GOAL = ROOT / ".work_hdcv/overall-performance/ogbl-biokg-cosformal"
INPUT = ROOT / ".work_hdcv/overall-performance/ogbl-biokg-star-order"


def ys(p):
    d = {}
    for l in p.read_text().splitlines():
        if ":" in l:
            k, v = l.split(":", 1); d[k.strip()] = v.strip()
    return d


def med(base, g, tag, seeds):
    v = []
    for s in seeds:
        p = base / g["goal_id"] / tag / f"s{s}" / "summary.yaml"
        if p.exists():
            v.append(int(ys(p).get("derived", 0)))
    return statistics.median(v) if len(v) == len(seeds) else None


def run_learned(spec, out, model, seed, wall):
    out.mkdir(parents=True, exist_ok=True)
    sp = out / "summary.yaml"
    if sp.exists():
        return ys(sp)
    cmd = ["./build/bin/gar_reason", "--mode", "agent", "--engine", "cpu",
           "--csr", CSR, "--rules", str(RULES), "--goal", spec,
           "--out", str(out), "--print", "0", "--star-order", "learned",
           "--star-order-model", str(model), "--scorer", "exact",
           "--budget", "1000000000", "--wall-limit", str(wall),
           "--max-candidates-per-rule", "10000000", "--hdc-dim", "128",
           "--policy", "deterministic", "--policy-seed", str(seed),
           "--fair-interval", "0"]
    subprocess.run(cmd, cwd=ROOT, capture_output=True)
    return ys(sp) if sp.exists() else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--seeds", default="7,13,42")
    ap.add_argument("--wall-limit", type=int, default=900)
    args = ap.parse_args()
    work = Path(args.work)
    goals = list(csv.DictReader(TEST.open(), delimiter="\t"))
    seeds = [int(s) for s in args.seeds.split(",")]
    S = {}
    for g in goals:
        gm = med(GOAL, g, "Det", seeds)
        im = med(INPUT, g, "input", seeds)
        lv = []
        for s in seeds:
            r = run_learned(g["spec"], work / g["goal_id"] / f"s{s}",
                            args.model, s, args.wall_limit)
            if r:
                lv.append(int(r.get("derived", 0)))
        lm = statistics.median(lv) if len(lv) == len(seeds) else None
        if gm is None or im is None or lm is None:
            continue
        st = g["stratum"]
        s = S.setdefault(st, {"goal": 0, "input": 0, "learned": 0, "oracle": 0, "n": 0})
        s["goal"] += gm; s["input"] += im; s["learned"] += lm
        s["oracle"] += min(gm, im); s["n"] += 1
    print(f"{'stratum':8s} {'n':>3s} {'goal':>11s} {'input':>11s} {'learned':>11s} "
          f"{'oracle':>11s} {'lrn/goal':>9s}")
    T = {"goal": 0, "input": 0, "learned": 0, "oracle": 0, "n": 0}
    for st in ("round1", "round2", "deep"):
        if st not in S:
            continue
        s = S[st]
        for k in T:
            T[k] += s[k]
        print(f"{st:8s} {s['n']:3d} {s['goal']:11d} {s['input']:11d} "
              f"{s['learned']:11d} {s['oracle']:11d} {s['learned']/s['goal']:8.3f}x")
    print(f"{'TOTAL':8s} {T['n']:3d} {T['goal']:11d} {T['input']:11d} "
          f"{T['learned']:11d} {T['oracle']:11d} {T['learned']/T['goal']:8.3f}x")
    print(f"\nLearned vs goal: {100*(T['learned']/T['goal']-1):+.1f}%  "
          f"vs input: {100*(T['learned']/T['input']-1):+.1f}%  "
          f"oracle vs goal: {100*(T['oracle']/T['goal']-1):+.1f}%")
    (work / "summary_learned_star.json").write_text(json.dumps({"total": T, "strata": S}, indent=2) + "\n")


if __name__ == "__main__":
    main()