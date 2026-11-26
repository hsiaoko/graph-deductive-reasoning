#!/usr/bin/env python3
"""Combined six-method table. Random is seed-sensitive (3-seed median); the
other methods are seed-invariant (1 seed = the median). Read-only over runs/."""
import csv, statistics
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
WORK = ROOT / ".work_hdcv/overall-performance/finreflectkg-50"
# method -> number of seeds to median over
METHODS = {"DepOnly": 1, "Deterministic": 1, "Random": 3, "Learned": 1,
           "AdaDeduce": 1}
SEEDS = {1: [7], 3: [7, 13, 42]}


def ys(p):
    d = {}
    for l in p.read_text().splitlines():
        if l and not l[0].isspace() and ":" in l:
            k, v = l.split(":", 1); d[k.strip()] = v.strip()
    return d


def read_run(d):
    sp, wp = d / "summary.yaml", d / "wall.txt"
    if not sp.exists():
        return None
    s = ys(sp)
    derived = int(s.get("derived_total", s.get("derived", "0")))
    hit = s.get("goal_derived", s.get("goal_hit", "false")) == "true"
    if "execution_cost_ms" in s:
        w = float(s["execution_cost_ms"])
    else:
        w = float(wp.read_text()) if wp.exists() else 0.0
    return derived, w / 1000, hit


def main():
    goals = list(csv.DictReader((WORK / "test_goals.tsv").open(), delimiter="\t"))
    acc = {m: {} for m in METHODS}
    done = 0
    for g in goals:
        per = {}
        ok = True
        for m, ns in METHODS.items():
            vals = [read_run(WORK / "runs" / g["goal_id"] / m / f"s{s}")
                    for s in SEEDS[ns]]
            vals = [v for v in vals if v]
            if len(vals) < ns:
                ok = False
                break
            per[m] = (statistics.median([v[0] for v in vals]),
                      statistics.median([v[1] for v in vals]),
                      all(v[2] for v in vals))
        if not ok:
            continue
        done += 1
        for m, (d, w, h) in per.items():
            a = acc[m].setdefault(g["round"], {"d": 0, "w": 0, "hit": 0, "n": 0})
            a["d"] += d; a["w"] += w; a["hit"] += h; a["n"] += 1
    print(f"complete goals: {done}/50  (Random 3-seed median; others 1-seed)\n")
    print(f"{'method':14s} {'derived':>12s} {'wall_s':>9s} {'hit':>7s}")
    for m in METHODS:
        tot = {"d": 0, "w": 0, "hit": 0, "n": 0}
        for rnd in sorted(acc[m]):
            for k in tot:
                tot[k] += acc[m][rnd][k]
        print(f"{m:14s} {tot['d']:12d} {tot['w']:9.2f} {tot['hit']}/{tot['n']}")


if __name__ == "__main__":
    main()