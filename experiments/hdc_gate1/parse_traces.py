#!/usr/bin/env python3
# Parse back_multi trace.log -> subgoal dataset TSV.
# Outcome: in goal's derived journal OR base graph -> 1; explicit exit->0 and
# not in journal/base -> 0; else dropped (cycle/memo/ambiguous early return).
import sys, re, collections

trace = sys.argv[1]           # /tmp/hdc_g1/trace.log
base_edges = sys.argv[2]      # /tmp/hdc_g1/base_edges.tsv
out_path = sys.argv[3]
closure_path = sys.argv[4]    # /tmp/exp/chase_heldout20/closure_edges.tsv

base = set()
with open(base_edges) as f:
    for line in f:
        r, s, d = line.rstrip("\n").split("\t")
        base.add((r, s, d))

# closure membership = derivability ground truth (2-round chase on the
# held-out graph); loaded for all relations (few GB, one pass)
closure = set()
with open(closure_path, errors="replace") as f:
    for line in f:
        if line.startswith("#"):
            continue
        p = line.rstrip("\n").split("\t")
        if len(p) >= 3:
            closure.add((p[1], p[0], p[2]))  # (rel, src, dst)

try_re = re.compile(r"^\[bwd\] try (\S+)\(([^,]+),([^)]+)\) depth=(\d+)")
exit_re = re.compile(r"^\[bwd\] exit (\S+)\(([^,]+),([^)]+)\) -> (\d+)")
goal_re = re.compile(r"^=== goal (\d+) (/\S+) (/\S+) (/\S+)$")
done_re = re.compile(r"^=== goal (\d+) done")
fact_re = re.compile(r"^=== goal (\d+) fact (\S+) (\S+) (\S+)$")

goals = {}
done_goals = set()
cur = None
tries = collections.defaultdict(list)   # goal -> [(rel,src,dst)]
exits0 = collections.defaultdict(set)   # goal -> set of failed targets
facts = collections.defaultdict(set)    # goal -> derived journal
with open(trace, errors="replace") as f:
    for line in f:
        m = goal_re.match(line)
        if m:
            cur = int(m.group(1))
            goals[cur] = (m.group(2), m.group(3), m.group(4))
            continue
        m = done_re.match(line)
        if m:
            done_goals.add(int(m.group(1)))
            cur = None
            continue
        m = fact_re.match(line)
        if m:
            facts[int(m.group(1))].add((m.group(2), m.group(3), m.group(4)))
            continue
        m = try_re.match(line)
        if m and cur is not None:
            rel, s, d = m.group(1), m.group(2), m.group(3)
            if s != "_" and d != "_":
                tries[cur].append((rel, s, d))
            continue
        m = exit_re.match(line)
        if m and cur is not None and m.group(4) == "0":
            exits0[cur].add((m.group(1), m.group(2), m.group(3)))

n1 = n0 = ndrop = 0
nder = 0
with open(out_path, "w") as out:
    out.write("goal\trel\tsrc\tdst\trealized\tderivable\n")
    for g, ts in tries.items():
        if g not in done_goals:
            continue  # in-flight goal at truncation: partial labels
        seen = set()
        for t in ts:
            if t in seen:
                continue
            seen.add(t)
            if t in facts[g] or t in base:
                o = 1
            elif t in exits0[g]:
                o = 0
            else:
                ndrop += 1
                continue
            dv = int(t in base or t in closure)
            nder += dv
            if o:
                n1 += 1
            else:
                n0 += 1
            out.write(f"{g}\t{t[0]}\t{t[1]}\t{t[2]}\t{o}\t{dv}\n")
print(f"subgoals: realized_succ={n1} fail={n0} dropped={ndrop} "
      f"derivable={nder} goals={len(goals)}", file=sys.stderr)
