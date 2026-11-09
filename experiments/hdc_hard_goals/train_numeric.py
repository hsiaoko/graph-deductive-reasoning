#!/usr/bin/env python3
"""Numeric-ridge value models (BNB-numeric-value arm) — same traces, same
labels, same LOGO isolation as train_export.py (HDC arm); only the feature
map differs: raw 11 features + the same 4 conjunction products, ridge on
standardized values (mean/std fit on the training rows of each LOGO fold
only, exported in the meta).

Export format: numeric_ridge_v1
  meta: names (11, same order), conj pairs, mean/std per feature, lam,
        trained_on, train_nodes
  bin:  16 float64 weights = 11 features + 4 conj products + bias
"""
import sys, re, glob, os
import numpy as np

W = "/home/zhuxk/project/graph/GARAgent/.work_hdcv"
OUT = f"{W}/hdc_hard_goals/models"
NAMES = ["cost", "clen", "ncand", "minprice", "relfam", "relevd", "conf",
         "dcost", "total", "dp0", "wit"]
CONJ = [(0, 9), (2, 7), (6, 8), (0, 1)]  # cost*dp0, ncand*dcost, conf*total, cost*clen
LAM = 1.0

TEST_GOALS = {
    "g0": f"{W}/g0.log",
    "r2c_2": f"{W}/r2c_2.log",
    "r2c_4": f"{W}/r2c_4.log",
    "r2c_8": f"{W}/r2c_8.log",
}
SUCCESS_LOGS = [f"{W}/g0.log", f"{W}/g6.log", f"{W}/g9.log", f"{W}/g22.log",
                f"{W}/r2c_2.log", f"{W}/r2c_4.log", f"{W}/r2c_8.log"]
FAIL_LOGS = sorted(set(glob.glob(f"{W}/g*.log") + glob.glob(f"{W}/r2c_*.log"))
                   - set(SUCCESS_LOGS) - set(TEST_GOALS.values()))

fork_re = re.compile(r"\[bnbl\] fork (\d+) (\d+) ([\d.]+) ([\d.eE+-]+) (\d+)")
eval_re = re.compile(r"\[bnbl\] eval (\d+) (\d+) ([\d.eE+-]+) ([\d.eE+-]+) "
                     r"([\d.eE+-]+) ([\d.eE+-]+) ([\d.eE+-]+) ([\d.eE+-]+) "
                     r"(\d+) (\d+)")
expand_re = re.compile(r"\[bnbl\] expand (\d+) (\d+) (\d) ([\d.]+)")
goal_re = re.compile(r"\[bnbl\] goal (\d+) ([\d.]+)")

def parse_goal(log):
    """Identical to train_export.py: nodes/expands/goals + cost table."""
    nodes, expands, goals = {}, [], []
    for line in open(log, errors="replace"):
        m = fork_re.search(line)
        if m:
            nodes[int(m[1])] = dict(parent=int(m[2]), pcost=float(m[3]),
                                    chain_len=int(m[5]))
            continue
        m = eval_re.search(line)
        if m:
            s = int(m[1])
            nodes.setdefault(s, dict(parent=None))
            nodes[s]["eval"] = [float(m.group(2))] + \
                [float(x) for x in m.groups()[2:]]
            continue
        m = expand_re.search(line)
        if m:
            expands.append((int(m[1]), int(m[2]), int(m[3]), float(m[4])))
            continue
        m = goal_re.search(line)
        if m:
            goals.append((int(m[1]), float(m[2])))
    cost = {}
    for s, r, ok, c in expands:
        cost.setdefault(s, c)
    for s, nd in nodes.items():
        if s not in cost:
            p = nd.get("parent")
            cost[s] = (nd.get("pcost", 0.0) if p is None
                       else cost.get(p, nd.get("pcost", 0.0)) + 1.0)
    return nodes, expands, goals, cost

def collect(log, want_labels):
    """Identical labeling to train_export.py, but RAW features (no HDC)."""
    nodes, expands, goals, cost = parse_goal(log)
    X, y = [], []
    if want_labels:
        if not goals:
            return None, None
        best = min(c for _, c in goals)
        goal_node = next(s for s, c in goals if c == best)
        on_path, cur = set(), goal_node
        while cur is not None and cur in nodes:
            on_path.add(cur)
            cur = nodes[cur].get("parent")
        for s, nd in nodes.items():
            ev = nd.get("eval")
            if ev is None:
                continue
            X.append([cost[s], nd.get("chain_len", 0)] + ev)
            y.append((best - cost[s]) if s in on_path else 3.0)
    else:
        for s, nd in nodes.items():
            ev = nd.get("eval")
            if ev is None:
                continue
            X.append([cost[s], nd.get("chain_len", 0)] + ev)
            y.append(3.0)
    return (np.array(X) if X else None), (np.array(y) if y else None)

def export(name, mu, sd, w, trained_on, n):
    os.makedirs(OUT, exist_ok=True)
    base = f"{OUT}/{name}"
    with open(base + ".meta", "w") as f:
        f.write("format numeric_ridge_v1\n")
        f.write(f"n_features {len(NAMES)}\n")
        f.write("names " + " ".join(NAMES) + "\n")
        for a, b in CONJ:
            f.write(f"conj {a} {b}\n")
        f.write("mean " + " ".join(f"{v:.17g}" for v in mu) + "\n")
        f.write("std " + " ".join(f"{v:.17g}" for v in sd) + "\n")
        f.write(f"lam {LAM}\n")
        f.write("trained_on " + " ".join(trained_on) + "\n")
        f.write(f"train_nodes {n}\n")
    with open(base + ".bin", "wb") as f:
        f.write(np.asarray(w, dtype=np.float64).tobytes())

for name, test_log in TEST_GOALS.items():
    Xs, ys = [], []
    trained_on = []
    for log in SUCCESS_LOGS:
        if log == test_log:
            continue
        X, y = collect(log, want_labels=True)
        if X is not None:
            Xs.append(X); ys.append(y)
            trained_on.append(os.path.basename(log))
    for log in FAIL_LOGS:
        X, y = collect(log, want_labels=False)
        if X is not None:
            Xs.append(X); ys.append(y)
    X = np.vstack(Xs)
    y = np.concatenate(ys)
    mu = X.mean(axis=0)                      # fit on THIS fold's train rows
    sd = X.std(axis=0); sd[sd == 0] = 1.0
    Z = (X - mu) / sd
    F = np.hstack([Z] + [(Z[:, a] * Z[:, b])[:, None] for a, b in CONJ])
    F1 = np.hstack([F, np.ones((len(F), 1))])
    w = np.linalg.solve(F1.T @ F1 + LAM * np.eye(F1.shape[1]), F1.T @ y)
    export(name, mu, sd, w, trained_on, len(y))
    print(f"{name}: trained_on={trained_on} nodes={len(y)} "
          f"w_norm={np.linalg.norm(w):.2f}", file=sys.stderr)
print("exported to", OUT)
