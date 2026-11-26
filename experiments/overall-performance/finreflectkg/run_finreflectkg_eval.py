#!/usr/bin/env python3
"""FinReflectKG stage 2: five-method evaluation on 50 derived goals.

Chase (one full closure) / DepOnly / Deterministic / Learned / AdaDeduce(hybrid),
3 seeds, UNIFIED640 per-goal median summed. Records derived facts, wall, hit.
"""
import argparse, csv, json, statistics, subprocess, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CSR = "/ssd_data/zhuxk/graphs/KG/finreflectkg/immutable_csr"
RULES = "/ssd_data/zhuxk/graphs/KG/finreflectkg/rules"
WORK = ROOT / ".work_hdcv/overall-performance/finreflectkg-50"


def ys(p):
    # Top-level keys only: indented per-rule "derived:" lines must not overwrite
    # the top-level derived / derived_total.
    d = {}
    for l in p.read_text().splitlines():
        if l and not l[0].isspace() and ":" in l:
            k, v = l.split(":", 1); d[k.strip()] = v.strip()
    return d


def flags(method, model):
    if method == "DepOnly":
        return ["--mode", "dep-only"]
    if method == "Deterministic":
        return ["--mode", "agent", "--search-direction", "forward", "--policy",
                "deterministic", "--star-order", "goal", "--scorer", "exact"]
    if method == "Random":
        return ["--mode", "agent", "--search-direction", "forward", "--policy",
                "random", "--star-order", "goal", "--scorer", "exact"]
    if method == "Learned":
        return ["--mode", "agent", "--search-direction", "forward", "--policy",
                "learned", "--star-order", "goal", "--scorer", "exact",
                "--relevance-model", str(model), "--learned-handcrafted-weight",
                "0", "--learned-relevance-weight", "1"]
    if method == "AdaDeduce":
        return ["--mode", "agent", "--search-direction", "hybrid", "--policy",
                "deterministic", "--star-order", "goal", "--scorer", "exact"]
    raise ValueError(method)


def run_goal(spec, out, method, model, seed, wall):
    out.mkdir(parents=True, exist_ok=True)
    sp, wp = out / "summary.yaml", out / "wall.txt"
    if not sp.exists():
        # Method flags (incl. --mode) must precede --goal: the parser routes
        # --goal by the current mode, so --mode has to be set first.
        cmd = ["./build/bin/gar_reason"] + flags(method, model) + [
            "--engine", "cpu", "--csr", CSR, "--rules", RULES, "--goal", spec,
            "--out", str(out), "--print", "0", "--budget", "1000000000",
            "--wall-limit", str(wall), "--max-candidates-per-rule", "10000000",
            "--hdc-dim", "128", "--policy-seed", str(seed), "--fair-interval", "0"]
        t0 = time.time()
        subprocess.run(cmd, cwd=ROOT, capture_output=True)
        wp.write_text(f"{(time.time() - t0) * 1000:.1f}")
    if not sp.exists():
        return None
    d = ys(sp)
    # Normalize chase/agent summaries: chase uses derived_total/goal_hit.
    derived = d.get("derived_total", d.get("derived", "0"))
    hit = d.get("goal_derived", d.get("goal_hit", "false"))
    # Agent modes expose execution_cost_ms (internal pure-execution time, free of
    # graph-load/contention overhead) -- prefer it. Chase/DepOnly lack it, so use
    # the externally measured wall (matches the BioKG baseline convention).
    if "execution_cost_ms" in d:
        w = float(d["execution_cost_ms"])
    else:
        w = float(wp.read_text()) if wp.exists() else 0.0
    return {"derived": derived, "goal_derived": hit, "execution_cost_ms": w}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", default=str(WORK / "model_h4" / "relevance.meta"))
    ap.add_argument("--seeds", default="7,13,42")
    ap.add_argument("--wall-limit", type=int, default=900)
    ap.add_argument("--methods", default="DepOnly,Deterministic,Learned,AdaDeduce")
    args = ap.parse_args()
    goals = list(csv.DictReader((WORK / "test_goals.tsv").open(), delimiter="\t"))
    seeds = [int(s) for s in args.seeds.split(",")]
    methods = args.methods.split(",")
    acc = {m: {} for m in methods}
    for g in goals:
        for m in methods:
            dv, wv, hv = [], [], []
            first = None
            for s in seeds:
                # DepOnly is deterministic (seed-invariant): run once, reuse.
                if m == "DepOnly" and first is not None:
                    r = first
                else:
                    r = run_goal(g["spec"], WORK / "runs" / g["goal_id"] / m / f"s{s}",
                                 m, args.model, s, args.wall_limit)
                    if m == "DepOnly":
                        first = r
                if r:
                    dv.append(int(r.get("derived", 0)))
                    wv.append(float(r.get("execution_cost_ms", 0)) / 1000)
                    hv.append(r.get("goal_derived") == "true")
            if len(dv) != len(seeds):
                continue
            rnd = g["round"]
            a = acc[m].setdefault(rnd, {"d": 0, "w": 0, "hit": 0, "n": 0})
            a["d"] += statistics.median(dv); a["w"] += statistics.median(wv)
            a["hit"] += all(hv); a["n"] += 1
        print(f"  {g['goal_id']} round{g['round']} done", flush=True)
    # Chase reference (single full closure)
    chase = ys(Path("/tmp/fin_chase/summary.yaml")) if Path("/tmp/fin_chase/summary.yaml").exists() else {}
    summary = {"chase": {"derived": int(chase.get("derived_total", 0)),
                         "note": "single full closure, wall ~100.23s"},
               "methods": {}}
    print(f"\n{'method':14s} {'round':>6s} {'n':>3s} {'derived':>12s} {'wall_s':>9s} {'hit':>6s}")
    for m in methods:
        tot = {"d": 0, "w": 0, "hit": 0, "n": 0}
        per = {}
        for rnd in sorted(acc[m]):
            a = acc[m][rnd]
            per[rnd] = a
            for k in tot:
                tot[k] += a[k]
            print(f"{m:14s} {rnd:>6s} {a['n']:3d} {a['d']:12d} {a['w']:9.2f} {a['hit']}/{a['n']}")
        summary["methods"][m] = {"total": tot, "per_round": per}
        print(f"{m:14s} {'TOTAL':>6s} {tot['n']:3d} {tot['d']:12d} {tot['w']:9.2f} {tot['hit']}/{tot['n']}\n")
    (WORK / "summary_eval.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()