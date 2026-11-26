#!/usr/bin/env python3
"""Read-only partial aggregator: compute the stratified table from cached runs
at any time (does not launch runs). Mirrors run_finreflectkg_eval aggregation."""
import csv, statistics
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
WORK = ROOT / ".work_hdcv/overall-performance/finreflectkg-50"
METHODS = ["DepOnly", "Deterministic", "Learned", "AdaDeduce"]


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
    derived = s.get("derived_total", s.get("derived", "0"))
    hit = s.get("goal_derived", s.get("goal_hit", "false"))
    if "execution_cost_ms" in s:
        w = float(s["execution_cost_ms"])
    else:
        w = float(wp.read_text()) if wp.exists() else 0.0
    return int(derived), w / 1000, hit == "true"


def main():
    goals = list(csv.DictReader((WORK / "test_goals.tsv").open(), delimiter="\t"))
    acc = {m: {} for m in METHODS}
    done = 0
    for g in goals:
        complete = True
        per_method = {}
        for m in METHODS:
            vals = []
            for s in (7, 13, 42):
                r = read_run(WORK / "runs" / g["goal_id"] / m / f"s{s}")
                if r:
                    vals.append(r)
            # DepOnly seed-invariant: one run suffices.
            need = 1 if m == "DepOnly" else 3
            if len(vals) < need:
                complete = False
                break
            dv = [v[0] for v in vals]; wv = [v[1] for v in vals]; hv = [v[2] for v in vals]
            per_method[m] = (statistics.median(dv), statistics.median(wv), all(hv))
        if not complete:
            continue
        done += 1
        for m, (d, w, h) in per_method.items():
            a = acc[m].setdefault(g["round"], {"d": 0, "w": 0, "hit": 0, "n": 0})
            a["d"] += d; a["w"] += w; a["hit"] += h; a["n"] += 1
    print(f"complete goals: {done}/50\n")
    print(f"{'method':14s} {'round':>6s} {'n':>3s} {'derived':>12s} {'wall_s':>9s} {'hit':>6s}")
    for m in METHODS:
        tot = {"d": 0, "w": 0, "hit": 0, "n": 0}
        for rnd in sorted(acc[m]):
            a = acc[m][rnd]
            for k in tot:
                tot[k] += a[k]
            print(f"{m:14s} {rnd:>6s} {a['n']:3d} {a['d']:12d} {a['w']:9.2f} {a['hit']}/{a['n']}")
        print(f"{m:14s} {'TOTAL':>6s} {tot['n']:3d} {tot['d']:12d} {tot['w']:9.2f} {tot['hit']}/{tot['n']}\n")


if __name__ == "__main__":
    main()