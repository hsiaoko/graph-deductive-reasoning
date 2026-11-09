#!/usr/bin/env python3
"""Generate the hard-goal candidate pool and the two frozen queues.

Reproduces (byte-comparable content):
  .work_hdcv/hdc_hard_goals/candidates_pool.tsv            (desc order, first pass)
  .work_hdcv/hdc_hard_goals/candidates_feasible_queue.tsv  (asc order, second pass)

Frozen algorithm (experiments/hdc_hard_goals/NOTES_HARD_GENERALIZATION.md +
NOTES_HARD_FEASIBLE.md): from the 1,652-rule 30M chase closure, take round==2
facts (structural proof_cost=2 guarantee), keep facts whose generating rule
has >= 1000 embeddings (per-rule stats from the closure's summary.yaml) and
whose head predicate is the consequent of >= 5 distinct rules; exclude every
spec/predicate appearing in any old training/test trace (56 logs), the
tasks50 workload, and the anchor/easy cohort. Desc order = (rule embeddings
desc, rule_id, rel, src, dst); asc order = the same ascending.

Usage: make_hard_candidates.py  (paths fixed below)
"""
import os, re, glob

W = '/home/zhuxk/project/graph/GARAgent/.work_hdcv'
OUT = f'{W}/hdc_hard_goals'
CLOSURE = f'{W}/of_chase30m/closure_edges.tsv'
SUMMARY = f'{W}/of_chase30m/summary.yaml'
RULES = '/ssd_data/zhuxk/graphs/KG/fb15k-237/rules'

def main():
    # per-rule embedding counts (closure summary.yaml)
    emb = {}
    cur = None
    for line in open(SUMMARY):
        m = re.match(r'\s+- id: (\S+)', line)
        if m:
            cur = m.group(1)
            continue
        m = re.match(r'\s+embeddings: (\d+)', line)
        if m and cur:
            emb[cur] = int(m.group(1))
    # head-predicate rule counts (consequent label from rule.yaml)
    head_rules = {}
    for d in sorted(os.listdir(RULES)):
        if not d.startswith('gar_'):
            continue
        y = open(f'{RULES}/{d}/rule.yaml').read()
        m = re.search(r'consequent:\s*\n(?:\s+\w+:.*\n)*?\s+label:\s*(\S+)', y)
        if m:
            head_rules.setdefault(m.group(1), set()).add(d)
    # exclusion sets: every old trace goal spec + their predicates +
    # tasks50 + anchor and easy cohorts
    specs = set()
    for log in glob.glob(f'{W}/g*.log') + glob.glob(f'{W}/r2c_*.log'):
        for line in open(log, errors='replace'):
            m = re.search(r'\[gar_reason\] goal: (\S+)', line)
            if m:
                specs.add(m.group(1))
                break
    with open(f'{OUT}/trace_goal_specs.txt', 'w') as f:
        for s in sorted(specs):
            f.write(s + '\n')
    excl_specs = specs
    trace_preds = set(s.split('(')[0] for s in excl_specs)
    for line in open(f'{W}/tasks50.tsv'):
        if line.startswith('#'):
            continue
        c = line.rstrip('\n').split('\t')
        excl_specs.add(f'{c[0]}({c[1]},{c[2]})')
    for s in ('/location/hud_county_place/county(/m/0r540,/m/0l2rj)',
              '/music/genre/artists(/m/0fd3y,/m/01vvycq)',
              '/film/film/runtime./film/film_cut/film_release_region(/m/04v8x9,/m/02w7gg)',
              '/olympics/olympic_participating_country/athletes./olympics/olympic_athlete_affiliation/olympics(/m/05r4w,/m/09n48)',
              '/sports/sports_position/players./sports/sports_team_roster/position(/m/02g_6x,/m/02vkdwz)',
              '/tv/tv_program/languages(/m/0584r4,/m/02h40lc)',
              '/tv/tv_writer/tv_programs./tv/tv_program_writer_relationship/tv_program(/m/057d89,/m/02_1q9)'):
        excl_specs.add(s)
    cands = []
    for line in open(CLOSURE):
        if line.startswith('#'):
            continue
        c = line.rstrip('\n').split('\t')
        if c[3] != '2':
            continue
        src, rel, dst, rule = c[0], c[1], c[2], c[4]
        if f'{rel}({src},{dst})' in excl_specs or rel in trace_preds:
            continue
        if emb.get(rule, 0) < 1000:
            continue
        if len(head_rules.get(rel, ())) < 5:
            continue
        cands.append((emb[rule], rule, rel, src, dst))
    desc = sorted(cands, key=lambda t: (-t[0], t[1], t[2], t[3], t[4]))
    with open(f'{OUT}/candidates_pool.tsv', 'w') as f:
        f.write('# order\trule_id\trule_embeddings\tgoal_spec\thead_n_rules\n')
        for i, (e, rule, rel, src, dst) in enumerate(desc):
            f.write(f'{i+1}\t{rule}\t{e}\t{rel}({src},{dst})\t'
                    f'{len(head_rules[rel])}\n')
    asc = sorted(cands, key=lambda t: (t[0], t[1], t[2], t[3], t[4]))
    with open(f'{OUT}/candidates_feasible_queue.tsv', 'w') as f:
        f.write('# order\trule_id\trule_embeddings\tgoal_spec\n')
        for i, (e, rule, rel, src, dst) in enumerate(asc):
            f.write(f'{i+1}\t{rule}\t{e}\t{rel}({src},{dst})\n')
    print(f'candidates: {len(cands)}; wrote pool (desc) + feasible queue (asc)')

if __name__ == '__main__':
    main()
