#!/usr/bin/env python3
# Aggregate exp-1 overall results into the main table.
import sys, glob, re, os
import numpy as np

W = "/home/zhuxk/project/graph/GARAgent/.work_hdcv"
EXP = "experiments/fb15k_overall"

goals = []
for line in open(f"{W}/goals50.tsv"):
    if line.startswith("#"): continue
    goals.append(line.rstrip("\n").split("\t"))
# goal key -> (round, origin)
gmap = {(g[0], g[1], g[2]): (int(g[3]), g[4]) for g in goals}

def parse_verify_report(path):
    rows = {}
    if not os.path.exists(path):
        return rows
    for line in open(path):
        if line.startswith("#") or "\t" not in line: continue
        p = line.rstrip("\n").split("\t")
        if len(p) < 6 or p[1] != "10" and not p[1].isdigit(): pass
        m = re.match(r"(.+)\(([^,]+),([^)]+)\)$", p[0])
        if not m: continue
        rows[(m.group(1), m.group(2), m.group(3))] = dict(
            verdict=p[2], steps=p[3], cost=p[4], wall_ms=float(p[5]))
    return rows

back = parse_verify_report(f"{EXP}/backward.tsv")
greedy = parse_verify_report(f"{EXP}/greedy.tsv")

def parse_bnb(arm):
    import os as _os
    rows = {}
    for f in sorted(glob.glob(f"{W}/bnb_{arm}_*.log")):
        i = int(re.search(r"_(\d+)\.log$", f).group(1))
        g = goals[i]
        key = (g[0], g[1], g[2])
        txt = open(f, errors="replace").read()
        done = re.search(r"\[bnb\] done: expanded=(\d+) goal=(\w+) "
                         r"best_cost=([\d.-]+) certified_optimal=(\d)", txt)
        vm = re.search(r"verify: goal .* -> (\w+)", txt)
        import subprocess as _sp
        try:
            bt = float(_sp.check_output(["stat", "-c", "%W", f]).strip())
        except Exception:
            bt = 0.0
        wall_s = (_os.path.getmtime(f) - bt) if bt > 0 else None
        rows[key] = dict(
            wall_s=wall_s,
            done=bool(done),
            expanded=int(done.group(1)) if done else None,
            goal=(done.group(2) == "true") if done else None,
            best_cost=float(done.group(3)) if done else None,
            certified=int(done.group(4)) if done else None,
            verdict=vm.group(1) if vm else None,
            timeout="Killed" in txt)
    return rows

bu, bh = parse_bnb("utility"), parse_bnb("hdc")

print("goal\tbstar_round\torigin\tback_hit\tback_ms\tgreedy_hit\tgreedy_ms\t"
      "bnbU_hit\tbnbU_exp\tbnbU_s\tbnbH_hit\tbnbH_exp\tbnbH_s")
for g in goals:
    key = (g[0], g[1], g[2])
    b, gr = back.get(key), greedy.get(key)
    u, h = bu.get(key), bh.get(key)
    def cell(r, f): return str(r[f]) if r else "NA"
    print("\t".join([
        f"{key[0]}({key[1]},{key[2]})", str(g[3]), g[4],
        cell(b, "verdict"), str(b["wall_ms"]) if b else "NA",
        cell(gr, "verdict"), str(gr["wall_ms"]) if gr else "NA",
        str(u["goal"]) if u and u["goal"] is not None else "NA",
        str(u["expanded"]) if u and u["expanded"] is not None else "NA",
        str(u["wall_s"]) if u and u["wall_s"] else "NA",
        str(h["goal"]) if h and h["goal"] is not None else "NA",
        str(h["expanded"]) if h and h["expanded"] is not None else "NA",
        str(h["wall_s"]) if h and h["wall_s"] else "NA"]))
