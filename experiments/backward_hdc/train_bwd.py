#!/usr/bin/env python3
# Train LOGO value models for the backward executor's OR-attempt ordering.
# Features (11, same order as the C++ call site + the att-result log):
#   open_d df tier relfam conf nrules vals1 vals2 depth spent remaining
# Label: attempt outcome succ (1 = attempt produced the target's facts).
# Ridge on HDC random features (same encoding pipeline as hdc_online).
import sys, re, os
import numpy as np

W = "/home/zhuxk/project/graph/GARAgent/.work_hdcv"
TRACE = f"{W}/bwd_traces.log"
OUT = f"{W}/models_bwd"
D, LAM = 4096, 1.0
NAMES = ["open_d", "df", "tier", "relfam", "conf", "nrules", "vals1",
         "vals2", "depth", "spent", "remaining"]
CONJ = [(0, 9), (2, 7), (6, 8), (0, 1)]
BIN_CAP, BIN_SCALE = 7, 2.0

att_re = re.compile(
    r"\[bwd\]   att-result rule=(\d+) oe=\((\d+),(\d+)\) df=(\d) tier=(\d) "
    r"open_d=(\d+) relfam=([\d.eE+-]+) conf=([\d.eE+-]+) nrules=(\d+) "
    r"vals1=(\d+) vals2=(\d+) spent=(\d+) remain=(\d+) succ=(\d) "
    r"dspent=(\d+) depth=(\d+)")
goal_re = re.compile(r"^=== goal (\d+) ")
done_re = re.compile(r"^=== goal (\d+) done derived=(\d)")

def v(key):
    r = np.random.default_rng(abs(hash(("bwdv", key))) % (1 << 32))
    return r.choice([-1.0, 1.0], size=D)

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

# parse: goal -> attempts (features, succ)
data = {}
cur = None
for line in open(TRACE, errors="replace"):
    m = goal_re.match(line)
    if m:
        cur = int(m.group(1))
        continue
    m = done_re.match(line)
    if m:
        cur = None
        continue
    m = att_re.search(line)
    if m and cur is not None:
        g = m.groups()
        # rule oe1 oe2 df tier open_d relfam conf nrules vals1 vals2 spent
        # remain succ dspent depth
        feats = [int(g[5]), int(g[3]), int(g[4]), float(g[6]), float(g[7]),
                 int(g[8]), int(g[9]), int(g[10]), int(g[15]), int(g[11]),
                 int(g[12])]
        data.setdefault(cur, []).append((feats, int(g[13])))

os.makedirs(OUT, exist_ok=True)
KEYS = [(n, b) for n in NAMES for b in range(BIN_CAP + 1)] + [("bias", 0)]
base_vecs = np.stack([v(("f", n, b)) if n != "bias" else v(("bias",))
                      for n, b in KEYS]).astype(np.int8)
ngoals = sorted(data)
print(f"attempts total={sum(len(v) for v in data.values())} "
      f"goals_with_attempts={len(ngoals)} "
      f"succ={sum(a[1] for v in data.values() for a in v)}", file=sys.stderr)
for tg in ngoals:
    Xs, ys, ton = [], [], []
    for g in ngoals:
        if g == tg:
            continue
        for feats, succ in data[g]:
            Xs.append(hdc_feat(feats))
            ys.append(0.0 if succ else 1.0)  # predict failure prob
        ton.append(str(g))
    X = np.stack(Xs)
    y = np.array(ys)
    X1 = np.hstack([X, np.ones((len(X), 1))])
    w = np.linalg.solve(X1.T @ X1 + LAM * np.eye(D + 1), X1.T @ y)
    base = f"{OUT}/g{tg}"
    with open(base + ".meta", "w") as f:
        f.write(f"format hdc_value_v1\ndim {D}\nn_keys {len(KEYS)}\n")
        f.write(f"bin_cap {BIN_CAP}\nbin_scale {BIN_SCALE}\n")
        f.write(f"n_features {len(NAMES)}\n")
        f.write("names " + " ".join(NAMES) + "\n")
        for i, (n, b) in enumerate(KEYS):
            f.write(f"key {i} {n} {b}\n")
        for a, b in CONJ:
            f.write(f"conj {a} {b}\n")
        f.write(f"lam {LAM}\ntrained_on {' '.join(ton)}\ntrain_nodes {len(y)}\n")
    with open(base + ".bin", "wb") as f:
        f.write(base_vecs.tobytes())
        f.write(w.astype(np.float64).tobytes())
print("exported", len(ngoals), "LOGO models to", OUT)
