#!/usr/bin/env python3
import argparse
import csv
import json
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / ".work_hdcv/fixpoint_policy"
OUT = WORK / "runs_v1"
CSR = "/ssd_data/zhuxk/graphs/KG/fb15k-237/immutable_csr"
RULES512 = ROOT / ".work_hdcv/full_policy_matrix/rules512"
RULES1652 = "/ssd_data/zhuxk/graphs/KG/fb15k-237/rules"
BOUNDED = ROOT / "experiments/fixpoint_policy/run_bounded.py"
BNB_GOALS = {
    "r2c_4": "/music/genre/artists(/m/0fd3y,/m/01vvycq)",
    "r2c_8": "/film/film/runtime./film/film_cut/film_release_region(/m/04v8x9,/m/02w7gg)",
}


def run(name, command):
    directory = OUT / name
    result = directory / "resource.json"
    if result.exists():
        saved = json.loads(result.read_text())
        print(f"skip {name}: {saved['status']}", flush=True)
        return
    directory.mkdir(parents=True, exist_ok=True)
    bounded = ["python3", str(BOUNDED), "--log", str(directory / "run.log"),
               "--result", str(result), "--timeout-sec", "1000",
               "--memory-gb", "256", "--system-memory-gb", "256", "--",
               *map(str, command)]
    print(f"run {name}", flush=True)
    subprocess.run(bounded, cwd=ROOT, check=False)
    saved = json.loads(result.read_text())
    if saved["status"].startswith("oom"):
        raise RuntimeError(f"stop experiment after {name}: {saved['status']}")


def agent_command(goal, output, rules=RULES512):
    return ["./build/bin/gar_reason", "--mode", "agent", "--csr", CSR,
            "--rules", str(rules), "--goal", goal, "--out", str(output),
            "--print", "0"]


def chase():
    for rep in range(1, 4):
        run(f"chase/rep{rep}", [
            "./build/bin/gar_reason", "--mode", "full", "--engine", "cpu",
            "--csr", CSR, "--rules", RULES512, "--out", OUT / f"chase/rep{rep}",
            "--threads", "1", "--print", "0", "--max-rounds", "1000",
            "--max-derivations", "1000000000", "--max-matches", "1000000000",
            "--no-matches"])


def fixpoint(goals, single_pass=False, star_order="input"):
    common = ["--executor", "fixpoint", "--fixpoint-star-order", star_order,
              "--scorer", "exact", "--budget", "1000000000", "--wall-limit",
              "900", "--max-candidates-per-rule", "100000"]
    repetitions = (1,) if single_pass else (1, 2, 3)
    seeds = (7,) if single_pass else (7, 13, 42)
    for row in goals:
        goal = row["spec"]
        if row["split"] == "test":
            for rep in repetitions:
                name = f"fixpoint/{row['goal_id']}/det/rep{rep}"
                run(name, agent_command(goal, OUT / name) + common +
                    ["--policy", "deterministic", "--policy-seed", "42"])
            for seed in seeds:
                name = f"fixpoint/{row['goal_id']}/random/seed{seed}"
                run(name, agent_command(goal, OUT / name) + common +
                    ["--policy", "random", "--policy-seed", str(seed)])
        else:
            name = f"fixpoint/{row['goal_id']}/det/rep1"
            run(name, agent_command(goal, OUT / name) + common +
                ["--policy", "deterministic", "--policy-seed", "42"])


def bnb():
    for goal_id, goal in BNB_GOALS.items():
        for arm in ("utility", "hdc", "numeric"):
            name = f"bnb/{goal_id}/{arm}"
            priority = {"utility": "utility", "hdc": "hdc-value",
                        "numeric": "numeric-value"}[arm]
            command = agent_command(goal, OUT / name, RULES1652) + [
                "--executor", "bnb", "--bnb-priority", priority, "--budget", "2",
                "--max-candidates-per-rule", "64", "--top-k", "0",
                "--policy", "hybrid", "--scorer", "exact"]
            if arm == "hdc":
                command += ["--value-model", str(ROOT / f".work_hdcv/models/{goal_id}.meta")]
            elif arm == "numeric":
                command += ["--value-model", str(ROOT / f".work_hdcv/hdc_hard_goals/models/{goal_id}.meta")]
            run(name, command)


def main():
    global OUT
    parser = argparse.ArgumentParser()
    parser.add_argument("--phase", choices=("chase", "fixpoint", "bnb", "all"),
                        default="all")
    parser.add_argument("--single-pass", action="store_true",
                        help="Run only the first not-yet-run repetition of each configuration.")
    parser.add_argument("--output-root", type=Path, default=OUT)
    parser.add_argument("--fixpoint-star-order", choices=("input", "goal"),
                        default="input")
    args = parser.parse_args()
    OUT = args.output_root.resolve()
    with (WORK / "goals.tsv").open() as stream:
        goals = list(csv.DictReader(stream, delimiter="\t"))
    if args.phase in ("chase", "all"):
        chase()
    if args.phase in ("fixpoint", "all"):
        fixpoint(goals, single_pass=args.single_pass,
                 star_order=args.fixpoint_star_order)
    if args.phase in ("bnb", "all"):
        bnb()


if __name__ == "__main__":
    main()
