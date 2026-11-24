#!/usr/bin/env python3
"""Generalization attack: L2 + capacity sweep on the cosine relevance model.

The overfit probe showed the HDC representation has full expressiveness (shallow
goals memorize to derived=1) but the generalized model only reaches -4.4%. The
gap is generalization: the model memorizes goal-specific proof facts instead of
learning the transferable "high goal-cosine -> relevant" rule. This sweeps L2
regularization and hidden capacity on the existing 200-goal traces and measures
held-out test derived ratio, seeking a config that generalizes better than the
no-regularization baseline.
"""
import argparse, csv, json, statistics, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
WORK = ROOT / ".work_hdcv/overall-performance/ogbl-biokg-generalize"
CSR = "/ssd_data/zhuxk/graphs/KG/ogbl-biokg/immutable_csr"
RULES = ROOT / ".work_hdcv/overall-performance/ogbl-biokg/rules"
TRACES = ROOT / ".work_hdcv/overall-performance/ogbl-biokg-learn/traces"
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


def train(hidden, l2, epochs, tag):
    model = WORK / tag / "relevance.meta"
    model.parent.mkdir(parents=True, exist_ok=True)
    if model.exists():
        return model
    cmd = ["./build/bin/gar_train_policy", "--csr", CSR, "--rules", str(RULES),
           "--traces", str(TRACES), "--test-goals", str(TEST_GOALS),
           "--out", str(model), "--label", "proof", "--hdc-dim", "128",
           "--epochs", str(epochs), "--batch-size", "64", "--hidden", str(hidden),
           "--l2", str(l2), "--label-gamma", "0.5", "--negative-ratio", "4",
           "--ranking", "--seed", "42"]
    subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    return model if model.exists() else None


def eval_model(model, goals, seeds, tag):
    ratios, wins, n = [], 0, 0
    for g in goals:
        dv, lv = [], []
        for s in seeds:
            dd = WORK / "eval" / g["goal_id"] / "Det" / f"s{s}"
            dd.mkdir(parents=True, exist_ok=True)
            if not (dd / "summary.yaml").exists():
                subprocess.run(base_cmd(g["spec"], dd, s) + ["--policy", "deterministic"],
                               cwd=ROOT, capture_output=True)
            ld = WORK / "eval" / g["goal_id"] / tag / f"s{s}"
            ld.mkdir(parents=True, exist_ok=True)
            if not (ld / "summary.yaml").exists():
                subprocess.run(base_cmd(g["spec"], ld, s) + [
                    "--policy", "learned", "--relevance-model", str(model),
                    "--learned-handcrafted-weight", "0", "--learned-relevance-weight", "1"],
                    cwd=ROOT, capture_output=True)
            if (dd / "summary.yaml").exists():
                dv.append(int(ys(dd / "summary.yaml").get("derived", 0)))
            if (ld / "summary.yaml").exists():
                lv.append(int(ys(ld / "summary.yaml").get("derived", 0)))
        if len(dv) == len(seeds) and len(lv) == len(seeds):
            dm, lm = statistics.median(dv), statistics.median(lv)
            ratios.append(lm / dm if dm else 1.0)
            wins += lm < dm; n += 1
    return {"mean_ratio": statistics.mean(ratios) if ratios else 1.0,
            "wins": wins, "goals": n}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--eval-goals", type=int, default=12)
    ap.add_argument("--seeds", default="7,13,42")
    ap.add_argument("--epochs", type=int, default=30)
    ap.add_argument("--configs", default="4:0,4:0.001,4:0.01,4:0.1,8:0.01,2:0.01",
                    help="hidden:l2 pairs")
    args = ap.parse_args()
    WORK.mkdir(parents=True, exist_ok=True)
    test = list(csv.DictReader(TEST_GOALS.open(), delimiter="\t"))[:args.eval_goals]
    seeds = [int(s) for s in args.seeds.split(",")]
    results = {}
    for cfg in args.configs.split(","):
        hidden, l2 = cfg.split(":")
        tag = f"h{hidden}_l2{l2}"
        model = train(int(hidden), float(l2), args.epochs, tag)
        if not model:
            print(f"{tag}: TRAIN FAILED", flush=True); continue
        rep = model.with_suffix(".report.json")
        pa = json.loads(rep.read_text()).get("epoch_pairwise_accuracy", []) if rep.exists() else []
        m = eval_model(model, test, seeds, tag)
        results[tag] = m
        print(f"{tag:14s} pairwise={pa[-1] if pa else 0:.3f} "
              f"test_ratio={m['mean_ratio']:.3f} wins={m['wins']}/{m['goals']}", flush=True)
    (WORK / "summary_generalize.json").write_text(json.dumps(results, indent=2) + "\n")
    best = min(results, key=lambda k: results[k]["mean_ratio"]) if results else None
    if best:
        print(f"\nBEST: {best} ratio={results[best]['mean_ratio']:.3f}", flush=True)


if __name__ == "__main__":
    main()