#!/usr/bin/env python3
"""Expressiveness probe: can HDC relevance memorize proof membership at all?

Train-on-test diagnostic (NOT a valid generalization result). Overfit a
high-capacity relevance model to the proof traces of a few probe goals, then
evaluate on those SAME goals. If derived collapses toward the proof-DAG size,
the (action HDC, goal HDC, f, cosine) representation CAN express proof
membership and the bottleneck is generalization/data. If even memorization
fails to reduce derived, the representation itself cannot distinguish proof
instances (deep-goal entities are far from the goal, cosine misses them).
"""
import argparse, csv, json, statistics, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
WORK = ROOT / ".work_hdcv/overall-performance/ogbl-biokg-overfit"
CSR = "/ssd_data/zhuxk/graphs/KG/ogbl-biokg/immutable_csr"
RULES = ROOT / ".work_hdcv/overall-performance/ogbl-biokg/rules"
TEST_GOALS = ROOT / ".work_hdcv/overall-performance/ogbl-biokg-50/goals.tsv"


def ys(p):
    d = {}
    for l in p.read_text().splitlines():
        if ":" in l:
            k, v = l.split(":", 1); d[k.strip()] = v.strip()
    return d


def base_cmd(spec, out, seed=7):
    return ["./build/bin/gar_reason", "--mode", "agent", "--engine", "cpu",
            "--csr", CSR, "--rules", str(RULES), "--goal", spec,
            "--out", str(out), "--print", "0", "--star-order", "goal",
            "--scorer", "exact", "--budget", "1000000000",
            "--wall-limit", "900", "--max-candidates-per-rule", "10000000",
            "--hdc-dim", "128", "--policy-seed", str(seed), "--fair-interval", "0"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--goals", default="p01,p02,p06,p09,p17")
    ap.add_argument("--hidden", type=int, default=32)
    ap.add_argument("--epochs", type=int, default=120)
    ap.add_argument("--negatives", type=int, default=64)
    ap.add_argument("--seeds", default="7,13,42")
    args = ap.parse_args()
    WORK.mkdir(parents=True, exist_ok=True)
    all_goals = {g["goal_id"]: g for g in
                 csv.DictReader(TEST_GOALS.open(), delimiter="\t")}
    probe = [all_goals[g] for g in args.goals.split(",") if g in all_goals]
    seeds = [int(s) for s in args.seeds.split(",")]
    traces = WORK / "traces"
    # 1. Det proof traces for the probe goals (positives = proof DAG).
    for g in probe:
        d = traces / g["goal_id"]
        d.mkdir(parents=True, exist_ok=True)
        if not (d / "policy_trace.json").exists():
            cmd = base_cmd(g["spec"], d) + [
                "--policy", "deterministic",
                "--policy-trace", str(d / "policy_trace.json"),
                "--policy-trace-negatives", str(args.negatives)]
            subprocess.run(cmd, cwd=ROOT, capture_output=True)
        tp = d / "policy_trace.json"
        if tp.exists():
            t = json.loads(tp.read_text())
            print(f"  {g['goal_id']} proof={len(t.get('derivations', []))} "
                  f"candidates={len(t.get('candidates', []))}", flush=True)
    # 2. Overfit: train on the probe goals themselves. Use an empty test-goals
    #    file so the leak guard does not fire (this IS train-on-test by design).
    empty = WORK / "empty_goals.tsv"
    empty.write_text("goal_id\tstratum\tcohort\trelation\tsrc\tdst\tspec\tround\n")
    model = WORK / f"model_overfit_h{args.hidden}" / "relevance.meta"
    model.parent.mkdir(parents=True, exist_ok=True)
    cmd = ["./build/bin/gar_train_policy", "--csr", CSR, "--rules", str(RULES),
           "--traces", str(traces), "--test-goals", str(empty),
           "--out", str(model), "--label", "proof", "--hdc-dim", "128",
           "--epochs", str(args.epochs), "--batch-size", "64",
           "--hidden", str(args.hidden), "--label-gamma", "0.5",
           "--negative-ratio", "4", "--ranking", "--seed", "42"]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    print(r.stdout[-800:], flush=True)
    if r.returncode != 0:
        print("TRAIN ERR:", r.stderr[-800:], flush=True)
        return
    rep = model.with_suffix(".report.json")
    if rep.exists():
        rr = json.loads(rep.read_text())
        pa = rr.get("epoch_pairwise_accuracy", [])
        print(f"OVERFIT pairwise {pa[0]:.3f}->{pa[-1]:.3f} "
              f"mse {rr['epoch_mse'][0]:.4f}->{rr['epoch_mse'][-1]:.4f}", flush=True)
    # 3. Eval on the SAME probe goals: Det vs overfit pure vs blend.
    print(f"\n{'goal':5s} {'proof':>6s} {'Det':>9s} {'pure':>9s} {'blend':>9s} "
          f"{'p/D':>6s} {'b/D':>6s}", flush=True)
    rows = []
    for g in probe:
        tp = traces / g["goal_id"] / "policy_trace.json"
        proof = len(json.loads(tp.read_text()).get("derivations", [])) if tp.exists() else 0
        res = {}
        for tag, policy, hw, rel in (("Det", "deterministic", 0, 0),
                                     ("pure", "learned", 0.0, 1.0),
                                     ("blend", "learned", 0.5, 0.5)):
            vals = []
            for s in seeds:
                d = WORK / "eval" / g["goal_id"] / tag / f"s{s}"
                d.mkdir(parents=True, exist_ok=True)
                if not (d / "summary.yaml").exists():
                    cmd = base_cmd(g["spec"], d, s) + ["--policy", policy]
                    if policy == "learned":
                        cmd += ["--relevance-model", str(model),
                                "--learned-handcrafted-weight", str(hw),
                                "--learned-relevance-weight", str(rel)]
                    subprocess.run(cmd, cwd=ROOT, capture_output=True)
                if (d / "summary.yaml").exists():
                    vals.append(int(ys(d / "summary.yaml").get("derived", 0)))
            res[tag] = statistics.median(vals) if len(vals) == len(seeds) else None
        if res["Det"] and res["pure"] and res["blend"]:
            print(f"{g['goal_id']:5s} {proof:6d} {res['Det']:9d} {res['pure']:9d} "
                  f"{res['blend']:9d} {res['pure']/res['Det']:6.3f} "
                  f"{res['blend']/res['Det']:6.3f}", flush=True)
            rows.append({"goal": g["goal_id"], "proof": proof, **res})
    (WORK / "summary_overfit.json").write_text(json.dumps(rows, indent=2) + "\n")


if __name__ == "__main__":
    main()