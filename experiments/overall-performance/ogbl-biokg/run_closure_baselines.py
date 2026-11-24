#!/usr/bin/env python3
"""Closure reference baselines: Chase (full closure, one run) + DepOnly (per-goal
cone chase + early-stop, 50 goals). chase/dep-only summaries use derived_total and
do NOT record wall, so wall is measured externally with time.time() and cached in
wall_ms.txt beside each summary. DepOnly is seed-invariant (one seed)."""
import csv, json, time, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CSR = "/ssd_data/zhuxk/graphs/KG/ogbl-biokg/immutable_csr"
RULES = ROOT / ".work_hdcv/overall-performance/ogbl-biokg/rules"
TEST = ROOT / ".work_hdcv/overall-performance/ogbl-biokg-50/goals.tsv"
WORK = ROOT / ".work_hdcv/overall-performance/ogbl-biokg-baselines"


def ys(p):
    d = {}
    for l in p.read_text().splitlines():
        if ":" in l:
            k, v = l.split(":", 1); d[k.strip()] = v.strip()
    return d


def base_flags():
    return ["--csr", CSR, "--rules", str(RULES), "--max-rounds", "100",
            "--max-derivations", "1000000000", "--max-matches", "1000000000",
            "--no-matches", "--print", "0"]


def timed_run(cmd, out):
    out.mkdir(parents=True, exist_ok=True)
    sp = out / "summary.yaml"
    wp = out / "wall_ms.txt"
    if not (sp.exists() and wp.exists()):
        t0 = time.time()
        subprocess.run(cmd, cwd=ROOT, capture_output=True)
        wp.write_text(f"{(time.time() - t0) * 1000:.1f}\n")
    if not sp.exists():
        return None, None
    s = ys(sp)
    wall = float(wp.read_text().strip()) / 1000 if wp.exists() else 0.0
    return s, wall


def main():
    goals = list(csv.DictReader(TEST.open(), delimiter="\t"))
    cs, cw = timed_run(["./build/bin/gar_reason", "--mode", "chase", "--engine",
                        "cpu", "--out", str(WORK / "Chase")] + base_flags(),
                       WORK / "Chase")
    if cs:
        print(f"Chase (full closure): derived={cs.get('derived_total')} "
              f"wall={cw:.2f}s rounds={cs.get('rounds')} fixpoint={cs.get('fixpoint')}",
              flush=True)
    strata = {}
    for g in goals:
        s, w = timed_run(["./build/bin/gar_reason", "--mode", "dep-only",
                          "--goal", g["spec"],
                          "--out", str(WORK / g["goal_id"] / "DepOnly")] + base_flags(),
                         WORK / g["goal_id"] / "DepOnly")
        if not s:
            continue
        st = g["stratum"]
        a = strata.setdefault(st, {"d": 0, "w": 0, "hit": 0, "n": 0})
        a["d"] += int(s.get("derived_total", 0)); a["w"] += w
        a["hit"] += s.get("goal_hit") == "true"; a["n"] += 1
    print(f"\n{'stratum':8s} {'n':>3s} {'derived':>12s} {'wall_s':>9s} {'hit':>6s}")
    tot = {"d": 0, "w": 0, "hit": 0, "n": 0}
    for st in ("round1", "round2", "deep"):
        if st not in strata:
            continue
        a = strata[st]
        for k in tot:
            tot[k] += a[k]
        print(f"{st:8s} {a['n']:3d} {a['d']:12d} {a['w']:9.2f} {a['hit']}/{a['n']}")
    print(f"{'TOTAL':8s} {tot['n']:3d} {tot['d']:12d} {tot['w']:9.2f} {tot['hit']}/{tot['n']}")
    summary = {"Chase": {"derived": int(cs.get("derived_total", 0)) if cs else None,
                         "wall": cw},
               "DepOnly": {"total": tot, "strata": strata}}
    (WORK / "summary_closure.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()