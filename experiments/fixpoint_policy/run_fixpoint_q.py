#!/usr/bin/env python3
import argparse
import csv
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / ".work_hdcv/fixpoint_q640"
CSR = "/ssd_data/zhuxk/graphs/KG/fb15k-237/immutable_csr"
RULES = ROOT / ".work_hdcv/fixpoint_rules640/rules640"
GT = ROOT / ".work_hdcv/fixpoint_rules640/runs/chase/rep1"
TEST_GOALS = ROOT / ".work_hdcv/fixpoint_rules640/goals.tsv"
BOUNDED = ROOT / "experiments/fixpoint_policy/run_bounded.py"
SALT = "20261106-fixpoint-q-train-v1"
MEMORY_GATE = 210 * (1 << 30)


def memory_ok():
    return int(subprocess.check_output(["free", "-b"], text=True).splitlines()[1].split()[2]) <= MEMORY_GATE


def gate():
    deadline = time.monotonic() + 900
    while not memory_ok():
        if time.monotonic() >= deadline:
            raise RuntimeError("memory gate did not open within 900s; phase left pending")
        time.sleep(30)


def run(name, command, root):
    directory = root / name
    result = directory / "resource.json"
    if result.exists():
        saved = json.loads(result.read_text())
        if saved["status"] == "completed" and saved["returncode"] == 0:
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
    if saved["status"] != "completed" or saved["returncode"] != 0:
        raise RuntimeError(f"stop after {name}: {saved['status']}, "
                           f"returncode={saved['returncode']}")


def select_goals():
    with TEST_GOALS.open() as stream:
        test = list(csv.DictReader(stream, delimiter="\t"))
    test_specs = {row["spec"] for row in test}
    groups = {"round1": [], "round2": [], "deep": []}
    with (GT / "closure_edges.tsv").open() as stream:
        for row in csv.reader(stream, delimiter="\t"):
            if not row or row[0].startswith("#"):
                continue
            src, relation, dst, round_text = row[:4]
            spec = f"{relation}({src},{dst})"
            if spec in test_specs:
                continue
            round_no = int(round_text)
            stratum = "round1" if round_no == 1 else ("round2" if round_no == 2 else "deep")
            key = hashlib.sha256(f"{SALT}|{relation}|{src}|{dst}".encode()).hexdigest()
            groups[stratum].append({"stratum": stratum, "relation": relation,
                                    "src": src, "dst": dst, "spec": spec,
                                    "round": str(round_no), "hash": key})
    selected, used = [], set()
    for stratum in ("round1", "round2", "deep"):
        candidates = sorted(groups[stratum], key=lambda row: row["hash"])
        chosen = []
        for prefer_new in (True, False):
            for row in candidates:
                if row in chosen or (prefer_new and row["relation"] in used):
                    continue
                chosen.append(row)
                used.add(row["relation"])
                if len(chosen) == 4:
                    break
            if len(chosen) == 4:
                break
        assert len(chosen) == 4
        selected.extend(chosen)
    WORK.mkdir(parents=True, exist_ok=True)
    with (WORK / "train_goals.tsv").open("w") as stream:
        writer = csv.DictWriter(stream, fieldnames=["goal_id", "stratum", "relation", "src", "dst", "spec", "round"],
                                delimiter="\t", extrasaction="ignore", lineterminator="\n")
        writer.writeheader()
        for index, row in enumerate(selected):
            writer.writerow({"goal_id": f"t{index:02d}", **row})
    shutil.copyfile(TEST_GOALS, WORK / "test_goals.tsv")
    print(f"selected {len(selected)} training goals; test goals={len(test)}", flush=True)


def agent_command(goal, output):
    return ["./build/bin/gar_reason", "--mode", "agent", "--csr", CSR,
            "--rules", str(RULES), "--goal", goal["spec"], "--out", output,
            "--print", "0", "--star-order", "goal",
            "--scorer", "exact", "--budget", "1000000000", "--wall-limit", "900",
            "--max-candidates-per-rule", "100000"]


def trace():
    import run_relevance
    return run_relevance.trace(0)


def evaluate():
    import run_relevance
    goals = run_relevance.load_goals(run_relevance.TEST_GOALS)
    return run_relevance.evaluate(goals, [7])


def train():
    import run_relevance
    return run_relevance.train()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--phase", choices=("select", "trace", "train", "eval", "all"), default="all")
    args = parser.parse_args()
    if args.phase in ("select", "all") or not (WORK / "train_goals.tsv").exists():
        select_goals()
    if args.phase == "select":
        return
    gate()
    if args.phase in ("trace", "all"):
        trace()
    if args.phase in ("train", "all"):
        train()
    if args.phase in ("eval", "all"):
        evaluate()


if __name__ == "__main__":
    main()
