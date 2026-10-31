#!/usr/bin/env python3
"""full_policy_matrix aggregation on rules64: per-goal + overall tables."""
import os, re, glob, statistics as st
from collections import defaultdict

W = '.work_hdcv/full_policy_matrix'
GOALS = {
    'g1': ('/award/award_nominee/award_nominations./award/award_nomination/award', '/m/02flpq'),
    'g2': ('/award/award_winner/awards_won./award/award_honor/award_winner', '/m/02r3zy'),
    'g3': ('/people/person/profession', '/m/03lgtv'),
    'g4': ('/people/person/places_lived./people/place_lived/location', '/m/0nvt9'),
    'g5': ('/soccer/football_team/current_roster./soccer/football_roster_position/position', None),
}

def load_gt():
    gt, base = {t: set() for t in GOALS}, {t: set() for t in GOALS}
    for line in open('.work_hdcv/base_edges.tsv'):
        rel, src, dst = line.rstrip('\n').split('\t')
        for t, (r, d) in GOALS.items():
            if rel == r and (d is None or d == dst):
                base[t].add((src, dst)); gt[t].add((src, dst))
    for line in open(f'{W}/gt_closure256/closure_edges.tsv'):
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
    for fn in ('closure_edges.tsv',):
        p = f'{d}/{fn}'
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
    rows = []
    for d in sorted(glob.glob(f'{W}/runs256/g*/*')):
        gtg, cfg = d.split('/')[-2], d.split('/')[-1]
        if gtg not in GOALS: continue
        main_d = f'{d}/rep1'
        if not os.path.exists(f'{main_d}/summary.yaml'):
            main_d = d
        if not os.path.exists(f'{main_d}/summary.yaml'):
            rows.append((gtg, cfg, 'MISSING')) ; continue
        y, wall = parse_run(main_d)
        rel, dst = GOALS[gtg]
        G, B = gt[gtg], base[gtg]
        found = run_answers(main_d, rel, dst) | B
        hit = found & G
        recall = len(hit)/len(G) if G else None       # None = empty GT (n/a)
        nb = G - B
        drecall = len(hit & nb)/len(nb) if nb else None  # None = degenerate
        prec = len(hit)/len(found) if found else 1.0
        walls = []
        for rep in sorted(glob.glob(f'{d}/rep*')):
            _, w = parse_run(rep)
            if w is not None: walls.append(w)
        if not walls and wall is not None: walls = [wall]
        w_med = st.median(walls) if walls else None
        rows.append((gtg, cfg, y.get('termination'), recall, drecall, prec,
                     int(y.get('derived', 0)), y.get('total_cost'),
                     w_med,
                     min(walls) if walls else None,
                     max(walls) if walls else None,
                     y.get('fwd_capped_enums', ''),
                     y.get('max_subgoal_depth', '')))
    with open(f'{W}/detail256.tsv', 'w') as f:
        f.write('goal\tconfig\ttermination\trecall\tderived_recall\tprecision\tderived\tcost\twall_med\twall_min\twall_max\tcapped_enums\tsubgoal_depth\n')
        for r in rows:
            f.write('\t'.join('' if v is None else (f'{v:.4f}' if isinstance(v, float) else str(v)) for v in r) + '\n')
    # console pivots
    for gtg in sorted(GOALS):
        gr = [r for r in rows if r[0] == gtg]
        print(f'== {gtg} GT={len(gt[gtg])} base={len(base[gtg])} ==')
        for r in gr:
            rec = 'n/a' if r[3] is None else f'{r[3]:.3f}'
            drec = 'n/a' if r[4] is None else f'{r[4]:.3f}'
            print(f'  {r[1]:22s} {r[2]:18s} rec={rec:>6s} drec={drec:>6s} prec={r[5]:.3f} wall={r[8]:.3f}s [{r[9]:.3f},{r[10]:.3f}] der={r[6]}')

if __name__ == '__main__':
    main()
