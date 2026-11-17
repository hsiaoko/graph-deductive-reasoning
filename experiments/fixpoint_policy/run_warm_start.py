#!/usr/bin/env python3
import argparse
import csv
import json
import os
from pathlib import Path
import statistics
import subprocess
import time

import run_fixpoint_q as base
from aggregate import number, summary

ROOT = Path(__file__).resolve().parents[2]


def write_goals(path, rows, fieldnames):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames, delimiter="\t",
                                extrasaction="ignore", lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def split_goals(args):
    with args.goals.open() as stream:
        reader = csv.DictReader(stream, delimiter="\t")
        rows = list(reader)
        fields = reader.fieldnames
    if not fields or "goal_id" not in fields or "spec" not in fields:
        raise ValueError("goals must contain goal_id and spec columns")
    if len(rows) < 2:
        raise ValueError("warm-start evaluation needs at least two goals")
    if args.cold_goals <= 0 or args.cold_goals >= len(rows):
        raise ValueError("--cold-goals must leave at least one warm goal")
    cold = rows[:args.cold_goals]
    warm = rows[args.cold_goals:]
    write_goals(args.work / "cold_goals.tsv", cold, fields)
    write_goals(args.work / "warm_goals.tsv", warm, fields)
    return cold, warm


def read_goals(path):
    with path.open() as stream:
        return list(csv.DictReader(stream, delimiter="\t"))


def command_for(args, goal, output):
    return [
        "./build/bin/gar_reason", "--mode", "agent", "--engine", args.engine,
        "--csr", str(args.csr), "--rules", str(args.rules),
        "--goal", goal["spec"], "--out", str(output), "--print", "0",
        "--star-order", "goal", "--scorer", "exact", "--budget", "1000000000",
        "--wall-limit", "900", "--max-candidates-per-rule", "100000",
        "--fair-interval", "0", "--policy-seed", str(args.policy_seed),
    ]


def cold(args, goals):
    for goal in goals:
        name = f"{goal['goal_id']}/cold_llm"
        output = args.work / "traces" / name
        command = command_for(args, goal, output) + [
            "--policy", "deterministic", "--q-trace", str(output / "q_trace.tsv"),
            "--llm-guide", "--guide-strength", str(args.guide_strength),
            "--llm-backend", args.llm_backend,
            "--llm-base-url", args.llm_base_url, "--llm-model", args.llm_model,
            "--llm-timeout-ms", str(args.llm_timeout_ms),
        ]
        if args.llm_mock:
            command.append("--llm-mock")
        base.run(name, command, args.work / "traces")


def train(args):
    started = time.monotonic()
    models = args.work / "models"
    env = dict(os.environ, PYTHONHASHSEED="0")
    subprocess.run([
        "python3", str(ROOT / "experiments/fixpoint_policy/train_fixpoint_q.py"),
        "--traces", str(args.work / "traces"), "--out", str(models),
        "--test-goals", str(args.work / "warm_goals.tsv"),
    ], cwd=ROOT, check=True, env=env)
    model = models / f"q_{args.model}.meta"
    subprocess.run(["./build/bin/value_model_check", str(model)],
                   cwd=ROOT, check=True)
    (models / "train.json").write_text(json.dumps({
        "wall": time.monotonic() - started, "model": model.name,
    }, indent=2) + "\n")


def warm(args, goals):
    model = args.work / "models" / f"q_{args.model}.meta"
    for goal in goals:
        name = f"{goal['goal_id']}/warm_ml"
        output = args.work / "runs" / name
        command = command_for(args, goal, output) + [
            "--policy", "learned", "--value-model", str(model),
        ]
        if args.model == "hdc":
            command += ["--hdc-value-backend", "original"]
        base.run(name, command, args.work / "runs")


def guide_overhead(path):
    result = {"calls": 0.0, "wall_ms": 0.0, "fallbacks": 0.0}
    active = False
    for line in path.read_text().splitlines():
        if line == "guide_overhead:":
            active = True
        elif active and line.startswith("  ") and ":" in line:
            key, value = line.strip().split(":", 1)
            result[key] = float(value)
        elif line and not line.startswith(" "):
            active = False
    return result


def read_run(run, arm):
    resource = json.loads((run / "resource.json").read_text())
    meta = summary(run / "summary.yaml")
    guide = guide_overhead(run / "summary.yaml")
    return {
        "goal": run.parent.name, "arm": arm, "status": resource["status"],
        "wall": resource["wall_sec"],
        "hit": int(meta.get("goal_derived") == "true"),
        "termination": meta.get("termination", resource["status"]),
        "evaluations": int(number(meta, "evaluate_events")),
        "derived": int(number(meta, "derived")),
        "guide_calls": int(guide["calls"]),
        "guide_wall": guide["wall_ms"] / 1000.0,
        "guide_fallbacks": int(guide["fallbacks"]),
    }


def aggregate(args, cold_goals, warm_goals):
    rows = []
    missing = []
    for goal in cold_goals:
        run = args.work / "traces" / goal["goal_id"] / "cold_llm"
        if (run / "resource.json").exists():
            rows.append(read_run(run, "cold_llm"))
        else:
            missing.append(f"{goal['goal_id']}/cold_llm")
    for goal in warm_goals:
        run = args.work / "runs" / goal["goal_id"] / "warm_ml"
        if (run / "resource.json").exists():
            rows.append(read_run(run, "warm_ml"))
        else:
            missing.append(f"{goal['goal_id']}/warm_ml")
    arms = {}
    for arm in ("cold_llm", "warm_ml"):
        selected = [row for row in rows if row["arm"] == arm]
        if not selected:
            continue
        arms[arm] = {
            "runs": len(selected),
            "wall_total": sum(row["wall"] for row in selected),
            "wall_macro": statistics.mean(row["wall"] for row in selected),
            "goal_hit_rate": statistics.mean(row["hit"] for row in selected),
            "evaluations": sum(row["evaluations"] for row in selected),
            "derived": sum(row["derived"] for row in selected),
            "guide_calls": sum(row["guide_calls"] for row in selected),
            "guide_wall": sum(row["guide_wall"] for row in selected),
            "guide_fallbacks": sum(row["guide_fallbacks"] for row in selected),
        }
    train_output = args.work / "models" / "train.json"
    train_wall = (json.loads(train_output.read_text())["wall"]
                  if train_output.exists() else 0.0)
    report = {
        "status": "complete" if not missing else "incomplete",
        "missing_runs": missing, "cold_goals": len(cold_goals),
        "warm_goals": len(warm_goals), "model": args.model,
        "arms": arms, "rows": rows, "train_wall": train_wall,
        "lifecycle_wall": train_wall + sum(row["wall"] for row in rows),
    }
    args.work.mkdir(parents=True, exist_ok=True)
    (args.work / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"status": report["status"], "arms": arms}, indent=2))


def main():
    parser = argparse.ArgumentParser(
        description="Cold-start LLM guide, train Q model, then warm-start ML.")
    parser.add_argument("--work", type=Path,
                        default=ROOT / ".work_hdcv/warm_start640")
    parser.add_argument("--goals", type=Path, default=base.TEST_GOALS)
    parser.add_argument("--csr", type=Path, default=base.CSR)
    parser.add_argument("--rules", type=Path, default=base.RULES)
    parser.add_argument("--cold-goals", type=int, default=4)
    parser.add_argument("--engine", choices=("cpu", "gpu"), default="gpu")
    parser.add_argument("--model", choices=("numeric", "hdc"), default="numeric")
    parser.add_argument("--policy-seed", type=int, default=7)
    parser.add_argument("--guide-strength", type=float, default=1.0)
    parser.add_argument("--llm-backend", choices=("openai", "jev"),
                        default="openai")
    parser.add_argument("--llm-base-url", default="https://api.deepseek.com")
    parser.add_argument("--llm-model", default="deepseek-chat")
    parser.add_argument("--llm-timeout-ms", type=int, default=120000)
    parser.add_argument("--llm-mock", action="store_true")
    parser.add_argument("--phase",
                        choices=("all", "cold", "train", "warm", "aggregate"),
                        default="all")
    args = parser.parse_args()
    args.work = args.work.resolve()
    args.csr = args.csr.resolve()
    args.rules = args.rules.resolve()
    args.goals = args.goals.resolve()

    if args.phase in ("all", "cold"):
        cold_goals, warm_goals = split_goals(args)
    else:
        cold_goals = read_goals(args.work / "cold_goals.tsv")
        warm_goals = read_goals(args.work / "warm_goals.tsv")
    base.gate()
    if args.phase in ("all", "cold"):
        cold(args, cold_goals)
    if args.phase in ("all", "train"):
        train(args)
    if args.phase in ("all", "warm"):
        warm(args, warm_goals)
    if args.phase in ("all", "aggregate"):
        aggregate(args, cold_goals, warm_goals)


if __name__ == "__main__":
    main()
