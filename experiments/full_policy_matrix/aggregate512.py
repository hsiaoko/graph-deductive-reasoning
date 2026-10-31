#!/usr/bin/env python3
"""full_policy_matrix @ rules512 aggregation: per-goal + macro-mean layers.

Macro protocol (pre-registered in RULE_SCALE512.md):
- membership: goals with GT derived answers > 0 (g1,g2,g3,g5 at rules512;
  g4 excluded, still empty).
- per goal: det/hyb = median of 3 timing reps; random family = median over
  3 policy seeds. Then unweighted macro mean across member goals.
- derivation reduction rate per goal = 1 - D_agent,g / D_chase_total,
  D_chase_total = 349,552 (GT closure derived total); chase baseline = 0.
"""
import os, re, glob, statistics as st

W = '.work_hdcv/full_policy_matrix'
GOALS = {
    'g1': ('/award/award_nominee/award_nominations./award/award_nomination/award', '/m/02flpq'),
    'g2': ('/award/award_winner/awards_won./award/award_honor/award_winner', '/m/02r3zy'),
    'g3': ('/people/person/profession', '/m/03lgtv'),
    'g4': ('/people/person/places_lived./people/place_lived/location', '/m/0nvt9'),
    'g5': ('/soccer/football_team/current_roster./soccer/football_roster_position/position', None),
}
# GT-derived>0 at rules512 = {g1,g2,g3,g5}; g4 empty (excluded).
MACRO_GOALS = ['g1', 'g2', 'g3', 'g5']
D_CHASE_TOTAL = 349552

def load_gt():
    gt, base = {t: set() for t in GOALS}, {t: set() for t in GOALS}
    for line in open('.work_hdcv/base_edges.tsv'):
        rel, src, dst = line.rstrip('\n').split('\t')
        for t, (r, d) in GOALS.items():
            if rel == r and (d is None or d == dst):
                base[t].add((src, dst)); gt[t].add((src, dst))
    for line in open(f'{W}/gt_closure512/closure_edges.tsv'):
        if line.startswith('#'): continue
        c = line.rstrip('\n').split('\t')
        for t, (r, d) in GOALS.items():
            if c[1] == r and (d is None or d == c[2]):
                gt[t].add((c[0], c[2]))
    return gt, base

def parse_run(d):
    y = {}
    for line in open(f'{d}/summary.yaml'):
        m = re.match(r'(\w+): (.*)', line.strip())
        if m: y[m.group(1)] = m.group(2)
    log = open(f'{d}/run.log', errors='replace').read()
    wall = re.findall(r'wall=([\d.e+-]+)s', log)
    return y, (float(wall[-1]) if wall else None)

def run_answers(d, rel, dst):
    found = set()
    p = f'{d}/closure_edges.tsv'
    if os.path.exists(p):
        for line in open(p):
            if line.startswith('#'): continue
            c = line.rstrip('\n').split('\t')
            if c[1] == rel and (dst is None or c[2] == dst):
                found.add((c[0], c[2]))
    p = f'{d}/chain.tsv'
    if os.path.exists(p):
        for line in open(p):
            if line.startswith('#'): continue
            fact = line.rstrip('\n').split('\t')[-1]
            if fact.startswith(rel + '('):
                a = fact[len(rel)+1:-1].split(',')
                if dst is None or a[1] == dst:
                    found.add((a[0], a[1]))
    return found

def main():
    gt, base = load_gt()
    rows = {}  # (goal, cfg) -> dict
    for d in sorted(glob.glob(f'{W}/runs512/g*/*')):
        gtg, cfg = d.split('/')[-2], d.split('/')[-1]
        if gtg not in GOALS: continue
        main_d = f'{d}/rep1' if os.path.exists(f'{d}/rep1/summary.yaml') else d
        if not os.path.exists(f'{main_d}/summary.yaml'): continue
        y, wall = parse_run(main_d)
        rel, dst = GOALS[gtg]
        G, B = gt[gtg], base[gtg]
        found = run_answers(main_d, rel, dst) | B
        hit = found & G
        rec = len(hit)/len(G) if G else None
        nb = G - B
        drec = len(hit & nb)/len(nb) if nb else None
        prec = len(hit)/len(found) if found else 1.0
        walls = []
        for rep in sorted(glob.glob(f'{d}/rep*')):
            _, w = parse_run(rep)
            if w is not None: walls.append(w)
        if not walls and wall is not None: walls = [wall]
        rows[(gtg, cfg)] = {
            'termination': y.get('termination'), 'recall': rec, 'drecall': drec,
            'prec': prec, 'derived': int(y.get('derived', 0)),
            'wall_med': st.median(walls) if walls else None,
            'wall_min': min(walls) if walls else None,
            'wall_max': max(walls) if walls else None,
        }
    # per-goal detail file
    with open(f'{W}/detail512.tsv', 'w') as f:
        f.write('goal\tconfig\ttermination\trecall\tderived_recall\tprecision\tderived\twall_med\twall_min\twall_max\n')
        for (gtg, cfg), r in sorted(rows.items()):
            f.write('\t'.join([gtg, cfg, str(r['termination']),
                '' if r['recall'] is None else f'{r["recall"]:.4f}',
                '' if r['drecall'] is None else f'{r["drecall"]:.4f}',
                f'{r["prec"]:.4f}', str(r['derived']),
                f'{r["wall_med"]:.4f}', f'{r["wall_min"]:.4f}', f'{r["wall_max"]:.4f}']) + '\n')
    # ---- macro layer ----
    def method_of(cfg):
        return cfg  # cfg already encodes method+seed grouping below
    # group configs into methods
    methods = {}
    for (gtg, cfg), r in rows.items():
        # cfg: fwd_<scorer>_det | fwd_<scorer>_hyb | fwd_<scorer>_rand_s{7,13,42}
        #        | bwd_score | bwd_rand_s* | bwd_hyb_s*
        m = cfg
        m = re.sub(r'_s(7|13|42)$', '', m)
        methods.setdefault(m, {})[gtg] = methods.get(m, {}).get(gtg, []) + [r]
    def goal_agg(rs):
        # median over seeds (random) or single (det/hyb already rep-median)
        return {
            'recall': st.median([x['recall'] for x in rs]),
            'drecall': st.median([x['drecall'] for x in rs]),
            'prec': st.median([x['prec'] for x in rs]),
            'wall': st.median([x['wall_med'] for x in rs]),
            'derived': st.median([x['derived'] for x in rs]),
        }
    print('== MACRO (unweighted mean over member goals g1,g2,g3,g5; g4 empty, excluded) ==')
    print(f'{"method":18s} {"recall":>7s} {"drecall":>7s} {"prec":>7s} {"wall_s":>9s} {"derived":>9s} {"reduct":>7s}   (per-goal walls: ' + ' '.join(MACRO_GOALS) + ')')
    for m in sorted(methods):
        gvals = []
        for gtg in MACRO_GOALS:
            if gtg not in methods[m]: continue
            gvals.append(goal_agg(methods[m][gtg]))
        if not gvals: continue
        rec = st.mean(v['recall'] for v in gvals)
        drec = st.mean(v['drecall'] for v in gvals)
        prec = st.mean(v['prec'] for v in gvals)
        wall = st.mean(v['wall'] for v in gvals)
        der = st.mean(v['derived'] for v in gvals)
        reduct = st.mean(1 - v['derived']/D_CHASE_TOTAL for v in gvals)
        per_goal_walls = ' '.join(f'{v["wall"]:.3f}' for v in gvals)
        print(f'{m:18s} {rec:7.3f} {drec:7.3f} {prec:7.3f} {wall:9.3f} {der:9.0f} {reduct:7.3f}   ({per_goal_walls})')

if __name__ == '__main__':
    main()
