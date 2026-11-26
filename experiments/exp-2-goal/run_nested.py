#!/usr/bin/env python3
"""Nested rule-set scaling: Chase closure (upper bound) vs AdaDeduce derived
(dedup union, middle) vs goal count (lower bound).

Random nested rule sets R_10 ⊂ R_50 ⊂ R_100 ⊂ R_200 ⊂ R_400 (prefixes of one
shuffled ordering). Per R_k: chase closure |C_k|; sample 50 derived goals;
AdaDeduce(hybrid) answers them; |A_k| = dedup union of derived facts. All counts
are dedup (one chase / union over goals), directly comparable. BioKG.
"""
import csv, json, random, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CSR = "/ssd_data/zhuxk/graphs/KG/ogbl-biokg/immutable_csr"
RULES = ROOT / ".work_hdcv/overall-performance/ogbl-biokg/rules"
WORK = Path(__file__).resolve().parent / "work_nested"
BIN = ROOT / "build/bin"
SIZES = [10, 50, 100, 200, 400]
NGOALS = 50
SEED = 20261125


def ys(p):
    d = {}
    for l in p.read_text().splitlines():
        if l and not l[0].isspace() and ":" in l:
            k, v = l.split(":", 1); d[k.strip()] = v.strip()
    return d


def rule_dirs():
    dirs = [e for e in RULES.iterdir()
            if e.is_dir() and (e / "rule.yaml").exists()]
    dirs.sort()
    return dirs


def symlink_dir(dirs, out):
    out.mkdir(parents=True, exist_ok=True)
    for d in dirs:
        dst = out / d.name
        if not dst.exists():
            dst.symlink_to(d.resolve())
    return out


def chase(rules_dir, out):
    out.mkdir(parents=True, exist_ok=True)
    sp = out / "summary.yaml"
    if not sp.exists():
        subprocess.run([str(BIN / "gar_reason"), "--mode", "chase", "--threads",
                        "1", "--engine", "cpu", "--csr", CSR, "--rules",
                        str(rules_dir), "--out", str(out), "--print", "0",
                        "--budget", "1000000000", "--max-candidates-per-rule",
                        "10000000"], cwd=ROOT, capture_output=True)
    return ys(sp) if sp.exists() else None


def closure_rows(chase_dir):
    rows = []
    for l in (chase_dir / "closure_edges.tsv").open():
        if l.startswith("#"):
            continue
        f = l.rstrip("\n").split("\t")
        if len(f) >= 3:
            rows.append((f[0], f[1], f[2]))
    return rows


def agent_goal(rules_dir, spec, out, direction):
    out.mkdir(parents=True, exist_ok=True)
    sp = out / "summary.yaml"
    if not sp.exists():
        subprocess.run([str(BIN / "gar_reason"), "--mode", "agent",
                        "--search-direction", direction, "--policy",
                        "deterministic", "--star-order", "goal", "--scorer",
                        "exact", "--engine", "cpu", "--csr", CSR, "--rules",
                        str(rules_dir), "--goal", spec, "--out", str(out),
                        "--print", "0", "--budget", "1000000000",
                        "--wall-limit", "300", "--max-candidates-per-rule",
                        "10000000", "--hdc-dim", "128", "--policy-seed", "7",
                        "--fair-interval", "0"], cwd=ROOT, capture_output=True)
    return ys(sp) if sp.exists() else None


def dep_goal(rules_dir, spec, out):
    out.mkdir(parents=True, exist_ok=True)
    sp = out / "summary.yaml"
    if not sp.exists():
        subprocess.run([str(BIN / "gar_reason"), "--mode", "dep-only",
                        "--engine", "cpu", "--csr", CSR, "--rules",
                        str(rules_dir), "--goal", spec, "--out", str(out),
                        "--print", "0", "--budget", "1000000000",
                        "--wall-limit", "300", "--max-candidates-per-rule",
                        "10000000"], cwd=ROOT, capture_output=True)
    return ys(sp) if sp.exists() else None


def main():
    WORK.mkdir(parents=True, exist_ok=True)
    dirs = rule_dirs()
    rng = random.Random(SEED)
    order = dirs[:]
    rng.shuffle(order)
    # Build nested rule dirs + chase closures (cached).
    rdirs, csize = {}, {}
    for k in SIZES:
        rdirs[k] = symlink_dir(order[:k], WORK / f"rules_{k}")
        chase(rdirs[k], WORK / f"chase_{k}")
        csize[k] = len(set(closure_rows(WORK / f"chase_{k}")))
    # FIXED goal set: sample once from the smallest closure (R_10). Because the
    # rule sets are nested, these goals are derivable under every larger R_k too.
    # Holding goals constant isolates rule-count scaling from goal-sample noise.
    pool = list(set(closure_rows(WORK / f"chase_{SIZES[0]}")))
    random.Random(SEED).shuffle(pool)
    goals = pool[:NGOALS]
    (WORK / "fixed_goals.tsv").write_text(
        "src\trelation\tdst\n" +
        "".join(f"{s}\t{r}\t{d}\n" for s, r, d in goals))
    print(f"fixed goals: {len(goals)} (sampled from R_{SIZES[0]} closure)",
          flush=True)
    results = []
    for k in SIZES:
        union, det_union, dep_union = set(), set(), set()
        per_sum = hits = det_hits = dep_hits = 0
        for i, (src, rel, dst) in enumerate(goals):
            spec = f"{rel}({src},{dst})"
            gdir = WORK / f"adaF_{k}" / f"g{i:03d}"
            gs = agent_goal(rdirs[k], spec, gdir, "hybrid")
            if gs and gs.get("goal_derived") == "true":
                hits += 1
            gfacts = closure_rows(gdir) if (gdir / "closure_edges.tsv").exists() else []
            union.update(gfacts)
            per_sum += len(gfacts)
            ddir = WORK / f"detF_{k}" / f"g{i:03d}"
            ds = agent_goal(rdirs[k], spec, ddir, "forward")
            if ds and ds.get("goal_derived") == "true":
                det_hits += 1
            dfacts = closure_rows(ddir) if (ddir / "closure_edges.tsv").exists() else []
            det_union.update(dfacts)
            pdir = WORK / f"depF_{k}" / f"g{i:03d}"
            ps = dep_goal(rdirs[k], spec, pdir)
            if ps and ps.get("goal_hit") == "true":
                dep_hits += 1
            pfacts = closure_rows(pdir) if (pdir / "closure_edges.tsv").exists() else []
            dep_union.update(pfacts)
        rec = {"rules": k, "chase_closure": csize[k], "goals": len(goals),
               "ada_dedup_union": len(union), "ada_per_goal_sum": per_sum,
               "ada_hits": hits, "det_dedup_union": len(det_union),
               "det_hits": det_hits, "dep_dedup_union": len(dep_union),
               "dep_hits": dep_hits}
        results.append(rec)
        print(f"R_{k:3d}: chase={csize[k]:8d}  det={len(det_union):8d}  "
              f"dep={len(dep_union):8d}  ada={len(union):7d}  goals={len(goals):3d}  "
              f"hit ada={hits} det={det_hits} dep={dep_hits}", flush=True)
    (WORK / "results_fixed.json").write_text(json.dumps(results, indent=2) + "\n")
    print(f"\nresults: {WORK / 'results_fixed.json'}", flush=True)


if __name__ == "__main__":
    main()