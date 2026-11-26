#!/usr/bin/env python3
"""FinReflectKG stage 1: sample goals, generate Deterministic traces, train the
Learned relevance model. Test goals (50) are stratified by closure derivation
round; train goals (150, disjoint) come from rounds 1-3 for fast trace generation.
Goals are derived closure facts (name-format closure_edges.tsv), not base edges.
"""
import argparse, csv, random, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CSR = "/ssd_data/zhuxk/graphs/KG/finreflectkg/immutable_csr"
RULES = "/ssd_data/zhuxk/graphs/KG/finreflectkg/rules"
CLOSURE = Path("/tmp/fin_chase/closure_edges.tsv")
WORK = ROOT / ".work_hdcv/overall-performance/finreflectkg-50"

TEST_QUOTA = {1: 8, 2: 8, 3: 8, 4: 8, 5: 8, 6: 6, 7: 4}


def load_closure():
    by_round = {}
    for l in CLOSURE.open():
        if l.startswith("#"):
            continue
        p = l.rstrip("\n").split("\t")
        if len(p) < 5:
            continue
        src, rel, dst, rnd, rule = p[0], p[1], p[2], int(p[3]), p[4]
        by_round.setdefault(rnd, []).append((rel, src, dst, rnd))
    return by_round


def write_goals(goals, path):
    with path.open("w") as f:
        w = csv.writer(f, delimiter="\t")
        w.writerow(["goal_id", "round", "relation", "src", "dst", "spec"])
        for i, (rel, src, dst, rnd) in enumerate(goals):
            w.writerow([f"g{i:03d}", rnd, rel, src, dst, f"{rel}({src},{dst})"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--train-goals", type=int, default=150)
    ap.add_argument("--hidden", type=int, default=4)
    ap.add_argument("--epochs", type=int, default=30)
    ap.add_argument("--seed", type=int, default=20261124)
    ap.add_argument("--skip-train", action="store_true")
    args = ap.parse_args()
    WORK.mkdir(parents=True, exist_ok=True)
    rng = random.Random(args.seed)
    by_round = load_closure()

    test = []
    for rnd, quota in TEST_QUOTA.items():
        pool = by_round.get(rnd, [])
        rng.shuffle(pool)
        test.extend(pool[:quota])
    write_goals(test, WORK / "test_goals.tsv")
    test_keys = {(r[0], r[1], r[2]) for r in test}

    train_pool = []
    for rnd in (1, 2, 3):
        for r in by_round.get(rnd, []):
            if (r[0], r[1], r[2]) not in test_keys:
                train_pool.append(r)
    rng.shuffle(train_pool)
    train = train_pool[:args.train_goals]
    write_goals(train, WORK / "train_goals.tsv")
    print(f"test goals: {len(test)}  train goals: {len(train)}", flush=True)

    traces = WORK / "traces"
    for i, (rel, src, dst, rnd) in enumerate(train):
        spec = f"{rel}({src},{dst})"
        d = traces / f"t{i:04d}" / "deterministic"
        d.mkdir(parents=True, exist_ok=True)
        if (d / "policy_trace.json").exists():
            continue
        subprocess.run(["./build/bin/gar_reason", "--mode", "agent", "--engine",
                        "cpu", "--search-direction", "forward", "--policy",
                        "deterministic", "--star-order", "goal", "--scorer",
                        "exact", "--csr", CSR, "--rules", RULES, "--goal", spec,
                        "--out", str(d), "--print", "0", "--budget", "1000000000",
                        "--wall-limit", "120", "--max-candidates-per-rule",
                        "10000000", "--hdc-dim", "128", "--policy-seed", "7",
                        "--fair-interval", "0", "--policy-trace",
                        str(d / "policy_trace.json"), "--policy-trace-negatives",
                        "64"], cwd=ROOT, capture_output=True)
        if i % 25 == 0:
            print(f"  traces {i}/{len(train)}", flush=True)
    print("traces done", flush=True)

    if args.skip_train:
        return
    model = WORK / f"model_h{args.hidden}" / "relevance.meta"
    model.parent.mkdir(parents=True, exist_ok=True)
    r = subprocess.run(["./build/bin/gar_train_policy", "--csr", CSR, "--rules",
                        RULES, "--traces", str(traces), "--test-goals",
                        str(WORK / "test_goals.tsv"), "--out", str(model),
                        "--label", "proof", "--ranking", "--hdc-dim", "128",
                        "--hidden", str(args.hidden), "--epochs", str(args.epochs),
                        "--batch-size", "64", "--seed", "42"],
                       cwd=ROOT, capture_output=True, text=True)
    print(r.stdout[-800:], flush=True)
    if r.returncode != 0:
        print("TRAIN ERR:", r.stderr[-800:], flush=True)
    print(f"model: {model}", flush=True)


if __name__ == "__main__":
    main()