#!/usr/bin/env python3
# Offline value-function study over GAR_DEBUG_BNB logs.
#   parse logs -> branch tree -> labels (on-optimal-path, cost-to-go)
#   -> ridge on engineered vs HDC random features (leave-one-goal-out)
#   -> replay best-first on the explored tree, expansions-to-optimal.
import sys, re, glob, collections
import numpy as np

LOGS = sorted(glob.glob("/home/zhuxk/project/graph/GARAgent/.work_hdcv/g*.log"))
D = 4096
rng = np.random.default_rng(7)

fork_re = re.compile(r"\[bnbl\] fork (\d+) (\d+) ([\d.]+) ([\d.eE+-]+) (\d+)")
eval_re = re.compile(
    r"\[bnbl\] eval (\d+) (\d+) ([\d.eE+-]+) ([\d.eE+-]+) ([\d.eE+-]+) "
    r"([\d.eE+-]+) ([\d.eE+-]+) ([\d.eE+-]+) (\d+) (\d+)")
expand_re = re.compile(r"\[bnbl\] expand (\d+) (\d+) (\d) ([\d.]+)")
goal_re = re.compile(r"\[bnbl\] goal (\d+) ([\d.]+)")
done_re = re.compile(r"\[bnb\] done:.*best_cost=([\d.-]+)")

def parse_goal(log):
    nodes = {}  # serial -> dict
    expands = []  # (serial, rule, ok, cost) in order
    goals = []    # (serial, cost) in order
    for line in open(log, errors="replace"):
        m = fork_re.search(line)
        if m:
            s, p, c, u, cl = int(m[1]), int(m[2]), float(m[3]), float(m[4]), int(m[5])
            nodes[s] = dict(parent=p, pcost=c, chain_len=cl)
            continue
        m = eval_re.search(line)
        if m:
            s = int(m[1])
            nodes.setdefault(s, dict(parent=None))
            nodes[s]["eval"] = [float(x) for x in m.groups()[1:]]
            continue
        m = expand_re.search(line)
        if m:
            expands.append((int(m[1]), int(m[2]), int(m[3]), float(m[4])))
            continue
        m = goal_re.search(line)
        if m:
            goals.append((int(m[1]), float(m[2])))
            continue
    cost = {}
    for s, r, ok, c in expands:
        cost.setdefault(s, c)
    for s, nd in nodes.items():
        if s not in cost:
            p = nd.get("parent")
            cost[s] = (nd.get("pcost", 0.0) if p is None
                       else cost.get(p, nd.get("pcost", 0.0)) + 1.0)
    return nodes, expands, goals, cost

def v(key):
    r = np.random.default_rng(abs(hash(("hv", key))) % (1 << 32))
    return r.choice([-1.0, 1.0], size=D)

goals_data = []
fail_runs = []  # failed goals: nodes all off-path (training data only)
for log in LOGS:
    nodes, expands, goals, cost = parse_goal(log)
    if not goals:
        # keep evaluated nodes as off-path training instances
        Xf, Xh = [], []
        for s, nd in nodes.items():
            if "eval" not in nd:
                continue
            ev = nd["eval"]
            eng = [cost[s], nd.get("chain_len", 0)] + ev
            Xf.append(eng)
            names = ["ncand", "minprice", "relfam", "relevd", "conf", "dcost",
                     "total", "dp0", "wit"]
            vals = [cost[s], nd.get("chain_len", 0)] + ev
            names = ["cost", "clen"] + names
            bins = [int(min(7, max(0, np.floor(abs(vv) * 2)))) for vv in vals]
            vbins = [v(("f", names[j], bins[j])) for j in range(len(names))]
            hv = np.zeros(D)
            for x in vbins:
                hv += x
            for a, b in [(0, 9), (2, 7), (6, 8), (0, 1)]:
                hv += vbins[a] * vbins[b]
            hv += v(("bias",))
            Xh.append(hv)
        if Xf:
            fail_runs.append((np.array(Xf), np.array(Xh)))
        continue
    best = min(c for _, c in goals)
    goal_node = next(s for s, c in goals if c == best)
    # optimal path = ancestors of goal_node
    on_path = set()
    cur = goal_node
    while cur is not None and cur in nodes:
        on_path.add(cur)
        cur = nodes[cur].get("parent")
    # features per node (last eval before use; skip never-evaluated)
    X_eng, X_hdc, yreg, ycls, serials = [], [], [], [], []
    for s, nd in nodes.items():
        if "eval" not in nd:
            continue
        ev = nd["eval"]  # ncand minprice relfam relevd conf dcost total dp0 wit
        eng = [cost[s], nd.get("chain_len", 0)] + ev
        # HDC features: bind conjunctions over discretized eval features
        names = ["ncand", "minprice", "relfam", "relevd", "conf", "dcost",
                 "total", "dp0", "wit"]
        vals = [cost[s], nd.get("chain_len", 0)] + ev
        names = ["cost", "clen"] + names
        bins = []
        for j, val in enumerate(vals):
            b = int(min(7, max(0, np.floor(abs(val) * 2))))
            bins.append(b)
        hv = np.zeros(D)
        vbins = [v(("f", names[j], bins[j])) for j in range(len(names))]
        for x in vbins:
            hv += x
        # a few pairwise conjunctions (bind = elementwise product)
        for a, b in [(0, 9), (2, 7), (6, 8), (0, 1)]:
            hv += vbins[a] * vbins[b]
        hv += v(("bias",))
        X_hdc.append(hv)
        X_eng.append(eng)
        on = s in on_path
        ycls.append(1 if on else 0)
        yreg.append((best - cost[s]) if on else 3.0)  # off-path: worst
        serials.append(s)
    goals_data.append(dict(X_eng=np.array(X_eng), X_hdc=np.array(X_hdc),
                           yreg=np.array(yreg), ycls=np.array(ycls),
                           serials=serials, nodes=nodes, expands=expands,
                           goal_node=goal_node, best=best, on_path=on_path,
                           cost=cost))

print(f"goals_with_solution={len(goals_data)} "
      f"fail_runs={len(fail_runs)} "
      f"nodes={sum(len(g['yreg']) for g in goals_data)} "
      f"fail_nodes={sum(len(f[0]) for f in fail_runs)} "
      f"pos={sum(int(g['ycls'].sum()) for g in goals_data)}", file=sys.stderr)

def ridge_fit(X, y, lam=1.0):
    X1 = np.hstack([X, np.ones((len(X), 1))])
    A = X1.T @ X1 + lam * np.eye(X1.shape[1])
    return np.linalg.solve(A, X1.T @ y)

def replay_order(g, score):
    # best-first on the explored tree: open = {root}; pop min
    # (cost + predicted_remaining); children revealed from fork events.
    children = collections.defaultdict(list)
    for s, nd in g["nodes"].items():
        p = nd.get("parent")
        if p is not None:
            children[p].append(s)
    # predicted remaining per serial (nodes without eval: default large)
    pred = dict(zip(g["serials"], score))
    default = g["best"] + 1.0
    import heapq
    openh = [(0.0, 0)]  # (priority, serial) root serial 0
    seen = {0}
    parent_of_goal = g["nodes"][g["goal_node"]].get("parent")
    n_exp = 0
    while openh:
        _, s = heapq.heappop(openh)
        n_exp += 1
        if s == parent_of_goal:
            return n_exp
        for c in children[s]:
            if c in seen:
                continue
            seen.add(c)
            pr = pred.get(c, default)
            # replay priority mirrors cost + remaining
            heapq.heappush(openh, (pr, c))
    return None  # goal parent never reached (should not happen)

# leave-one-goal-out
from scipy.stats import spearmanr
sp_eng, sp_hdc, sp_both, sp_util = [], [], [], []
rep_orig, rep_eng, rep_hdc, rep_both, rep_direct = [], [], [], [], []
for i, g in enumerate(goals_data):
    tr = [j for j in range(len(goals_data)) if j != i]
    Xe_tr = np.vstack([goals_data[j]["X_eng"] for j in tr] +
                      [f[0] for f in fail_runs])
    Xh_tr = np.vstack([goals_data[j]["X_hdc"] for j in tr] +
                      [f[1] for f in fail_runs])
    y_tr = np.concatenate([goals_data[j]["yreg"] for j in tr] +
                          [np.full(len(f[0]), 3.0) for f in fail_runs])
    # standardize engineered
    mu, sd = Xe_tr.mean(0), Xe_tr.std(0) + 1e-9
    we = ridge_fit((Xe_tr - mu) / sd, y_tr)
    wh = ridge_fit(Xh_tr, y_tr)
    wb = ridge_fit(np.hstack([(Xe_tr - mu) / sd, Xh_tr]), y_tr)
    Xe_te = (g["X_eng"] - mu) / sd
    pe = np.hstack([Xe_te, np.ones((len(Xe_te), 1))]) @ we
    ph = np.hstack([g["X_hdc"], np.ones((len(g["X_hdc"]), 1))]) @ wh
    pb = np.hstack([np.hstack([Xe_te, g["X_hdc"]]),
                    np.ones((len(Xe_te), 1))]) @ wb
    sp_eng.append(spearmanr(pe, g["yreg"]).statistic)
    sp_hdc.append(spearmanr(ph, g["yreg"]).statistic)
    sp_both.append(spearmanr(pb, g["yreg"]).statistic)
    # baseline: the bnb priority itself (cost + minprice) as the predictor
    ev_arr = g["X_eng"]  # [cost, clen, ncand, minprice, ...]
    util = ev_arr[:, 0] + ev_arr[:, 3]
    sp_util.append(spearmanr(util, g["yreg"]).statistic)
    # replay: remaining prediction -> priority = cost + pred_remaining
    cost_map = g["cost"]
    def replay_with(predmap):
        children = collections.defaultdict(list)
        for s, nd in g["nodes"].items():
            p = nd.get("parent")
            if p is not None:
                children[p].append(s)
        import heapq
        default = g["best"] + 1.0
        openh = [(0.0, 0)]
        seen = {0}
        parent_of_goal = g["nodes"][g["goal_node"]].get("parent")
        n = 0
        while openh:
            _, s = heapq.heappop(openh)
            n += 1
            if s == parent_of_goal:
                return n
            for c in children[s]:
                if c in seen:
                    continue
                seen.add(c)
                heapq.heappush(openh, (cost_map[c] + predmap.get(c, default), c))
        return None
    pe_map = dict(zip(g["serials"], pe))
    ph_map = dict(zip(g["serials"], ph))
    pb_map = dict(zip(g["serials"], pb))
    # original priority = cost + minprice; minprice from eval (ev[1])
    # ev = [ncand, minprice, relfam, relevd, conf, dcost, total, dp0, wit]
    orig_map = {}
    for s, nd in g["nodes"].items():
        ev = nd.get("eval")
        orig_map[s] = ev[1] if ev else 1.0  # minprice
    rep_orig.append(replay_with(orig_map))
    # direct: expansion index of the goal parent in the REAL trace order
    pog = g["nodes"][g["goal_node"]].get("parent")
    seen_serials = set()
    direct = None
    for k, (sr, rr, ok, cc) in enumerate(g["expands"]):
        seen_serials.add(sr)
        if sr == pog:
            direct = k + 1
            break
    rep_direct.append(direct)
    rep_eng.append(replay_with(pe_map))
    rep_hdc.append(replay_with(ph_map))
    rep_both.append(replay_with(pb_map))

print("spearman util/eng/hdc/both:",
      f"{np.mean(sp_util):.3f} {np.mean(sp_eng):.3f} {np.mean(sp_hdc):.3f} "
      f"{np.mean(sp_both):.3f}")
print("  per-goal util:", [round(x, 2) for x in sp_util])
print("  per-goal eng :", [round(x, 2) for x in sp_eng])
print("  per-goal hdc :", [round(x, 2) for x in sp_hdc])
print("  per-goal both:", [round(x, 2) for x in sp_both])
print("replay expansions-to-optimal (mean over goals):")
print(f"  direct-trace={np.mean([x for x in rep_direct if x]):.1f} "
      f"orig-replay={np.mean(rep_orig):.1f} eng={np.mean(rep_eng):.1f} "
      f"hdc={np.mean(rep_hdc):.1f} both={np.mean(rep_both):.1f}")
print(f"  direct list: {rep_direct}")
for name, arr in [("orig", rep_orig), ("eng", rep_eng), ("hdc", rep_hdc),
                  ("both", rep_both)]:
    print(f"  {name}: {arr}")


# ---- secondary: branch-level split (leakage caveat: nodes of one goal in
# both folds; allowed by the pre-registered criteria with limitation noted)
print("== branch-level 5-fold (within-goal split; leakage caveat) ==")
from scipy.stats import spearmanr as _sp
Xe_all = np.vstack([g["X_eng"] for g in goals_data] + [f[0] for f in fail_runs])
Xh_all = np.vstack([g["X_hdc"] for g in goals_data] + [f[1] for f in fail_runs])
y_all = np.concatenate([g["yreg"] for g in goals_data] +
                       [np.full(len(f[0]), 3.0) for f in fail_runs])
goal_lab = np.concatenate([[k] * len(g["yreg"]) for k, g in
                           enumerate(goals_data)] +
                          [[-1] * len(f[0]) for f in fail_runs])
mu, sd = Xe_all.mean(0), Xe_all.std(0) + 1e-9
kf = np.random.default_rng(3).permutation(len(y_all)) % 5
for name, X in [("eng", (Xe_all - mu) / sd), ("hdc", Xh_all),
                ("both", np.hstack([(Xe_all - mu) / sd, Xh_all]))]:
    sps = []
    for f in range(5):
        tr, te = kf != f, kf == f
        w = ridge_fit(X[tr], y_all[tr])
        pred = np.hstack([X[te], np.ones((te.sum(), 1))]) @ w
        # evaluate spearman only on successful-goal nodes
        m = goal_lab[te] >= 0
        if m.sum() > 5 and np.std(pred[m]) > 0 and np.std(y_all[te][m]) > 0:
            r = _sp(pred[m], y_all[te][m]).statistic
            if r == r:
                sps.append(r)
    print(f"  {name}: spearman={np.mean(sps) if sps else float('nan'):.3f} "
          f"folds={len(sps)}/5")
