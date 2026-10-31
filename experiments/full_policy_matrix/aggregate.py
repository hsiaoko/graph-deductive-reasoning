#!/usr/bin/env python3
"""full_policy_matrix aggregation: per-goal tables + overall table."""
import os, re, glob, pickle, statistics as st
from collections import defaultdict

W = '.work_hdcv/full_policy_matrix'
GOALS = {
    'g1': '/award/award_nominee/award_nominations./award/award_nomination/award(_,/m/02flpq)',
    'g2': '/award/award_winner/awards_won./award/award_honor/award_winner(_,/m/02r3zy)',
    'g3': '/people/person/profession(_,/m/03lgtv)',
    'g4': '/people/person/places_lived./people/place_lived/location(_,/m/0nvt9)',
    'g5': '/soccer/football_team/current_roster./soccer/football_roster_position/position(_,_)',
}

# GT answers from the fixpoint closure + base graph
def load_gt():
    gt = {}
    for tag, spec in GOALS.items():
        rel = spec.split('(')[0]
        args = spec[spec.index('(')+1:spec.rindex(')')].split(',')
        dst = args[1] if args[1] != '_' else None
        gt[tag] = (rel, dst, set())
    for line in open('.work_hdcv/base_edges.tsv'):
        rel, src, dst = line.rstrip('\n').split('\t')
        for tag, (r, d, s) in gt.items():
            if rel == r and (d is None or d == dst):
                s.add((src, dst))
    for line in open(f'{W}/gt_closure/closure_edges.tsv'):
        if line.startswith('#'): continue
        c = line.rstrip('\n').split('\t')
        for tag, (r, d, s) in gt.items():
            if c[1] == r and (d is None or d == c[2]):
                s.add((c[0], c[2]))
    return gt

def parse_run(d):
    y = {}
    for line in open(f'{d}/summary.yaml'):
        m = re.match(r'([\w]+): (.*)', line.strip())
        if m: y[m.group(1)] = m.group(2)
    log = open(f'{d}/run.log', errors='replace').read()
    wall = re.findall(r'wall=([\d.e+-]+)s', log)
    return y, (float(wall[-1]) if wall else None)

def run_answers(d, rel, dst):
    """all goal-matching facts the run ended with: base ∪ journal ∪ chain"""
    found = set()
    if os.path.exists(f'{d}/closure_edges.tsv'):
        for line in open(f'{d}/closure_edges.tsv'):
            if line.startswith('#'): continue
            c = line.rstrip('\n').split('\t')
            if c[1] == rel and (dst is None or c[2] == dst):
                found.add((c[0], c[2]))
    if os.path.exists(f'{d}/chain.tsv'):
        for line in open(f'{d}/chain.tsv'):
            if line.startswith('#'): continue
            fact = line.rstrip('\n').split('\t')[-1]
            if fact.startswith(rel + '('):
                a = fact[len(rel)+1:-1].split(',')
                if dst is None or a[1] == dst:
                    found.add((a[0], a[1]))
    return found

def main():
    gt = load_gt()
    # base answers per goal (for derived-only recall)
    base = {}
    for tag, (rel, dst, s) in gt.items():
        b = set()
        for line in open('.work_hdcv/base_edges.tsv'):
            r, src, d = line.rstrip('\n').split('\t')
            if r == rel and (dst is None or d == dst):
                b.add((src, d))
        base[tag] = b

    rows = []
    for d in sorted(glob.glob(f'{W}/runs/g*/*')):
        parts = d.split('/')
        gtg, cfg = parts[-2], parts[-1]
        if gtg not in GOALS: continue
        reps = sorted(glob.glob(f'{d}/rep*/summary.yaml'))
        main_d = f'{d}/rep1' if reps else d
        if not os.path.exists(f'{main_d}/summary.yaml'):
            rows.append((gtg, cfg, 'MISSING', '', '', '', '', '', '', '', '', ''))
            continue
        y, wall = parse_run(main_d)
        rel, dst, G = gt[gtg]
        found = run_answers(main_d, rel, dst) | base[gtg]
        hit = found & G
        recall = len(hit) / len(G) if G else 1.0
        nb = G - base[gtg]
        drecall = len(hit & nb) / len(nb) if nb else 1.0
        # answer-level precision: found goal-answers that are in GT / found
        # (soundness of ALL derived facts vs the closure is checked
        # separately, globally).
        n_der = int(y.get('derived', 0))
        prec = len(hit) / len(found) if found else 1.0
        # timing: median over reps
        walls = []
        for rep in sorted(glob.glob(f'{d}/rep*')):
            _, w = parse_run(rep)
            if w is not None: walls.append(w)
        w_med = st.median(walls) if walls else wall
        w_min, w_max = (min(walls), max(walls)) if walls else ('', '')
        rows.append((gtg, cfg, y.get('termination'), recall, drecall, prec,
                     n_der, y.get('total_cost'), w_med, w_min, w_max,
                     y.get('fwd_capped_enums', ''), ))
    with open(f'{W}/detail.tsv', 'w') as f:
        f.write('goal\tconfig\ttermination\trecall\tderived_recall\tprecision\tderived\tcost\twall_med\twall_min\twall_max\tcapped_enums\n')
        for r in rows:
            f.write('\t'.join(map(str, r)) + '\n')
    # console: per-goal pivot
    cfgs = sorted({r[1] for r in rows})
    for gtg in sorted(GOALS):
        print(f'== {gtg} (GT={len(gt[gtg][2])}, base={len(base[gtg])}) ==')
        for r in rows:
            if r[0] == gtg:
                print(f'  {r[1]:22s} {r[2]:18s} rec={r[3] if r[3]=="" else f"{r[3]:.3f}"} drec={r[4] if r[4]=="" else f"{r[4]:.3f}"} prec={r[5] if r[5]=="" else f"{r[5]:.3f}"} wall={r[8]} [{r[9]},{r[10]}] der={r[6]}')

if __name__ == '__main__':
    main()
