#!/usr/bin/env python3
# Gate-1 analysis: do structure-preserving subgoal encodings predict reasoning
# outcomes beyond a predicate success-rate counting table?
# Encodings (D-dim bipolar, bundling composition; shift = positional roll):
#   RANDOM: all symbols random.
#   A (schema): v_pred = majority bundle of quantized feature symbols
#     (head/body rule counts, instance/degree bins); entities random.
#   B (neighborhood): v_ent = majority bundle of bind(v_rel, rho v_nbr) over
#     the base graph; predicates random.
#   AB: both.
# Subgoal vector = sign(v_pred + rho^1 v_src + rho^2 v_dst).
# Metrics under goal-level 10-fold CV (a goal's subgoals never leak into the
# train fold): cosine-band outcome-agreement lift; 1-NN accuracy vs the
# per-predicate counting table.
import sys, collections
import numpy as np

DSV = sys.argv[1]          # subgoals tsv from parse_traces.py
RULES = sys.argv[2]        # rule_names.txt
EDGES = sys.argv[3]        # base_edges.tsv
D = 4096
rng = np.random.default_rng(123)

LABEL = sys.argv[4] if len(sys.argv) > 4 else "realized"
rows = []
with open(DSV) as f:
    next(f)
    for line in f:
        p = line.rstrip("\n").split("\t")
        o = int(p[4]) if LABEL == "realized" else int(p[5])
        rows.append((int(p[0]), p[1], p[2], p[3], o))
preds = sorted({r[1] for r in rows})
ents = sorted({r[2] for r in rows} | {r[3] for r in rows})
pidx = {p: i for i, p in enumerate(preds)}
eidx = {e: i for i, e in enumerate(ents)}
NP_, NE_ = len(preds), len(ents)
print(f"instances={len(rows)} preds={NP_} ents={NE_} pos_rate="
      f"{np.mean([r[4] for r in rows]):.3f}", file=sys.stderr)

# ---------- predicate schema features ----------
head_cnt = collections.Counter()
body_cnt = collections.Counter()
with open(RULES) as f:
    for line in f:
        if "→" not in line:
            continue
        body, head = line.split("→")
        head_pred = head.split("(")[0].strip().rstrip("^-1")
        # strip ^-1 suffixes
        hp = head.split("(")[0].strip()
        hp = hp[:-2] if hp.endswith("^-1") else hp
        head_cnt[hp] += 1
        for atom in body.split("∧"):
            bp = atom.split("(")[0].replace("name:", "").strip()
            bp = bp[:-2] if bp.endswith("^-1") else bp
            if bp:
                body_cnt[bp] += 1
inst = collections.Counter()
nsrc = collections.defaultdict(set)
ndst = collections.defaultdict(set)
out_e = collections.defaultdict(list)   # entity -> [(rel, nbr)] (out edges)
in_e = collections.defaultdict(list)
with open(EDGES) as f:
    for line in f:
        r, s, d = line.rstrip("\n").split("\t")
        inst[r] += 1
        nsrc[r].add(s)
        ndst[r].add(d)
        out_e[s].append((r, d))
        in_e[d].append((r, s))

def qbin(v, bins=(0, 1, 2, 4, 8, 16, 32, 64, 128, 256, 1 << 30)):
    for i, b in enumerate(bins):
        if v <= b:
            return i
    return len(bins)

def rand_vec(key):
    r = np.random.default_rng(abs(hash(key)) % (1 << 32))
    return r.choice([-1, 1], size=D).astype(np.int8)

def majority(vecs):
    s = np.sum(np.stack(vecs).astype(np.int32), axis=0)
    out = np.sign(s)
    zero = out == 0
    if zero.any():  # deterministic tie, fixed per coordinate
        tie = rand_vec(("tie",))[zero]
        out[zero] = tie
    return out.astype(np.int8)

# ---------- build symbol vectors per encoding ----------
def build(enc):
    VP = np.zeros((NP_, D), dtype=np.int8)
    VE = np.zeros((NE_, D), dtype=np.int8)
    for p, i in pidx.items():
        if enc in ("A", "AB"):
            feats = [("h", qbin(head_cnt[p])), ("b", qbin(body_cnt[p])),
                     ("i", qbin(inst[p])), ("s", qbin(len(nsrc[p]))),
                     ("d", qbin(len(ndst[p])))]
            VP[i] = majority([rand_vec(("feat",) + f) for f in feats])
        else:
            VP[i] = rand_vec(("pred", p))
    if enc in ("B", "AB"):
        VR = {p: rand_vec(("rel", p)) for p in preds}
        for e, i in eidx.items():
            vecs = []
            for (r, x) in out_e.get(e, [])[:64]:
                if r in pidx:
                    vecs.append(VR[r] * np.roll(rand_vec(("e", x)), 1))
            for (r, x) in in_e.get(e, [])[:64]:
                if r in pidx:
                    vecs.append(VR[r] * np.roll(rand_vec(("e", x)), 2))
            VE[i] = majority(vecs) if vecs else rand_vec(("e", e))
    else:
        for e, i in eidx.items():
            VE[i] = rand_vec(("e", e))
    return VP, VE

def subgoal_matrix(VP, VE, idx_rows):
    X = np.zeros((len(idx_rows), D), dtype=np.int8)
    for k, (g, r, s, d, o) in enumerate(idx_rows):
        v = (VP[pidx[r]].astype(np.int16) +
             np.roll(VE[eidx[s]], 1).astype(np.int16) +
             np.roll(VE[eidx[d]], 2).astype(np.int16))
        sv = np.sign(v)
        z = sv == 0
        if z.any():
            sv[z] = rand_vec(("tie",))[z]
        X[k] = sv.astype(np.int8)
    return X

y = np.array([r[4] for r in rows])
goals_of = np.array([r[0] for r in rows])
p_of = np.array([pidx[r[1]] for r in rows])
prior_agree = float(np.mean(y) ** 2 + (1 - np.mean(y)) ** 2)

results = {}
for enc in ("RANDOM", "A", "B", "AB"):
    VP, VE = build(enc)
    X = subgoal_matrix(VP, VE, rows).astype(np.float32)
    # ---- band analysis (cross-goal sampled pairs) ----
    npairs = 400000
    ia = rng.integers(0, len(rows), npairs)
    ib = rng.integers(0, len(rows), npairs)
    same_goal = goals_of[ia] == goals_of[ib]
    ia, ib = ia[~same_goal], ib[~same_goal]
    # exclude identical (rel,src,dst) pairs: their agreement is identity
    # memory, not generalization
    key = np.array([hash(r[1]) ^ (hash(r[2]) << 1) ^ (hash(r[3]) << 2)
                    for r in rows])
    same_fact = key[ia] == key[ib]
    ia, ib = ia[~same_fact], ib[~same_fact]
    agree = (y[ia] == y[ib]).astype(np.float32)
    cos = np.empty(len(ia), dtype=np.float32)
    for k0 in range(0, len(ia), 65536):
        k1 = min(k0 + 65536, len(ia))
        cos[k0:k1] = np.sum(X[ia[k0:k1]] * X[ib[k0:k1]], axis=1) / D
    bands = [(0, .05), (.05, .1), (.1, .2), (.2, .35), (.35, .5), (.5, 1.01)]
    band_stats = []
    for lo, hi in bands:
        m = (cos >= lo) & (cos < hi)
        n = int(m.sum())
        band_stats.append((lo, hi, n,
                           float(agree[m].mean()) if n else float("nan")))
    # ---- goal-level 10-fold: 1-NN vs counting table ----
    uniq_goals = np.unique(goals_of)
    fold_of_goal = {g: i % 10 for i, g in enumerate(uniq_goals)}
    folds = np.array([fold_of_goal[g] for g in goals_of])
    nn_correct = ct_correct = n_test = 0
    for f in range(10):
        te = folds == f
        tr = ~te
        Xtr, Xte, ytr, yte = X[tr], X[te], y[tr], y[te]
        if len(yte) == 0:
            continue
        # subsample for speed
        if len(ytr) > 12000:
            sel = rng.choice(len(ytr), 12000, replace=False)
            Xtr, ytr = Xtr[sel], ytr[sel]
        if len(yte) > 1500:
            sel = rng.choice(len(yte), 1500, replace=False)
            Xte, yte = Xte[sel], yte[sel]
            te_idx = np.where(te)[0][sel]
        else:
            te_idx = np.where(te)[0]
        S = Xte @ Xtr.T
        nn_pred = ytr[np.argmax(S, axis=1)]
        nn_correct += int((nn_pred == yte).sum())
        # counting table
        rate = np.zeros(NP_)
        for p in range(NP_):
            msk = p_of[tr] == p
            rate[p] = ytr[msk].mean() if msk.any() else ytr.mean()
        ct_pred = (rate[p_of[te_idx]] >= 0.5).astype(int)
        ct_correct += int((ct_pred == yte).sum())
        n_test += len(yte)
    results[enc] = (band_stats, nn_correct / n_test, ct_correct / n_test)

print(f"prior_agree={prior_agree:.3f}")
for enc, (bands, nn, ct) in results.items():
    print(f"== {enc}: 1NN={nn:.3f} counter_table={ct:.3f}")
    for lo, hi, n, a in bands:
        print(f"   [{lo:.2f},{hi:.2f}) n={n:>7d} agree={a:.3f} "
              f"lift={a - prior_agree:+.3f}")
