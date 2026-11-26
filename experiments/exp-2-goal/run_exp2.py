#!/usr/bin/env python3
"""exp-2-goal: Safe Relevance Filtering (Proposition 2), BioKG fast pass.

Compares unrestricted chase (all rules) vs relevance-filtered chase (r(g,mu)>0,
i.e. the goal cone) across a goal panel spanning cone sizes. Measures candidate
action reduction and confirms zero derivable-goal loss. Single-thread, no cap,
dedup derived counts (one chase per config) -- avoids per-goal-sum caliber issues.
"""
import csv, json, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CSR = "/ssd_data/zhuxk/graphs/KG/ogbl-biokg/immutable_csr"
RULES = ROOT / ".work_hdcv/overall-performance/ogbl-biokg/rules"
WORK = Path(__file__).resolve().parent / "work_biokg"
BIN = ROOT / "build/bin"


def ys(p):
    d = {}
    for l in p.read_text().splitlines():
        if l and not l[0].isspace() and ":" in l:
            k, v = l.split(":", 1); d[k.strip()] = v.strip()
    return d


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


def cone_sizes():
    r = subprocess.run([str(BIN / "gar_cone_sizes"), "--csr", CSR, "--rules",
                        str(RULES)], cwd=ROOT, capture_output=True, text=True)
    sizes = {}
    for row in csv.DictReader(r.stdout.splitlines(), delimiter="\t"):
        sizes[row["predicate"]] = int(row["cone_size"])
    return sizes


def closure_facts(chase_dir):
    """relation -> list of (src, dst) derived facts."""
    by_pred = {}
    p = chase_dir / "closure_edges.tsv"
    for l in p.open():
        if l.startswith("#"):
            continue
        f = l.rstrip("\n").split("\t")
        if len(f) < 3:
            continue
        by_pred.setdefault(f[1], []).append((f[0], f[2]))
    return by_pred


def goal_in_closure(chase_dir, rel, src, dst):
    for l in (chase_dir / "closure_edges.tsv").open():
        if l.startswith("#"):
            continue
        f = l.rstrip("\n").split("\t")
        if len(f) >= 3 and f[1] == rel and f[0] == src and f[2] == dst:
            return True
    return False


def build_cone_dir(spec, out):
    out.mkdir(parents=True, exist_ok=True)
    gt = out / "goal.tsv"
    gt.write_text(f"spec\n{spec}\n")
    subprocess.run([str(BIN / "gar_cone_union"), "--csr", CSR, "--rules",
                    str(RULES), "--goals-tsv", str(gt), "--out", str(out / "A")],
                   cwd=ROOT, capture_output=True)
    return out / "A"


def main():
    WORK.mkdir(parents=True, exist_ok=True)
    sizes = cone_sizes()
    # Unrestricted full chase (reference candidates + closure).
    full = chase(RULES, WORK / "full")
    e_full = int(full["embeddings_total"])
    dc_full = int(full["derivation_candidates_total"])
    facts = closure_facts(WORK / "full")
    # Select goal predicates spanning cone sizes (exclude cone 0 = base preds).
    derivable = [(p, sizes[p]) for p in facts if sizes.get(p, 0) > 0]
    derivable.sort(key=lambda x: x[1])
    n = len(derivable)
    idxs = sorted({0, n // 4, n // 2, (3 * n) // 4, n - 1})
    panel = [derivable[i] for i in idxs]
    print(f"E_full candidates={e_full}  productive={dc_full}  "
          f"derived={full['derived_total']}", flush=True)
    print(f"panel (predicate, cone_size, irr%):", flush=True)
    results = []
    for pred, csz in panel:
        src, dst = facts[pred][0]
        spec = f"{pred}({src},{dst})"
        irr = 100.0 * (1.0 - csz / 720.0)
        cdir = build_cone_dir(spec, WORK / pred.replace("/", "_"))
        filt = chase(cdir, WORK / pred.replace("/", "_") / "chase_filtered")
        e_g = int(filt["embeddings_total"])
        dc_g = int(filt["derivation_candidates_total"])
        lost = not goal_in_closure(WORK / pred.replace("/", "_") / "chase_filtered",
                                   pred, src, dst)
        red = 100.0 * (1.0 - e_g / e_full)
        red_dc = 100.0 * (1.0 - dc_g / dc_full)
        rec = {"predicate": pred, "cone_size": csz, "irrelevant_pct": irr,
               "spec": spec, "emb_full": e_full, "emb_filtered": e_g,
               "candidate_reduction_pct": red,
               "productive_reduction_pct": red_dc,
               "derived_filtered": int(filt["derived_total"]),
               "goal_lost": lost}
        results.append(rec)
        print(f"  {pred:45s} cone={csz:3d} irr={irr:5.1f}% "
              f"emb={e_g:9d} reduce={red:5.1f}% lost={lost}", flush=True)
    (WORK / "results.json").write_text(json.dumps(
        {"e_full": e_full, "dc_full": dc_full, "panel": results}, indent=2) + "\n")
    print(f"\nresults: {WORK / 'results.json'}", flush=True)


if __name__ == "__main__":
    main()