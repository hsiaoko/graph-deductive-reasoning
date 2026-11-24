#!/usr/bin/env python3
import argparse
import csv
import json
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / ".work_hdcv/relevance640"
CSR = "/ssd_data/zhuxk/graphs/KG/fb15k-237/immutable_csr"
RULES = ROOT / ".work_hdcv/fixpoint_rules640/rules640"
GT = ROOT / ".work_hdcv/fixpoint_rules640/runs/chase/rep1"
TRAIN_GOALS = ROOT / ".work_hdcv/fixpoint_q640/train_goals.tsv"
TEST_GOALS = ROOT / ".work_hdcv/fixpoint_q640/test_goals.tsv"
BOUNDED = ROOT / "experiments/fixpoint_policy/run_bounded.py"
HDC_DIM = 128
VALIDATION_IDS = ("t01", "t06", "t10")


def load_goals(path):
    with path.open() as stream:
        return list(csv.DictReader(stream, delimiter="\t"))


def write_validation_goals():
    goals = [row for row in load_goals(TRAIN_GOALS) if row["goal_id"] in VALIDATION_IDS]
    WORK.mkdir(parents=True, exist_ok=True)
    path = WORK / "validation_goals.tsv"
    with path.open("w") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(goals[0]), delimiter="\t",
                                lineterminator="\n")
        writer.writeheader()
        writer.writerows(goals)
    return path


def agent_command(goal, output):
    return ["./build/bin/gar_reason", "--mode", "agent", "--engine", "cpu",
            "--csr", CSR, "--rules", str(RULES), "--goal", goal["spec"],
            "--out", str(output), "--print", "0", "--star-order", "goal",
            "--scorer", "exact", "--budget", "1000000000", "--wall-limit", "900",
            "--max-candidates-per-rule", "100000", "--hdc-dim", str(HDC_DIM)]


def bounded(name, command, root, timeout=1000, require_hit=False):
    directory = root / name
    result = directory / "resource.json"
    if result.exists():
        saved = json.loads(result.read_text())
        hit = True
        if require_hit:
            summary_path = directory / "summary.yaml"
            hit = summary_path.exists() and "goal_derived: true" in summary_path.read_text()
        if saved["status"] == "completed" and saved["returncode"] == 0 and hit:
            print(f"skip {name}", flush=True)
            return True
    directory.mkdir(parents=True, exist_ok=True)
    output = directory / "run.log"
    result.parent.mkdir(parents=True, exist_ok=True)
    command = ["python3", str(BOUNDED), "--log", str(output), "--result", str(result),
               "--timeout-sec", str(timeout), "--memory-gb", "256",
               "--system-memory-gb", "256", "--", *map(str, command)]
    print(f"run {name}", flush=True)
    subprocess.run(command, cwd=ROOT, check=False)
    saved = json.loads(result.read_text())
    ok = saved["status"] == "completed" and saved["returncode"] == 0
    if ok and require_hit:
        summary_path = directory / "summary.yaml"
        ok = summary_path.exists() and "goal_derived: true" in summary_path.read_text()
    if not ok:
        print(f"failed {name}: {saved['status']} returncode={saved['returncode']}",
              file=sys.stderr, flush=True)
    return ok


def trace(limit):
    failures = []
    goals = load_goals(TRAIN_GOALS)
    if limit:
        goals = goals[:limit]
    for goal in goals:
        for policy in ("deterministic", "random"):
            name = f"{goal['goal_id']}/{policy}"
            output = WORK / "traces" / name
            command = agent_command(goal, output) + [
                "--policy", policy, "--policy-seed", "7", "--wall-limit", "3600",
                "--policy-trace", output / "policy_trace.json",
                "--policy-trace-negatives", "8"]
            if not bounded(name, command, WORK / "traces", timeout=3700,
                           require_hit=True):
                failures.append(name)
    (WORK / "trace_failures.json").write_text(json.dumps(failures, indent=2) + "\n")
    return not failures


def train():
    validation = write_validation_goals()
    reports = []
    for hidden in (4, 16, 32):
        output = WORK / "models" / f"h{hidden}" / "relevance.meta"
        report = output.with_suffix(".report.json")
        command = ["./build/bin/gar_train_policy", "--csr", CSR, "--rules", str(RULES),
                   "--traces", str(WORK / "traces"), "--test-goals", str(TEST_GOALS),
                   "--validation-goals", str(validation), "--out", str(output),
                   "--epochs", "8", "--batch-size", "32", "--hidden", str(hidden),
                   "--label-gamma", "0.5", "--negative-ratio", "4", "--seed", "42"]
        print("train " + " ".join(map(str, command)), flush=True)
        subprocess.run(list(map(str, command)), cwd=ROOT, check=True)
        saved = json.loads(report.read_text())
        saved["hidden"] = hidden
        saved["model"] = str(output)
        reports.append(saved)
    ranked = [row for row in reports if row["validation"]["ranked_goals"] > 0]
    if ranked:
        selected = max(ranked, key=lambda row: (
            row["validation"]["pairwise_accuracy"], row["validation"]["recall_at_10"],
            -row["validation"]["mse"]))
    else:
        selected = min(reports, key=lambda row: row["epoch_mse"][-1])
    (WORK / "models" / "selection.json").write_text(json.dumps(
        {"selected": selected, "candidates": reports}, indent=2) + "\n")
    source = Path(selected["model"])
    (WORK / "models" / "selected.meta").write_bytes(source.read_bytes())
    (WORK / "models" / "selected.bin").write_bytes(source.with_suffix(".bin").read_bytes())
    print(f"selected hidden={selected['hidden']}", flush=True)


def selected_model():
    selection = json.loads((WORK / "models" / "selection.json").read_text())
    return Path(selection["selected"]["model"])


def evaluate(goals, seeds):
    methods = (("Deterministic", "deterministic"),
               ("Random", "random"), ("Learned", "learned"))
    failures = []
    for goal in goals:
        for method, policy in methods:
            for seed in seeds:
                name = f"{goal['goal_id']}/{method}/seed{seed}"
                command = agent_command(goal, WORK / "runs" / name) + [
                    "--policy", policy, "--policy-seed", str(seed), "--fair-interval", "0"]
                if policy == "learned":
                    command += ["--relevance-model", str(selected_model())]
                if not bounded(name, command, WORK / "runs"):
                    failures.append(name)
    (WORK / "run_failures.json").write_text(json.dumps(failures, indent=2) + "\n")
    return not failures


def verify(goals):
    WORK.mkdir(parents=True, exist_ok=True)
    tasks = WORK / "verify_goals.tsv"
    with tasks.open("w") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(goals[0]), delimiter="\t",
                                lineterminator="\n")
        writer.writeheader()
        writer.writerows(goals)
    for method, policy in (("Deterministic", "deterministic"),
                           ("Random", "random"), ("Learned", "learned")):
        command = ["./build/bin/gar_reason", "--mode", "agent-batch", "--engine", "cpu",
                   "--csr", CSR, "--rules", str(RULES), "--tasks", str(tasks),
                   "--workers", "4", "--gt", str(GT), "--out", str(WORK / "verify" / method),
                   "--policy", policy, "--policy-seed", "7", "--scorer", "exact",
                   "--star-order", "goal", "--budget", "1000000000",
                   "--max-candidates-per-rule", "100000", "--hdc-dim", str(HDC_DIM),
                   "--fair-interval", "0"]
        if policy == "learned":
            command += ["--relevance-model", str(selected_model())]
        bounded(method, command, WORK / "verify", timeout=3600)


def yaml_summary(path):
    values = {}
    for line in path.read_text().splitlines():
        if ":" in line:
            key, value = line.split(":", 1)
            values[key.strip()] = value.strip()
    return values


def aggregate(goals, seeds):
    rows = []
    for goal in goals:
        for method in ("Deterministic", "Random", "Learned"):
            for seed in seeds:
                directory = WORK / "runs" / goal["goal_id"] / method / f"seed{seed}"
                resource_path = directory / "resource.json"
                summary_path = directory / "summary.yaml"
                if not resource_path.exists() or not summary_path.exists():
                    continue
                resource = json.loads(resource_path.read_text())
                summary = yaml_summary(summary_path)
                rows.append({"goal": goal["goal_id"], "stratum": goal["stratum"],
                             "method": method, "seed": seed,
                             "wall": resource["wall_sec"],
                             "execution": float(summary["execution_cost_ms"]) / 1000,
                             "derived": int(summary["derived"]),
                             "hit": summary["goal_derived"] == "true",
                             "termination": summary["termination"],
                             "caps": int(summary.get("candidate_caps", 0)),
                             "rss": resource["peak_tree_rss_bytes"]})
    fields = ["goal", "stratum", "method", "seed", "wall", "execution", "derived",
              "hit", "termination", "caps", "rss"]
    WORK.mkdir(parents=True, exist_ok=True)
    with (WORK / "rows.tsv").open("w") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, delimiter="\t",
                                lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    summary = {}
    for method in ("Deterministic", "Random", "Learned"):
        method_rows = [row for row in rows if row["method"] == method]
        per_goal = []
        for goal in goals:
            values = [row for row in method_rows if row["goal"] == goal["goal_id"]]
            if len(values) != len(seeds):
                continue
            per_goal.append({"wall": statistics.median(row["wall"] for row in values),
                             "execution": statistics.median(row["execution"] for row in values),
                             "derived": statistics.median(row["derived"] for row in values)})
        summary[method] = {
            "runs": len(method_rows), "goals": len(per_goal),
            "hit": sum(row["hit"] for row in method_rows) / max(1, len(method_rows)),
            "wall": statistics.mean(row["wall"] for row in per_goal) if per_goal else None,
            "execution": statistics.mean(row["execution"] for row in per_goal) if per_goal else None,
            "derived": statistics.mean(row["derived"] for row in per_goal) if per_goal else None,
            "caps": sum(row["caps"] for row in method_rows),
            "max_rss": max((row["rss"] for row in method_rows), default=0)}
    (WORK / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2), flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--phase", choices=("trace", "train", "eval", "verify",
                                            "aggregate", "all"), default="all")
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--seeds", default="7,13,42")
    parser.add_argument("--skip-verify", action="store_true")
    args = parser.parse_args()
    seeds = [int(value) for value in args.seeds.split(",")]
    goals = load_goals(TEST_GOALS)
    if args.limit:
        goals = goals[:args.limit]
    if args.phase in ("trace", "all"):
        trace(args.limit)
    if args.phase in ("train", "all"):
        train()
    if args.phase in ("eval", "all"):
        evaluate(goals, seeds)
    if args.phase in ("verify", "all") and not args.skip_verify:
        verify(goals)
    if args.phase in ("aggregate", "all"):
        aggregate(goals, seeds)


if __name__ == "__main__":
    main()
