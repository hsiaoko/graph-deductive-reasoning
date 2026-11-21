#!/usr/bin/env python3
"""Scale up relevance training goals to test the data-scarcity root cause.

Samples genuinely *derivable* goals from the chase closure (round>=1 facts),
generates deterministic policy traces, retrains the relevance model, and prints
the training report so we can see whether more proof-DAG positives make the
model actually learn (MSE drops, recall@1 rises). Eval on the 36 held-out test
goals is done separately via run_blend_ablation.py with the new model.

The earlier gengoals.tsv pool turned out to be base facts (every trace ended
'goal_already_in_graph' with zero derivations), so it yielded no positives; the
closure is the correct source of derivable goals.
"""
import argparse, csv, hashlib, json, shutil, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / ".work_hdcv/relevance_scale"
CSR = "/ssd_data/zhuxk/graphs/KG/fb15k-237/immutable_csr"
RULES = ROOT / ".work_hdcv/fixpoint_rules640/rules640"
CLOSURE = ROOT / ".work_hdcv/fixpoint_rules640/runs/chase/rep1/closure_edges.tsv"
TRAIN_GOALS = ROOT / ".work_hdcv/fixpoint_q640/train_goals.tsv"
TEST_GOALS = ROOT / ".work_hdcv/fixpoint_q640/test_goals.tsv"


def hash_key(seed, rel, src, dst):
    return hashlib.sha256(f"{seed}|{rel}|{src}|{dst}".encode()).hexdigest()


def held_out_facts():
    facts = set()
    for path in (TRAIN_GOALS, TEST_GOALS):
        for row in csv.DictReader(path.open(), delimiter="\t"):
            facts.add((row["relation"], row["src"], row["dst"]))
    return facts


def sample_goals(count, min_round, max_round, seed):
    exclude = held_out_facts()
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
    # Prefer one goal per relation first for diversity, then fill remaining.
    chosen, used_rel = [], set()
    for row in rows:
        if row[0] in used_rel:
            continue
        chosen.append(row)
        used_rel.add(row[0])
        if len(chosen) == count:
            break
    if len(chosen) < count:
        picked = {(r[0], r[1], r[2]) for r in chosen}
        for row in rows:
            if (row[0], row[1], row[2]) in picked:
                continue
            chosen.append(row)
            picked.add((row[0], row[1], row[2]))
            if len(chosen) == count:
                break
    goals = []
    for i, (rel, src, dst, round_no) in enumerate(chosen):
        goals.append({"goal_id": f"g{i:04d}", "relation": rel, "src": src,
                      "dst": dst, "round": round_no,
                      "spec": f"{rel}({src},{dst})"})
    return goals


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--goals", type=int, default=150)
    ap.add_argument("--hidden", type=int, default=32)
    ap.add_argument("--epochs", type=int, default=20)
    ap.add_argument("--min-round", type=int, default=2)
    ap.add_argument("--max-round", type=int, default=4)
    ap.add_argument("--seed", default="20261121")
    args = ap.parse_args()
    WORK.mkdir(parents=True, exist_ok=True)
    goals = sample_goals(args.goals, args.min_round, args.max_round, args.seed)
    with (WORK / "scale_goals.tsv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(goals[0]), delimiter="\t",
                                lineterminator="\n")
        writer.writeheader()
        writer.writerows(goals)
    print(f"sampled {len(goals)} derivable goals "
          f"(round {args.min_round}..{args.max_round})", flush=True)
    traces = WORK / "traces"
    # Clear stale traces (e.g. the earlier base-fact pool) so the trainer only
    # sees the current derivable-goal traces.
    if traces.exists():
        shutil.rmtree(traces)
    for goal in goals:
        d = traces / goal["goal_id"] / "deterministic"
        d.mkdir(parents=True, exist_ok=True)
        cmd = ["./build/bin/gar_reason", "--mode", "agent", "--engine", "cpu",
               "--csr", CSR, "--rules", str(RULES), "--goal", goal["spec"],
               "--out", str(d), "--print", "0", "--star-order", "goal",
               "--scorer", "exact", "--budget", "1000000000",
               "--wall-limit", "300", "--max-candidates-per-rule", "100000",
               "--hdc-dim", "128", "--policy", "deterministic",
               "--policy-seed", "7", "--policy-trace", str(d / "policy_trace.json"),
               "--policy-trace-negatives", "8"]
        subprocess.run(cmd, cwd=ROOT, capture_output=True)
    model = WORK / f"model_h{args.hidden}" / "relevance.meta"
    cmd = ["./build/bin/gar_train_policy", "--csr", CSR, "--rules", str(RULES),
           "--traces", str(traces), "--test-goals", str(TEST_GOALS),
           "--out", str(model), "--epochs", str(args.epochs),
           "--batch-size", "64", "--hidden", str(args.hidden),
           "--label-gamma", "0.5", "--negative-ratio", "4", "--seed", "42"]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    print(r.stdout[-2000:], flush=True)
    print(r.stderr[-1000:], flush=True)
    report = model.with_suffix(".report.json")
    if report.exists():
        print(json.dumps(json.loads(report.read_text()), indent=2), flush=True)


if __name__ == "__main__":
    main()