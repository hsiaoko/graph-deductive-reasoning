#!/usr/bin/env python3
"""Oracle upper bound on learnable search reduction.

The forward Deterministic policy trace records `derivations` = exactly the
goal's proof DAG (WritePolicyTrace only emits proof facts). So len(derivations)
is the number of facts a *perfect* proof-directed policy would derive (base
facts are free) -- an oracle lower bound on exploration. Comparing it to the
total derived tells us whether there is learnable headroom:

  proof_size << derived  -> huge waste, headroom exists (instance-level signal
                            could in principle reduce it);
  proof_size ~= derived  -> the proof cone itself is huge, no policy can help.

This decides whether investing in a better learned policy (path A) is viable
on the hard fallback goals, or whether the gain is purely structural (hybrid).
"""
import argparse, csv, json, statistics, subprocess
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / ".work_hdcv/oracle_bound"
CSR = "/ssd_data/zhuxk/graphs/KG/fb15k-237/immutable_csr"
RULES = ROOT / ".work_hdcv/fixpoint_rules640/rules640"
TEST_GOALS = ROOT / ".work_hdcv/fixpoint_q640/test_goals.tsv"


def yaml_summary(path):
    values = {}
    for line in path.read_text().splitlines():
        if ":" in line:
            k, v = line.split(":", 1)
            values[k.strip()] = v.strip()
    return values


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--wall-limit", type=int, default=300)
    args = ap.parse_args()
    WORK.mkdir(parents=True, exist_ok=True)
    goals = list(csv.DictReader(TEST_GOALS.open(), delimiter="\t"))
    if args.limit:
        goals = goals[:args.limit]
    rows = []
    for goal in goals:
        d = WORK / goal["goal_id"]
        d.mkdir(parents=True, exist_ok=True)
        sp, tp = d / "summary.yaml", d / "policy_trace.json"
        if not sp.exists() or not tp.exists():
            cmd = ["./build/bin/gar_reason", "--mode", "agent", "--engine", "cpu",
                   "--csr", CSR, "--rules", str(RULES), "--goal", goal["spec"],
                   "--out", str(d), "--print", "0", "--star-order", "goal",
                   "--scorer", "exact", "--budget", "1000000000",
                   "--wall-limit", str(args.wall_limit),
                   "--max-candidates-per-rule", "100000", "--hdc-dim", "128",
                   "--policy", "deterministic", "--policy-seed", "7",
                   "--fair-interval", "0", "--policy-trace", str(tp),
                   "--policy-trace-negatives", "0"]
            subprocess.run(cmd, cwd=ROOT, capture_output=True)
        if not sp.exists() or not tp.exists():
            continue
        s = yaml_summary(sp)
        trace = json.loads(tp.read_text())
        if s["goal_derived"] != "true":
            continue
        derived = int(s["derived"])
        proof = len(trace["derivations"])
        rows.append({"goal": goal["goal_id"], "stratum": goal["stratum"],
                     "derived": derived, "proof": proof,
                     "waste": derived - proof,
                     "waste_frac": (derived - proof) / max(1, derived)})
    with (WORK / "rows.tsv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]), delimiter="\t",
                                lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    # Hard goals = those where backward fails (hybrid ~ forward); approximate by
    # large derived. Report per-stratum and overall oracle bound.
    summary = {
        "goals": len(rows),
        "derived_mean": statistics.mean(r["derived"] for r in rows),
        "proof_mean": statistics.mean(r["proof"] for r in rows),
        "waste_frac_mean": statistics.mean(r["waste_frac"] for r in rows),
        "by_stratum": {},
    }
    for stratum in sorted(set(r["stratum"] for r in rows)):
        sub = [r for r in rows if r["stratum"] == stratum]
        summary["by_stratum"][stratum] = {
            "n": len(sub),
            "derived_mean": statistics.mean(r["derived"] for r in sub),
            "proof_mean": statistics.mean(r["proof"] for r in sub),
            "waste_frac_mean": statistics.mean(r["waste_frac"] for r in sub),
        }
    (WORK / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2), flush=True)
    print("\nper-goal (derived / proof / waste%):", flush=True)
    for r in sorted(rows, key=lambda x: -x["derived"]):
        print(f"  {r['goal']} {r['stratum']:7s} derived={r['derived']:7d} "
              f"proof={r['proof']:5d} waste={100*r['waste_frac']:5.1f}%", flush=True)


if __name__ == "__main__":
    main()