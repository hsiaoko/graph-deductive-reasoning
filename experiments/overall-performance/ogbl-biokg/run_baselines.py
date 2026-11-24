#!/usr/bin/env python3
"""Formal six-method BioKG baseline run (docs/biokg_baseline_setting.md).

noScoring / Deterministic / Random(softmax) / Learned(relevance) / LearnedStar /
AdaDeduce(hybrid), 50 held-out goals x 3 seeds (UNIFIED640: per-goal median summed).
Resumable: a run with an existing summary.yaml is skipped.
"""
import argparse, csv, json, statistics, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CSR = "/ssd_data/zhuxk/graphs/KG/ogbl-biokg/immutable_csr"
RULES = ROOT / ".work_hdcv/overall-performance/ogbl-biokg/rules"
TEST = ROOT / ".work_hdcv/overall-performance/ogbl-biokg-50/goals.tsv"
REL_MODEL = ROOT / ".work_hdcv/overall-performance/ogbl-biokg-learn/model_cos_rank_h4/relevance.meta"
STAR_MODEL = ROOT / ".work_hdcv/overall-performance/ogbl-biokg-learnedstar/star_order.model"

METHODS = ["noScoring", "Det", "Random", "Learned", "LearnedStar", "AdaDeduce"]


def method_flags(m):
    if m == "noScoring":
        return ["--policy", "deterministic", "--scorer", "null", "--star-order", "goal"]
    if m == "Det":
        return ["--policy", "deterministic", "--scorer", "exact", "--star-order", "goal"]
    if m == "Random":
        return ["--policy", "random", "--scorer", "exact", "--star-order", "goal"]
    if m == "Learned":
        return ["--policy", "learned", "--scorer", "exact", "--star-order", "goal",
                "--relevance-model", str(REL_MODEL),
                "--learned-handcrafted-weight", "0", "--learned-relevance-weight", "1"]
    if m == "LearnedStar":
        return ["--policy", "deterministic", "--scorer", "exact",
                "--star-order", "learned", "--star-order-model", str(STAR_MODEL)]
    if m == "AdaDeduce":
        return ["--search-direction", "hybrid", "--policy", "deterministic",
                "--scorer", "exact", "--star-order", "goal",
                "--backward-max-calls", "100000", "--backward-max-depth", "256"]
    raise ValueError(m)


def ys(p):
    d = {}
    for l in p.read_text().splitlines():
        if ":" in l:
            k, v = l.split(":", 1); d[k.strip()] = v.strip()
    return d


def run(spec, out, m, seed, wall):
    out.mkdir(parents=True, exist_ok=True)
    sp = out / "summary.yaml"
    if sp.exists():
        return ys(sp)
    cmd = ["./build/bin/gar_reason", "--mode", "agent", "--engine", "cpu",
           "--csr", CSR, "--rules", str(RULES), "--goal", spec,
           "--out", str(out), "--print", "0", "--budget", "1000000000",
           "--wall-limit", str(wall), "--max-candidates-per-rule", "10000000",
           "--hdc-dim", "128", "--fair-interval", "0",
           "--policy-seed", str(seed)] + method_flags(m)
    subprocess.run(cmd, cwd=ROOT, capture_output=True)
    return ys(sp) if sp.exists() else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    ap.add_argument("--seeds", default="7,13,42")
    ap.add_argument("--wall-limit", type=int, default=900)
    ap.add_argument("--methods", default=",".join(METHODS))
    args = ap.parse_args()
    work = Path(args.work)
    goals = list(csv.DictReader(TEST.open(), delimiter="\t"))
    seeds = [int(s) for s in args.seeds.split(",")]
    methods = args.methods.split(",")
    # per method per stratum accumulators
    acc = {m: {} for m in methods}
    done = 0
    for g in goals:
        for m in methods:
            dv, wv, hv = [], [], []
            for s in seeds:
                r = run(g["spec"], work / g["goal_id"] / m / f"s{s}", m, s, args.wall_limit)
                if r:
                    dv.append(int(r.get("derived", 0)))
                    wv.append(float(r.get("execution_cost_ms", 0)) / 1000)
                    hv.append(r.get("goal_derived") == "true")
                done += 1
            if len(dv) != len(seeds):
                continue
            st = g["stratum"]
            a = acc[m].setdefault(st, {"d": 0, "w": 0, "hit": 0, "n": 0, "wins": 0})
            md, mw = statistics.median(dv), statistics.median(wv)
            a["d"] += md; a["w"] += mw; a["hit"] += all(hv); a["n"] += 1
        print(f"progress runs~{done}/{len(goals)*len(methods)*len(seeds)}", flush=True)
    # Det per-goal medians for wins
    det_med = {}
    for g in goals:
        v = []
        for s in seeds:
            p = work / g["goal_id"] / "Det" / f"s{s}" / "summary.yaml"
            if p.exists():
                v.append(int(ys(p).get("derived", 0)))
        if len(v) == len(seeds):
            det_med[g["goal_id"]] = statistics.median(v)
    summary = {"methods": {}}
    print(f"\n{'method':12s} {'stratum':8s} {'n':>3s} {'derived':>12s} {'wall_s':>9s} {'hit':>6s}")
    for m in methods:
        tot = {"d": 0, "w": 0, "hit": 0, "n": 0}
        per = {}
        for st in ("round1", "round2", "deep"):
            if st not in acc[m]:
                continue
            a = acc[m][st]
            # wins vs Det
            wins = 0
            for g in goals:
                if g["stratum"] != st or g["goal_id"] not in det_med:
                    continue
                v = []
                for s in seeds:
                    p = work / g["goal_id"] / m / f"s{s}" / "summary.yaml"
                    if p.exists():
                        v.append(int(ys(p).get("derived", 0)))
                if len(v) == len(seeds) and statistics.median(v) < det_med[g["goal_id"]]:
                    wins += 1
            per[st] = {**a, "wins": wins}
            for k in tot:
                tot[k] += a[k]
            print(f"{m:12s} {st:8s} {a['n']:3d} {a['d']:12d} {a['w']:9.2f} "
                  f"{a['hit']}/{a['n']} wins={wins}")
        summary["methods"][m] = {"total": tot, "strata": per}
        print(f"{m:12s} {'TOTAL':8s} {tot['n']:3d} {tot['d']:12d} {tot['w']:9.2f} "
              f"{tot['hit']}/{tot['n']}\n")
    (work / "summary_baselines.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()