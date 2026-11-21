#!/usr/bin/env python3
"""De-risking probe 2: do exploratory rollouts find cheaper proofs than Det?

A learned policy can only exceed the handcrafted teacher if training data
contains trajectories better than it. Deterministic traces cap at Det; backward
fails on hard goals. This runs exploratory policies (random / uniform-random)
on hard goals across seeds and checks whether any rollout reaches the goal with
far fewer derivations than Det -- i.e. whether better-than-Det teacher data is
obtainable by exploration.
"""
import argparse, csv, json, statistics, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / ".work_hdcv/exploration_probe"
CSR = "/ssd_data/zhuxk/graphs/KG/fb15k-237/immutable_csr"
RULES = ROOT / ".work_hdcv/fixpoint_rules640/rules640"
TEST_GOALS = ROOT / ".work_hdcv/fixpoint_q640/test_goals.tsv"
# Det derived per goal from the oracle-bound run (forward deterministic).
DET_DERIVED = {"p09": 108411, "p11": 122902, "p21": 575480, "p07": 55697,
               "p34": 356169, "p02": 75082}


def yaml_summary(path):
    values = {}
    for line in path.read_text().splitlines():
        if ":" in line:
            k, v = line.split(":", 1)
            values[k.strip()] = v.strip()
    return values


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--goals", default="p09,p11,p21,p07")
    ap.add_argument("--seeds", default="7,13,42,101,202")
    ap.add_argument("--policies", default="random,uniform-random")
    ap.add_argument("--wall-limit", type=int, default=120)
    args = ap.parse_args()
    WORK.mkdir(parents=True, exist_ok=True)
    specs = {r["goal_id"]: r["spec"]
             for r in csv.DictReader(TEST_GOALS.open(), delimiter="\t")}
    seeds = [int(s) for s in args.seeds.split(",")]
    policies = args.policies.split(",")
    rows = []
    for gid in args.goals.split(","):
        for policy in policies:
            for seed in seeds:
                d = WORK / gid / policy / f"seed{seed}"
                d.mkdir(parents=True, exist_ok=True)
                sp = d / "summary.yaml"
                if not sp.exists():
                    cmd = ["./build/bin/gar_reason", "--mode", "agent",
                           "--engine", "cpu", "--csr", CSR, "--rules", str(RULES),
                           "--goal", specs[gid], "--out", str(d), "--print", "0",
                           "--star-order", "goal", "--scorer", "exact",
                           "--budget", "1000000000",
                           "--wall-limit", str(args.wall_limit),
                           "--max-candidates-per-rule", "100000",
                           "--hdc-dim", "128", "--policy", policy,
                           "--policy-seed", str(seed), "--fair-interval", "0"]
                    subprocess.run(cmd, cwd=ROOT, capture_output=True)
                if not sp.exists():
                    continue
                s = yaml_summary(sp)
                rows.append({"goal": gid, "policy": policy, "seed": seed,
                             "derived": int(s["derived"]),
                             "hit": s["goal_derived"] == "true",
                             "termination": s["termination"]})
    with (WORK / "rows.tsv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]), delimiter="\t",
                                lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    print(f"{'goal':5s} {'policy':15s} {'hit':>4s} {'min':>8s} {'median':>8s} "
          f"{'Det':>8s}  best-vs-Det", flush=True)
    for gid in args.goals.split(","):
        det = DET_DERIVED.get(gid)
        for policy in policies:
            sub = [r for r in rows if r["goal"] == gid and r["policy"] == policy
                   and r["hit"]]
            if not sub:
                print(f"{gid:5s} {policy:15s} {0:>4d} {'—':>8s} {'—':>8s} "
                      f"{str(det):>8s}  no hit", flush=True)
                continue
            ds = [r["derived"] for r in sub]
            mn, md = min(ds), statistics.median(ds)
            ratio = f"{mn/det:.2f}x" if det else "—"
            print(f"{gid:5s} {policy:15s} {len(sub):>4d} {mn:>8d} {md:>8.0f} "
                  f"{str(det):>8s}  {ratio}", flush=True)


if __name__ == "__main__":
    main()