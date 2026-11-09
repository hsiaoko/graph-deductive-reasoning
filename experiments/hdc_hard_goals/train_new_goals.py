#!/usr/bin/env python3
"""Train HDC + numeric LOGO value models for the NEW evaluation goals
(g6/g9/g22), with strict per-goal exclusion of that goal's success traces.

Shares parsing/labeling/encoding conventions with train_export.py (HDC)
and train_numeric.py (numeric) but is a standalone script so the accepted
anchor models are never regenerated or altered. Per new test goal it emits:
  <OUT>/gX.meta+.bin     (hdc_value_v1, same keys/vectors as train_export)
  <OUT>/gX_num.meta+.bin (numeric_ridge_v1, standardization fit per fold)
"""
import sys, re, glob, os
import numpy as np

W = "/home/zhuxk/project/graph/GARAgent/.work_hdcv"
OUT = f"{W}/hdc_hard_goals/models"
D = 4096
NAMES = ["cost", "clen", "ncand", "minprice", "relfam", "relevd", "conf",
         "dcost", "total", "dp0", "wit"]
CONJ = [(0, 9), (2, 7), (6, 8), (0, 1)]
BIN_CAP, BIN_SCALE, LAM = 7, 2.0, 1.0

ALL_SUCCESS = [f"{W}/g0.log", f"{W}/g6.log", f"{W}/g9.log", f"{W}/g22.log",
               f"{W}/r2c_2.log", f"{W}/r2c_4.log", f"{W}/r2c_8.log"]
POOL_ONLY = os.environ.get("HDC_HARD_POOL") == "1"
NEW_GOALS = ["hard_pool"] if POOL_ONLY else ["g6", "g9", "g22"]
FAIL_LOGS = sorted(set(glob.glob(f"{W}/g*.log") + glob.glob(f"{W}/r2c_*.log"))
                   - set(ALL_SUCCESS)
                   - {f"{W}/{t}.log" for t in ("r2c_2", "r2c_4", "r2c_8")})

fork_re = re.compile(r"\[bnbl\] fork (\d+) (\d+) ([\d.]+) ([\d.eE+-]+) (\d+)")
eval_re = re.compile(r"\[bnbl\] eval (\d+) (\d+) ([\d.eE+-]+) ([\d.eE+-]+) "
                     r"([\d.eE+-]+) ([\d.eE+-]+) ([\d.eE+-]+) ([\d.eE+-]+) "
                     r"(\d+) (\d+)")
expand_re = re.compile(r"\[bnbl\] expand (\d+) (\d+) (\d) ([\d.]+)")
goal_re = re.compile(r"\[bnbl\] goal (\d+) ([\d.]+)")

def v(key):
    r = np.random.default_rng(abs(hash(("hv2", key))) % (1 << 32))
    return r.choice([-1.0, 1.0], size=D)

def parse_goal(log):
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

def hdc_feat(vals):
    bins = [int(min(BIN_CAP, max(0, np.floor(abs(x) * BIN_SCALE))))
            for x in vals]
    vb = [v(("f", NAMES[j], bins[j])) for j in range(len(NAMES))]
    hv = np.zeros(D)
    for x in vb:
        hv += x
    for a, b in CONJ:
        hv += vb[a] * vb[b]
    hv += v(("bias",))
    return hv

def ridge(X, y):
    X1 = np.hstack([X, np.ones((len(X), 1))])
    return np.linalg.solve(X1.T @ X1 + LAM * np.eye(X1.shape[1]), X1.T @ y)

os.makedirs(OUT, exist_ok=True)
KEYS = [(n, b) for n in NAMES for b in range(BIN_CAP + 1)] + [("bias", 0)]
base_vecs = np.stack([v(("f", n, b)) if n != "bias" else v(("bias",))
                      for n, b in KEYS]).astype(np.int8)

for name in NEW_GOALS:
    test_log = None if POOL_ONLY else f"{W}/{name}.log"
    Xs, ys = [], []
    trained_on = []
    for log in ALL_SUCCESS:
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
    X = np.vstack(Xs); y = np.concatenate(ys)
    n = len(y)
    # ---- HDC model (train_export.py encoding, verbatim) ----
    Xh = np.stack([hdc_feat(row) for row in X])
    wh = ridge(Xh, y)
    base = f"{OUT}/{name}"
    with open(base + ".meta", "w") as f:
        f.write("format hdc_value_v1\n")
        f.write(f"dim {D}\nn_keys {len(KEYS)}\n")
        f.write(f"bin_cap {BIN_CAP}\nbin_scale {BIN_SCALE}\n")
        f.write(f"n_features {len(NAMES)}\n")
        f.write("names " + " ".join(NAMES) + "\n")
        for i, (k, b) in enumerate(KEYS):
            f.write(f"key {i} {k} {b}\n")
        for a, b in CONJ:
            f.write(f"conj {a} {b}\n")
        f.write(f"lam {LAM}\n")
        f.write("trained_on " + " ".join(trained_on) + "\n")
        f.write(f"train_nodes {n}\n")
    with open(base + ".bin", "wb") as f:
        f.write(base_vecs.tobytes())
        f.write(wh.astype(np.float64).tobytes())
    # ---- numeric model (train_numeric.py conventions, verbatim) ----
    mu = X.mean(axis=0)
    sd = X.std(axis=0); sd[sd == 0] = 1.0
    Z = (X - mu) / sd
    F = np.hstack([Z] + [(Z[:, a] * Z[:, b])[:, None] for a, b in CONJ])
    wn = ridge(F, y)
    with open(base + "_num.meta", "w") as f:
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
    with open(base + "_num.bin", "wb") as f:
        f.write(np.asarray(wn, dtype=np.float64).tobytes())
    excluded = "shared pool; no test goal" if POOL_ONLY else (
        f"excluded {os.path.basename(test_log)} from success set")
    print(f"{name}: trained_on={trained_on} nodes={n} ({excluded})",
          file=sys.stderr)
print("exported to", OUT)
