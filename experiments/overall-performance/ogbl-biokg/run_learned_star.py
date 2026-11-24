#!/usr/bin/env python3
"""Train the LearnedStar goal-level star-order selector (pure forward, no backward).

LearnedStar chooses between input and goal star order per goal via a goal-level
logistic model on (cone_size, log1p pred_degree), mirroring the path-C strategy
selector. Training labels come from running BOTH fixed orders on train goals and
marking which derives fewer facts. The model is saved as gar_star_order_v1 and
consumed by --star-order learned --star-order-model.

Note: the oracle best-of-two headroom is -16.9%, but the order choice tracks the
instance-level derivation depth (round), which predicate-level cone/degree cannot
fully predict; this measures how much a cheap goal-level model actually captures.
"""
import argparse, csv, subprocess
from pathlib import Path
import numpy as np
from sklearn.linear_model import LogisticRegression
from sklearn.model_selection import cross_val_score

ROOT = Path(__file__).resolve().parents[3]
CSR = "/ssd_data/zhuxk/graphs/KG/ogbl-biokg/immutable_csr"
RULES = ROOT / ".work_hdcv/overall-performance/ogbl-biokg/rules"
TRAIN = ROOT / ".work_hdcv/overall-performance/ogbl-biokg/train_goals.tsv"


def ys(p):
    d = {}
    for l in p.read_text().splitlines():
        if ":" in l:
            k, v = l.split(":", 1); d[k.strip()] = v.strip()
    return d


def run_order(spec, out, order, wall):
    out.mkdir(parents=True, exist_ok=True)
    sp = out / "summary.yaml"
    if sp.exists():
        return ys(sp)
    cmd = ["./build/bin/gar_reason", "--mode", "agent", "--engine", "cpu",
           "--csr", CSR, "--rules", str(RULES), "--goal", spec,
           "--out", str(out), "--print", "0", "--star-order", order,
           "--scorer", "exact", "--budget", "1000000000",
           "--wall-limit", str(wall), "--max-candidates-per-rule", "10000000",
           "--hdc-dim", "128", "--policy", "deterministic", "--policy-seed", "7",
           "--fair-interval", "0"]
    subprocess.run(cmd, cwd=ROOT, capture_output=True)
    return ys(sp) if sp.exists() else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", required=True)
    ap.add_argument("--wall-limit", type=int, default=300)
    ap.add_argument("--out-model", required=True)
    args = ap.parse_args()
    work = Path(args.work)
    goals = list(csv.DictReader(TRAIN.open(), delimiter="\t"))
    # features
    feats_tsv = work / "train_feats.tsv"
    feats_tsv.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["./build/bin/gar_goal_features", "--csr", CSR,
                    "--rules", str(RULES), "--goals-tsv", str(TRAIN),
                    "--out", str(feats_tsv)], cwd=ROOT, capture_output=True)
    feats = {r["goal_id"]: r for r in csv.DictReader(feats_tsv.open(), delimiter="\t")}
    X, y, ids = [], [], []
    for g in goals:
        f = feats.get(g["goal_id"])
        if not f:
            continue
        gd = run_order(g["spec"], work / g["goal_id"] / "goal", "goal", args.wall_limit)
        idd = run_order(g["spec"], work / g["goal_id"] / "input", "input", args.wall_limit)
        if not gd or not idd:
            continue
        gv, iv = int(gd.get("derived", 0)), int(idd.get("derived", 0))
        X.append([int(f["cone_size"]), np.log1p(int(f["pred_degree"]))])
        y.append(1 if iv < gv else 0)  # 1 = input order better
        ids.append((g["goal_id"], g["stratum"], gv, iv))
    X, y = np.array(X), np.array(y)
    print(f"train goals: {len(y)}  input-better: {int(y.sum())}/{len(y)}")
    for gid, st, gv, iv in ids:
        print(f"  {gid:5s} {st:7s} goal={gv:9d} input={iv:9d} label={'input' if iv<gv else 'goal'}")
    clf = LogisticRegression(C=1.0, max_iter=1000)
    if len(np.unique(y)) > 1:
        cv = cross_val_score(clf, X, y, cv=min(5, len(y)), scoring="accuracy")
        print(f"CV accuracy: {cv.mean():.3f} +/- {cv.std():.3f}")
    clf.fit(X, y)
    w0, w1 = clf.coef_[0]
    b = clf.intercept_[0]
    print(f"weights: w0(cone)={w0:.6f} w1(logdeg)={w1:.6f} b={b:.6f}")
    out = Path(args.out_model)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(f"gar_star_order_v1\nfeatures cone_size log1p_pred_degree\n"
                   f"w0 {w0:.10f}\nw1 {w1:.10f}\nb {b:.10f}\nthreshold 0.5\n")
    print(f"model saved: {out}")


if __name__ == "__main__":
    main()