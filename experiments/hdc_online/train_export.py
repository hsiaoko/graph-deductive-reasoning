#!/usr/bin/env python3
# Train LOGO value models from GAR_DEBUG_BNB traces and export
# (meta.txt + weights.bin) for the C++ ValueModel.
# Feature/encoding pipeline mirrors experiments/hdc_value2/analyze2.py exactly.
import sys, re, glob, collections
import numpy as np

W = "/home/zhuxk/project/graph/GARAgent/.work_hdcv"
OUT = f"{W}/models"
D = 4096
NAMES = ["cost", "clen", "ncand", "minprice", "relfam", "relevd", "conf",
         "dcost", "total", "dp0", "wit"]
CONJ = [(0, 9), (2, 7), (6, 8), (0, 1)]
BIN_CAP, BIN_SCALE = 7, 2.0
LAM = 1.0

# (test_goal_name, log) — the 4 non-trivial goals from hdc_value2
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
            X.append(hdc_feat([cost[s], nd.get("chain_len", 0)] + ev))
            y.append((best - cost[s]) if s in on_path else 3.0)
    else:
        for s, nd in nodes.items():
            ev = nd.get("eval")
            if ev is None:
                continue
            X.append(hdc_feat([cost[s], nd.get("chain_len", 0)] + ev))
            y.append(3.0)
    return (np.array(X) if X else None), (np.array(y) if y else None)

def ridge(X, y):
    X1 = np.hstack([X, np.ones((len(X), 1))])
    return np.linalg.solve(X1.T @ X1 + LAM * np.eye(D + 1), X1.T @ y)

# export base vectors once (shared by all models: same encoding)
import os
os.makedirs(OUT, exist_ok=True)
KEYS = [(n, b) for n in NAMES for b in range(BIN_CAP + 1)] + [("bias", 0)]
base_vecs = np.stack([v(("f", n, b)) if n != "bias" else v(("bias",))
                      for n, b in KEYS]).astype(np.int8)

for name, test_log in TEST_GOALS.items():
    Xs, ys = [], []
    trained_on = []
    for log in SUCCESS_LOGS:
        if log == test_log:
            continue
        X, y = collect(log, want_labels=True)
        if X is not None:
            Xs.append(X)
            ys.append(y)
            trained_on.append(os.path.basename(log))
    for log in FAIL_LOGS:
        X, y = collect(log, want_labels=False)
        if X is not None:
            Xs.append(X)
            ys.append(y)
    X = np.vstack(Xs)
    y = np.concatenate(ys)
    w = ridge(X, y)
    base = f"{OUT}/{name}"
    with open(base + ".meta", "w") as f:
        f.write("format hdc_value_v1\n")
        f.write(f"dim {D}\nn_keys {len(KEYS)}\n")
        f.write(f"bin_cap {BIN_CAP}\nbin_scale {BIN_SCALE}\n")
        f.write(f"n_features {len(NAMES)}\n")
        f.write("names " + " ".join(NAMES) + "\n")
        for i, (n, b) in enumerate(KEYS):
            f.write(f"key {i} {n} {b}\n")
        for a, b in CONJ:
            f.write(f"conj {a} {b}\n")
        f.write(f"lam {LAM}\n")
        f.write("trained_on " + " ".join(trained_on) + "\n")
        f.write(f"train_nodes {len(y)}\n")
    with open(base + ".bin", "wb") as f:
        f.write(base_vecs.tobytes())
        f.write(w.astype(np.float64).tobytes())
    print(f"{name}: trained_on={trained_on} nodes={len(y)} "
          f"w_norm={np.linalg.norm(w):.2f}", file=sys.stderr)
print("exported to", OUT)
