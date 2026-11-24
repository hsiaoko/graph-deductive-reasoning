#!/usr/bin/env python3
"""BioKG learned-relevance scale experiment: more train goals + smaller model.

Diagnosis: BioKG relevance model is severely undertrained (24 goals -> 73
positives / 345 examples vs ~8289 params, MSE flat). This samples many more
derivable goals from the chase closure, generates deterministic traces, retrains
with a smaller hidden layer, and evaluates whether Learned reduces derived facts
vs Deterministic on held-out test goals.
"""
import argparse, csv, hashlib, json, statistics, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
WORK = ROOT / ".work_hdcv/overall-performance/ogbl-biokg-learn"
CSR = "/ssd_data/zhuxk/graphs/KG/ogbl-biokg/immutable_csr"
RULES = ROOT / ".work_hdcv/overall-performance/ogbl-biokg/rules"
CLOSURE = ROOT / ".work_hdcv/overall-performance/ogbl-biokg/ground_truth/chase/closure_edges.tsv"
TEST_GOALS = ROOT / ".work_hdcv/overall-performance/ogbl-biokg-50/goals.tsv"


def hash_key(seed, rel, src, dst):
    return hashlib.sha256(f"{seed}|{rel}|{src}|{dst}".encode()).hexdigest()


def sample_goals(count, min_round, max_round, seed, exclude):
    rows = []
    for parts in csv.reader(CLOSURE.open(), delimiter="\t"):
        if not parts or parts[0].startswith("#"):
            continue
        src, rel, dst, round_no = parts[:4]
        round_no = int(round_no)
        if not (min_round <= round_no <= max_round):
            continue
        if rel.endswith("^-1") or (rel, src, dst) in exclude:
            continue
        rows.append((rel, src, dst, round_no))
    rows.sort(key=lambda r: hash_key(seed, *r[:3]))
    chosen, used_rel = [], set()
    for row in rows:
        if row[0] in used_rel:
            continue
        chosen.append(row); used_rel.add(row[0])
        if len(chosen) == count:
            break
    if len(chosen) < count:
        picked = {(r[0], r[1], r[2]) for r in chosen}
        for row in rows:
            if (row[0], row[1], row[2]) in picked:
                continue
            chosen.append(row); picked.add((row[0], row[1], row[2]))
            if len(chosen) == count:
                break
    return [{"goal_id": f"g{i:04d}", "spec": f"{rel}({src},{dst})",
             "round": rn} for i, (rel, src, dst, rn) in enumerate(chosen)]


def agent_cmd(spec, out, seed=7):
    return ["./build/bin/gar_reason", "--mode", "agent", "--engine", "cpu",
            "--csr", CSR, "--rules", str(RULES), "--goal", spec,
            "--out", str(out), "--print", "0", "--star-order", "goal",
            "--scorer", "exact", "--budget", "1000000000",
            "--wall-limit", "300", "--max-candidates-per-rule", "10000000",
            "--hdc-dim", "128", "--policy-seed", str(seed), "--fair-interval", "0"]


def ys(p):
    d = {}
    for l in p.read_text().splitlines():
        if ":" in l:
            k, v = l.split(":", 1); d[k.strip()] = v.strip()
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--goals", type=int, default=200)
    ap.add_argument("--hidden", type=int, default=4)
    ap.add_argument("--epochs", type=int, default=30)
    ap.add_argument("--min-round", type=int, default=1)
    ap.add_argument("--max-round", type=int, default=4)
    ap.add_argument("--eval-goals", type=int, default=12)
    ap.add_argument("--seed", default="20261122")
    args = ap.parse_args()
    WORK.mkdir(parents=True, exist_ok=True)
    test = list(csv.DictReader(TEST_GOALS.open(), delimiter="\t"))
    exclude = {(r["relation"], r["src"], r["dst"]) for r in test}
    goals = sample_goals(args.goals, args.min_round, args.max_round,
                         args.seed, exclude)
    print(f"sampled {len(goals)} train goals (round {args.min_round}..{args.max_round})", flush=True)
    traces = WORK / "traces"
    for g in goals:
        d = traces / g["goal_id"] / "deterministic"
        d.mkdir(parents=True, exist_ok=True)
        if (d / "policy_trace.json").exists():
            continue
        cmd = agent_cmd(g["spec"], d) + [
            "--policy", "deterministic", "--policy-trace", str(d / "policy_trace.json"),
            "--policy-trace-negatives", "8"]
        subprocess.run(cmd, cwd=ROOT, capture_output=True)
    model = WORK / f"model_h{args.hidden}" / "relevance.meta"
    cmd = ["./build/bin/gar_train_policy", "--csr", CSR, "--rules", str(RULES),
           "--traces", str(traces), "--test-goals", str(TEST_GOALS),
           "--out", str(model), "--label", "proof", "--epochs", str(args.epochs),
           "--batch-size", "64", "--hidden", str(args.hidden),
           "--label-gamma", "0.5", "--negative-ratio", "4", "--seed", "42"]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    print(r.stdout[-800:], flush=True)
    rep = model.with_suffix(".report.json")
    if rep.exists():
        rr = json.loads(rep.read_text())
        print(f"TRAIN: pos={rr['positive_examples']} examples={rr['train_examples']} "
              f"mse {rr['epoch_mse'][0]:.4f}->{rr['epoch_mse'][-1]:.4f}", flush=True)
    # Eval Learned vs Deterministic on a held-out test subset.
    print(f"\n=== eval {args.eval_goals} test goals: Learned vs Det ===", flush=True)
    wins = 0; ld = []; dd = []
    for g in test[:args.eval_goals]:
        res = {}
        for method, policy in (("Det", "deterministic"), ("Learned", "learned")):
            d = WORK / "eval" / g["goal_id"] / method
            d.mkdir(parents=True, exist_ok=True)
            cmd = agent_cmd(g["spec"], d) + ["--policy", policy]
            if policy == "learned":
                cmd += ["--relevance-model", str(model)]
            subprocess.run(cmd, cwd=ROOT, capture_output=True)
            s = ys(d / "summary.yaml")
            res[method] = int(s.get("derived", 0))
        ld.append(res["Learned"]); dd.append(res["Det"])
        w = res["Learned"] < res["Det"]; wins += w
        print(f"  {g['goal_id']} {g['stratum']:7s} Det={res['Det']:8d} "
              f"Learned={res['Learned']:8d} {'WIN' if w else ''}", flush=True)
    print(f"\nLearned mean={statistics.mean(ld):.0f} Det mean={statistics.mean(dd):.0f} "
          f"ratio={statistics.mean(ld)/statistics.mean(dd):.3f} wins={wins}/{len(ld)}", flush=True)


if __name__ == "__main__":
    main()