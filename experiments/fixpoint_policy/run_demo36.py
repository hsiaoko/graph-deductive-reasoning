#!/usr/bin/env python3
"""Fresh 36-goal demo: learned strategy selector (C) vs forward Deterministic.

Runs both on the 36 test goals (single seed) and prints a per-goal table of
derived facts / execution time / hit, plus aggregates and win count.
"""
import csv, statistics, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / ".work_hdcv/demo36"
CSR = "/ssd_data/zhuxk/graphs/KG/fb15k-237/immutable_csr"
RULES = ROOT / ".work_hdcv/fixpoint_rules640/rules640"
TEST_GOALS = ROOT / ".work_hdcv/fixpoint_q640/test_goals.tsv"
STRATEGY = ROOT / ".work_hdcv/strategy/strategy.model"
SEED = 7


def yaml_summary(path):
    v = {}
    for line in path.read_text().splitlines():
        if ":" in line:
            k, val = line.split(":", 1)
            v[k.strip()] = val.strip()
    return v


def run(direction, goal, d):
    d.mkdir(parents=True, exist_ok=True)
    sp = d / "summary.yaml"
    if sp.exists():
        return yaml_summary(sp)
    cmd = ["./build/bin/gar_reason", "--mode", "agent", "--engine", "cpu",
           "--csr", CSR, "--rules", str(RULES), "--goal", goal["spec"],
           "--out", str(d), "--print", "0", "--star-order", "goal",
           "--scorer", "exact", "--budget", "1000000000", "--wall-limit", "900",
           "--max-candidates-per-rule", "100000", "--hdc-dim", "128",
           "--policy", "deterministic", "--policy-seed", str(SEED),
           "--fair-interval", "0", "--search-direction", direction]
    if direction == "learned-strategy":
        cmd += ["--strategy-model", str(STRATEGY)]
    subprocess.run(cmd, cwd=ROOT, capture_output=True)
    return yaml_summary(sp) if sp.exists() else None


def main():
    goals = list(csv.DictReader(TEST_GOALS.open(), delimiter="\t"))
    rows = []
    for g in goals:
        det = run("forward", g, WORK / "forward" / g["goal_id"])
        ls = run("learned-strategy", g, WORK / "learned" / g["goal_id"])
        if not det or not ls:
            continue
        rows.append({
            "goal": g["goal_id"], "stratum": g["stratum"],
            "det_derived": int(det["derived"]),
            "ls_derived": int(ls["derived"]),
            "det_exec": float(det["execution_cost_ms"]) / 1000,
            "ls_exec": float(ls["execution_cost_ms"]) / 1000,
            "det_hit": det["goal_derived"] == "true",
            "ls_hit": ls["goal_derived"] == "true",
        })
    print(f"{'goal':5s} {'stratum':8s} {'Det_derived':>12s} {'LS_derived':>11s} "
          f"{'ratio':>6s} {'Det_s':>7s} {'LS_s':>7s} {'win':>4s}")
    for r in rows:
        ratio = r["ls_derived"] / max(1, r["det_derived"])
        win = "Y" if r["ls_derived"] < r["det_derived"] else ""
        print(f"{r['goal']:5s} {r['stratum']:8s} {r['det_derived']:12d} "
              f"{r['ls_derived']:11d} {ratio:6.3f} {r['det_exec']:7.3f} "
              f"{r['ls_exec']:7.3f} {win:>4s}")
    dd = statistics.mean(r["det_derived"] for r in rows)
    ld = statistics.mean(r["ls_derived"] for r in rows)
    de = statistics.mean(r["det_exec"] for r in rows)
    le = statistics.mean(r["ls_exec"] for r in rows)
    wins = sum(r["ls_derived"] < r["det_derived"] for r in rows)
    hit = sum(r["ls_hit"] for r in rows) / len(rows)
    print("\n=== aggregate (36 goals, seed 7) ===")
    print(f"Det  derived={dd:.1f} execution={de:.3f}s")
    print(f"LS   derived={ld:.1f} execution={le:.3f}s hit={hit:.2f}")
    print(f"LS vs Det: derived {100*(ld/dd-1):+.1f}%  execution {100*(le/de-1):+.1f}%")
    print(f"goals where LS derives fewer facts: {wins}/{len(rows)}")


if __name__ == "__main__":
    main()