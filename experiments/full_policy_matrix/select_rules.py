#!/usr/bin/env python3
"""Deterministic rule-subset selection (pre-registered in RULE_SUBSET.md).

Reads every rule dir's rule.yaml + csr_edge_labels.bin under RULES_ROOT,
builds the predicate DG (body label -> consequent label), and emits
manifest.tsv + a rules64/ symlink directory.

Usage: select_rules.py <rules_root> <out_dir> [N=64]

N is the loop bound of the pre-registered deterministic algorithm; because
selection order does not depend on N, the first M entries of the N-list are
exactly the N-list of a smaller M (prefix nesting, verified by the caller).
"""
import os, re, struct, sys
from collections import defaultdict

GOALS = [
    '/award/award_nominee/award_nominations./award/award_nomination/award',
    '/award/award_winner/awards_won./award/award_honor/award_winner',
    '/people/person/profession',
    '/people/person/places_lived./people/place_lived/location',
    '/soccer/football_team/current_roster./soccer/football_roster_position/position',
]

def parse_rule(d):
    """Minimal deterministic parse: labels dict, consequent label, quality."""
    y = open(os.path.join(d, 'rule.yaml')).read()
    # labels block: "labels:" then indented "name: id" lines
    labels = {}
    m = re.search(r'labels:\n((?:\s+.+\n)+)', y)
    for lm in re.finditer(r'\s+(\S+):\s*(\d+)', m.group(1)):
        labels[int(lm.group(2))] = lm.group(1)
    cons = re.search(r'consequent:\s*\n(?:\s+\w+:.*\n)*?\s+label:\s*(\S+)', y)
    head = cons.group(1)
    conf = float(re.search(r'confidence:\s*([\d.eE+-]+)', y).group(1))
    supp = int(re.search(r'support:\s*(\d+)', y).group(1))
    # body: pattern edge label ids + antecedent predicate labels
    body = set()
    b = open(os.path.join(d, 'csr_edge_labels.bin'), 'rb').read()
    for lid in struct.unpack(f'{len(b)//4}I', b):
        body.add(labels[lid])
    for pm in re.finditer(r'-\s*\{[^}]*label:\s*(\S+?)\}', y):
        body.add(pm.group(1))
    rid = re.search(r'id:\s*(\S+)', y).group(1)
    return rid, head, body, conf, supp

def main(root, out, n_max=64):
    rules = []
    for d in sorted(os.listdir(root)):
        if d.startswith('gar_'):
            rules.append(parse_rule(os.path.join(root, d)))
    rules.sort(key=lambda r: r[0])  # rule_id asc baseline
    # DG arcs body->head; reachability to each goal
    arcs = defaultdict(set)  # body pred -> set(head pred)
    for rid, head, body, conf, supp in rules:
        for b in body:
            arcs[b].add(head)
    def reaches(goal):
        # preds that can reach goal along body->head arcs (reverse BFS)
        rev = defaultdict(set)
        for b, heads in arcs.items():
            for h in heads:
                rev[h].add(b)
        seen, frontier = {goal}, [goal]
        while frontier:
            p = frontier.pop()
            for q in rev.get(p, ()):
                if q not in seen:
                    seen.add(q)
                    frontier.append(q)
        return seen
    key = lambda r: (-r[3], -r[4], r[0])  # conf desc, support desc, id asc
    S, L, reason = set(), [], {}
    # step 1: per-goal top-4 direct heads (goal order)
    for k, g in enumerate(GOALS, 1):
        heads = sorted([r for r in rules if r[1] == g], key=key)[:4]
        if not heads:
            print(f'NOTE: goal {k} ({g}) has NO direct head rule', file=sys.stderr)
        for r in heads:
            if r[0] not in S:
                S.add(r[0]); L.append(r); reason[r[0]] = f'head-top4-goal{k}'
    # step 2: round-robin over ancestor pools
    pools = []
    for k, g in enumerate(GOALS, 1):
        reach = reaches(g)
        pools.append(sorted([r for r in rules if r[1] in reach and r[0] not in S], key=key))
    idx = [0]*5
    while len(L) < n_max:
        progressed = False
        for k in range(5):
            if len(L) >= n_max: break
            while idx[k] < len(pools[k]) and pools[k][idx[k]][0] in S:
                idx[k] += 1
            if idx[k] < len(pools[k]):
                r = pools[k][idx[k]]; idx[k] += 1
                S.add(r[0]); L.append(r); reason[r[0]] = f'roundrobin-goal{k+1}'
                progressed = True
        if not progressed: break
    os.makedirs(out, exist_ok=True)
    rdir = os.path.join(out, f'rules{n_max}')
    os.makedirs(rdir, exist_ok=True)
    with open(os.path.join(out, f'manifest{n_max}.tsv'), 'w') as f:
        f.write('order\trule_id\thead\tselected_reason\tconfidence\tsupport\n')
        for i, r in enumerate(L):
            f.write(f'{i+1}\t{r[0]}\t{r[1]}\t{reason[r[0]]}\t{r[3]}\t{r[4]}\n')
            link = os.path.join(rdir, r[0])
            if not os.path.exists(link):
                os.symlink(os.path.join(root, r[0]), link)
    print(f'selected {len(L)} rules -> {rdir}; head picks = {sum(1 for r in L if reason[r[0]].startswith("head"))}')
    # head coverage check per goal (64 and 32 prefixes, and N)
    for k, g in enumerate(GOALS, 1):
        n = sum(1 for r in L if r[1] == g)
        n64 = sum(1 for r in L[:64] if r[1] == g)
        n32 = sum(1 for r in L[:32] if r[1] == g)
        print(f'goal{k}: direct-heads in {n_max}={n}, first-64={n64}, first-32={n32}')

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 64)
