#!/usr/bin/env python3
# hdc_value2: 3-way value-model comparison (engineered / one-hot / HDC)
# with LOGO prediction, key-branch ranking, and offline replay.
import sys, re, glob, collections, heapq
import numpy as np
from scipy.stats import spearmanr

W = "/home/zhuxk/project/graph/GARAgent/.work_hdcv"
LOGS = sorted(glob.glob(f"{W}/g0.log") + glob.glob(f"{W}/g6.log") +
              glob.glob(f"{W}/g9.log") + glob.glob(f"{W}/g22.log") +
              glob.glob(f"{W}/r2_*.log") + glob.glob(f"{W}/r2c_*.log") + [x for x in glob.glob(f"{W}/g*.log") if x not in {f"{W}/g0.log", f"{W}/g6.log", f"{W}/g9.log", f"{W}/g22.log"}])
D = 4096
rng = np.random.default_rng(7)

# rule idx -> head predicate
rule_head = {}
for line in open(f"{W}/rule_heads.tsv"):
    i, rid, h = line.rstrip("\n").split("\t")
    rule_head[int(i)] = h
preds_all = sorted(set(rule_head.values()))
p2i = {p: i for i, p in enumerate(preds_all)}
NPRED = len(preds_all)

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
        for rex, tag in ((fork_re, 0), (eval_re, 1), (expand_re, 2),
                         (goal_re, 3)):
            m = rex.search(line)
            if not m:
                continue
            if tag == 0:
                nodes[int(m[1])] = dict(parent=int(m[2]), pcost=float(m[3]),
                                        chain_len=int(m[5]))
            elif tag == 1:
                s = int(m[1])
                nodes.setdefault(s, dict(parent=None))
                nodes[s]["eval"] = [float(m.group(2))] + \
                    [float(x) for x in m.groups()[2:]]
            elif tag == 2:
                expands.append((int(m[1]), int(m[2]), int(m[3]), float(m[4])))
            else:
                goals.append((int(m[1]), float(m[2])))
            break
    cost = {}
    for s, r, ok, c in expands:
        cost.setdefault(s, c)
    for s, nd in nodes.items():
        if s not in cost:
            p = nd.get("parent")
            cost[s] = (nd.get("pcost", 0.0) if p is None
                       else cost.get(p, nd.get("pcost", 0.0)) + 1.0)
    return nodes, expands, goals, cost

def feats(nodes, cost):
    """per evaluated node: eng / onehot / hdc features + front rule id"""
    first_rule = {}
    Xe, Xo, Xh, serials = [], [], [], []
    for s, nd in nodes.items():
        ev = nd.get("eval")
        if ev is None:
            continue
        vals = [cost[s], nd.get("chain_len", 0)] + ev  # 11 raw features
        Xe.append(vals)
        # one-hot: head pred of front rule + bins of dp0/chain_len/ncand
        oh = np.zeros(NPRED + 8 + 4 + 8)
        rid = nd.get("front_rule")
        if rid in rule_head:
            oh[p2i[rule_head[rid]]] = 1.0
        oh[NPRED + min(7, int(ev[8]))] = 1.0                    # dp0 bin
        oh[NPRED + 8 + min(3, nd.get("chain_len", 0))] = 1.0    # chain len
        oh[NPRED + 12 + min(7, int(np.log2(max(1, ev[0]))))] = 1.0  # ncand
        Xo.append(oh)
        names = ["cost", "clen", "ncand", "minprice", "relfam", "relevd",
                 "conf", "dcost", "total", "dp0", "wit"]
        bins = [int(min(7, max(0, np.floor(abs(x) * 2)))) for x in vals]
        vb = [v(("f", names[j], bins[j])) for j in range(len(names))]
        hv = np.zeros(D)
        for x in vb:
            hv += x
        for a, b in [(0, 9), (2, 7), (6, 8), (0, 1)]:
            hv += vb[a] * vb[b]
        hv += v(("bias",))
        Xh.append(hv)
        serials.append(s)
    return (np.array(Xe), np.array(Xo), np.array(Xh), serials)

goals_ok, fail_pool = [], []
for log in LOGS:
    nodes, expands, goals, cost = parse_goal(log)
    # front rule per node = rule of its first expand
    for s, r, ok, c in expands:
        if s in nodes and "front_rule" not in nodes[s]:
            nodes[s]["front_rule"] = r
    if not goals:
        Xe, Xo, Xh, sr = feats(nodes, cost)
        if len(sr):
            fail_pool.append((Xe, Xo, Xh))
        continue
    best = min(c for _, c in goals)
    goal_node = next(s for s, c in goals if c == best)
    on_path, cur = set(), goal_node
    while cur is not None and cur in nodes:
        on_path.add(cur)
        cur = nodes[cur].get("parent")
    Xe, Xo, Xh, sr = feats(nodes, cost)
    yreg = np.array([(best - cost[s]) if s in on_path else 3.0
                     for s in sr])
    # non-triviality: expansion index of the goal node's parent (real order)
    pog = nodes[goal_node].get("parent")
    direct_idx = None
    for k, (es, rr, okk, cc) in enumerate(expands):
        if es == pog:
            direct_idx = k + 1
            break
    goals_ok.append(dict(nodes=nodes, expands=expands, cost=cost,
                         goal_node=goal_node, best=best, on_path=on_path,
                         Xe=Xe, Xo=Xo, Xh=Xh, serials=sr, yreg=yreg,
                         direct=direct_idx, n_exp=len(expands)))

print(f"success_goals={len(goals_ok)} fail_runs={len(fail_pool)} "
      f"nodes={sum(len(g['yreg']) for g in goals_ok)} "
      f"fail_nodes={sum(len(f[0]) for f in fail_pool)} "
      f"path_pos={sum(int((g['yreg'] < 3).sum()) for g in goals_ok)}",
      file=sys.stderr)

def ridge(X, y, lam=1.0):
    X1 = np.hstack([X, np.ones((len(X), 1))])
    return np.linalg.solve(X1.T @ X1 + lam * np.eye(X1.shape[1]),
                           X1.T @ y)

def replay(g, pred):
    children = collections.defaultdict(list)
    for s, nd in g["nodes"].items():
        p = nd.get("parent")
        if p is not None:
            children[p].append(s)
    pm = dict(zip(g["serials"], pred))
    default = g["best"] + 1.0
    openh, seen, n = [(0.0, 0)], {0}, 0
    pog = g["nodes"][g["goal_node"]].get("parent")
    while openh:
        _, s = heapq.heappop(openh)
        n += 1
        if s == pog:
            return n
        for c in children[s]:
            if c not in seen:
                seen.add(c)
                heapq.heappush(openh, (g["cost"][c] + pm.get(c, default), c))
    return None

def key_branch_acc(g, pred):
    """fraction of optimal-path branch points where the model ranks the
    on-path child first among its siblings"""
    pm = dict(zip(g["serials"], pred))
    ok = tot = 0
    for s in g["on_path"]:
        children = [c for c, nd in g["nodes"].items()
                    if nd.get("parent") == s]
        on_children = [c for c in children if c in g["on_path"]]
        if len(children) < 2 or not on_children:
            continue
        tot += 1
        best_c = min(children, key=lambda c: pm.get(c, 1e9))
        if best_c in on_children:
            ok += 1
    return ok, tot

names = ["eng", "onehot", "hdc", "eng+hdc"]
sp = {n: [] for n in names}
rep = {n: [] for n in ["orig"] + names}
key = {n: [0, 0] for n in names}
for i, g in enumerate(goals_ok):
    tr = [j for j in range(len(goals_ok)) if j != i]
    Xe_tr = np.vstack([goals_ok[j]["Xe"] for j in tr] +
                      [f[0] for f in fail_pool])
    Xo_tr = np.vstack([goals_ok[j]["Xo"] for j in tr] +
                      [f[1] for f in fail_pool])
    Xh_tr = np.vstack([goals_ok[j]["Xh"] for j in tr] +
                      [f[2] for f in fail_pool])
    y_tr = np.concatenate([goals_ok[j]["yreg"] for j in tr] +
                          [np.full(len(f[0]), 3.0) for f in fail_pool])
    mu, sd = Xe_tr.mean(0), Xe_tr.std(0) + 1e-9
    mods = {
        "eng": (np.hstack([(Xe_tr - mu) / sd, np.ones((len(y_tr), 1))]),
                np.hstack([(g["Xe"] - mu) / sd, np.ones((len(g["Xe"]), 1))])),
        "onehot": (np.hstack([Xo_tr, np.ones((len(y_tr), 1))]),
                   np.hstack([g["Xo"], np.ones((len(g["Xo"]), 1))])),
        "hdc": (np.hstack([Xh_tr, np.ones((len(y_tr), 1))]),
                np.hstack([g["Xh"], np.ones((len(g["Xh"]), 1))])),
        "eng+hdc": (np.hstack([(Xe_tr - mu) / sd, Xh_tr,
                               np.ones((len(y_tr), 1))]),
                    np.hstack([(g["Xe"] - mu) / sd, g["Xh"],
                               np.ones((len(g["Xe"]), 1))])),
    }
    for n in names:
        Xt, Xv = mods[n]
        w = ridge(Xt[:, :-1], y_tr)  # ridge adds bias col itself
        pred = np.hstack([Xv[:, :-1], np.ones((len(Xv), 1))]) @ w
        r = spearmanr(pred, g["yreg"]).statistic
        sp[n].append(r if r == r else np.nan)
        rep[n].append(replay(g, pred))
        ko, kt = key_branch_acc(g, pred)
        key[n][0] += ko
        key[n][1] += kt
    # orig priority replay: priority = cost + minprice -> pass minprice as
    # the remaining-estimate (replay adds cost itself)
    rep["orig"].append(replay(g, np.array(
        [g["nodes"][s].get("eval", [0, 1.0])[1] for s in g["serials"]])))
print("== goals (direct-idx = goal-parent expansion index in real trace) ==")
for i, g in enumerate(goals_ok):
    print(f"  goal{i}: expansions={g['n_exp']} direct={g['direct']} "
          f"best_cost={g['best']:.0f} path_nodes={len(g['on_path'])}")
print("Xh checksum:", float(np.vstack([g["Xh"] for g in goals_ok]).sum()),
      file=sys.stderr)
print("== LOGO Spearman ==")
for n in names:
    print(f"  {n}: mean={np.nanmean(sp[n]):.3f} "
          f"per-goal={[round(x,2) for x in sp[n]]}")
print("== replay expansions-to-optimal (per goal) ==")
for n in ["orig"] + names:
    print(f"  {n}: {rep[n]}  mean={np.nanmean([x for x in rep[n] if x]):.1f}")
print("== key-branch first-rank accuracy (optimal-path branch points) ==")
for n in names:
    print(f"  {n}: {key[n][0]}/{key[n][1]}")
