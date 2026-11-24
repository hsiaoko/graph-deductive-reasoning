#!/usr/bin/env python3
"""MC cost-to-go value route (Design 2): rollout data + value model training.

The relevance (proof-proximity) label caps at Deterministic. The value label is
the marginal cost-to-go: along exploratory rollouts, an action taken in a cheap
rollout gets a high cheapness target exp(-remaining/scale), an action in an
expensive rollout a low one. The cross-rollout cost variation is the learnable
signal (the value gate showed 3/12 goals have stable cheaper-than-Det rollouts).

This samples train goals disjoint from the held-out test set, runs K random
rollouts per goal recording the chosen action per decision (decision_trace.tsv),
writes value_meta.tsv (goal_spec, total_derived), and trains a value model with
gar_train_policy --label value. The model is a drop-in for LearnedPolicy (argmax
cheapness == argmin cost-to-go).
"""
import argparse, csv, hashlib, json, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
WORK = ROOT / ".work_hdcv/overall-performance/ogbl-biokg-value"
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


def ys(p):
    d = {}
    for l in p.read_text().splitlines():
        if ":" in l:
            k, v = l.split(":", 1); d[k.strip()] = v.strip()
    return d


def rollout(spec, out, seed, wall):
    out.mkdir(parents=True, exist_ok=True)
    sp = out / "summary.yaml"
    dt = out / "decision_trace.tsv"
    if sp.exists() and dt.exists():
        return ys(sp)
    cmd = ["./build/bin/gar_reason", "--mode", "agent", "--engine", "cpu",
           "--csr", CSR, "--rules", str(RULES), "--goal", spec,
           "--out", str(out), "--print", "0", "--star-order", "goal",
           "--scorer", "exact", "--budget", "1000000000",
           "--wall-limit", str(wall), "--max-candidates-per-rule", "10000000",
           "--hdc-dim", "128", "--policy", "random", "--policy-seed", str(seed),
           "--fair-interval", "0", "--decision-trace", str(dt)]
    subprocess.run(cmd, cwd=ROOT, capture_output=True)
    return ys(sp) if sp.exists() else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--goals", type=int, default=40)
    ap.add_argument("--seeds", default="7,13,42,101,202,303")
    ap.add_argument("--min-round", type=int, default=1)
    ap.add_argument("--max-round", type=int, default=2)
    ap.add_argument("--hidden", type=int, default=8)
    ap.add_argument("--epochs", type=int, default=30)
    ap.add_argument("--value-scale", type=float, default=10000.0)
    ap.add_argument("--value-stride", type=int, default=8)
    ap.add_argument("--value-max-decisions", type=int, default=4000)
    ap.add_argument("--wall-limit", type=int, default=300)
    ap.add_argument("--goal-seed", default="20261123")
    ap.add_argument("--train-only", action="store_true")
    args = ap.parse_args()
    WORK.mkdir(parents=True, exist_ok=True)
    test = list(csv.DictReader(TEST_GOALS.open(), delimiter="\t"))
    exclude = {(r["relation"], r["src"], r["dst"]) for r in test}
    goals = sample_goals(args.goals, args.min_round, args.max_round,
                         args.goal_seed, exclude)
    seeds = [int(s) for s in args.seeds.split(",")]
    print(f"sampled {len(goals)} train goals x {len(seeds)} seeds", flush=True)
    vtraces = WORK / "value_traces"
    vtraces.mkdir(parents=True, exist_ok=True)
    n_ok = 0
    for g in goals:
        for s in seeds:
            run = vtraces / f"{g['goal_id']}__s{s}"
            summ = rollout(g["spec"], run, s, args.wall_limit)
            if not summ or summ.get("goal_derived") != "true":
                continue
            total = int(summ.get("derived", 0))
            (run / "value_meta.tsv").write_text(
                f"goal_spec\t{g['spec']}\ntotal_derived\t{total}\n")
            n_ok += 1
        print(f"  {g['goal_id']} round{g['round']} done", flush=True)
    print(f"rollouts ok: {n_ok}", flush=True)
    model = WORK / f"model_value_h{args.hidden}" / "relevance.meta"
    model.parent.mkdir(parents=True, exist_ok=True)
    cmd = ["./build/bin/gar_train_policy", "--csr", CSR, "--rules", str(RULES),
           "--value-traces", str(vtraces), "--test-goals", str(TEST_GOALS),
           "--out", str(model), "--label", "value", "--hdc-dim", "128",
           "--value-scale", str(args.value_scale),
           "--value-stride", str(args.value_stride),
           "--value-max-decisions", str(args.value_max_decisions),
           "--epochs", str(args.epochs), "--batch-size", "256",
           "--hidden", str(args.hidden), "--seed", "42"]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    print(r.stdout[-1500:], flush=True)
    if r.returncode != 0:
        print("TRAIN STDERR:", r.stderr[-1500:], flush=True)
    rep = model.with_suffix(".report.json")
    if rep.exists():
        rr = json.loads(rep.read_text())
        print(f"TRAIN examples={rr['train_examples']} "
              f"mse {rr['epoch_mse'][0]:.4f}->{rr['epoch_mse'][-1]:.4f} "
              f"val_mse={rr['validation']['mse']:.4f}", flush=True)
    print(f"model: {model}", flush=True)


if __name__ == "__main__":
    main()