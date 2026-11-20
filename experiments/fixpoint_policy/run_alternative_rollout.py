#!/usr/bin/env python3
import argparse
import csv
import json
import os
from pathlib import Path
import subprocess

import run_fixpoint_q as base
from aggregate import number, summary

ROOT = Path(__file__).resolve().parents[2]
NAMES = ["price", "pattern_edges", "log_support", "relfam", "relevd", "conf",
         "dcost", "total", "dp0", "wit", "premises"]
ORIGINAL_TRACES = ROOT / ".work_hdcv/fixpoint_q640/traces"


def command_for(goal, output):
    return base.agent_command(goal, output) + [
        "--engine", "cpu", "--fair-interval", "0", "--policy-seed", "7",
    ]


def read_decisions(path):
    with path.open() as stream:
        reader = csv.DictReader(stream, delimiter="\t")
        reader.fieldnames = ["decision" if name == "# decision" else name
                             for name in reader.fieldnames]
        return list(reader)


def write_replay(path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w") as stream:
        stream.write("# action_id\trule\tembedding\n")
        for row in rows:
            stream.write(f"{row['action_id']}\t{row['rule']}\t"
                         f"{row['embedding']}\n")


def evaluate_count(path):
    resource = json.loads((path / "resource.json").read_text())
    if resource["status"] != "completed" or resource["returncode"] != 0:
        raise RuntimeError(f"invalid rollout execution: {path}")
    meta = summary(path / "summary.yaml")
    return int(number(meta, "evaluate_events")), meta


def write_training_trace(path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w") as stream:
        stream.write("# event\tgoal_derived\tremaining_evaluations\taction_id\trule")
        stream.write("".join("\t" + name for name in NAMES) + "\n")
        for index, row in enumerate(rows):
            values = [str(index), "1", str(row["remaining"]), row["action_id"],
                      row["rule"]]
            values.extend(row[name] for name in NAMES)
            stream.write("\t".join(values) + "\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--work", type=Path,
                        default=ROOT / ".work_hdcv/alternative_rollout_p34")
    parser.add_argument("--stride", type=int, default=50000)
    parser.add_argument("--alternatives", type=int, default=1)
    args = parser.parse_args()
    args.work = args.work.resolve()
    with base.TEST_GOALS.open() as stream:
        goals = {row["goal_id"]: row
                 for row in csv.DictReader(stream, delimiter="\t")}
    goal = goals["p34"]
    base_run = args.work / "runs/p34/base"
    base_command = command_for(goal, base_run) + [
        "--policy", "deterministic",
        "--decision-trace", str(base_run / "decisions.tsv"),
        "--decision-stride", str(args.stride),
        "--decision-alternatives", str(args.alternatives),
    ]
    base.run("p34/base", base_command, args.work / "runs")
    base_total, base_meta = evaluate_count(base_run)
    decisions = read_decisions(base_run / "decisions.tsv")
    chosen = [row for row in decisions if row["chosen"] == "1"]
    alternatives = [row for row in decisions if row["chosen"] == "0"]
    if not alternatives:
        raise RuntimeError("no alternative actions were sampled")

    rollout_rows = []
    training_rows = []
    for index, alternative in enumerate(alternatives):
        decision = int(alternative["decision"])
        before = int(alternative["evaluations_before"])
        replay = chosen[:decision] + [alternative]
        name = f"p34/rollout_{index:03d}"
        output = args.work / "runs" / name
        replay_path = args.work / "replays" / f"{index:03d}.tsv"
        write_replay(replay_path, replay)
        command = command_for(goal, output) + [
            "--policy", "replay", "--replay-actions", str(replay_path),
        ]
        base.run(name, command, args.work / "runs")
        total, meta = evaluate_count(output)
        hit = meta.get("goal_derived") == "true"
        remaining = total - before
        baseline_remaining = base_total - before
        row = {
            "decision": decision, "evaluations_before": before,
            "hit": int(hit), "rollout_evaluations": total,
            "rollout_remaining": remaining if hit else None,
            "baseline_remaining": baseline_remaining,
            "improvement": baseline_remaining - remaining if hit else None,
            "wall": json.loads((output / "resource.json").read_text())["wall_sec"],
            "rule": alternative["rule"], "embedding": alternative["embedding"],
        }
        rollout_rows.append(row)
        if hit and remaining > 0 and baseline_remaining > 0:
            training_rows.extend([
                {**chosen[decision], "remaining": baseline_remaining},
                {**alternative, "remaining": remaining},
            ])

    fields = list(rollout_rows[0])
    with (args.work / "rollouts.tsv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, delimiter="\t",
                                lineterminator="\n")
        writer.writeheader()
        writer.writerows(rollout_rows)
    if not training_rows:
        raise RuntimeError("no successful alternative continuation can be trained on")

    train_root = args.work / "train_traces"
    write_training_trace(train_root / "rollout/p34/q_trace.tsv", training_rows)
    for path in sorted(ORIGINAL_TRACES.glob("*/*/q_trace.tsv")):
        relative = path.relative_to(ORIGINAL_TRACES)
        destination = train_root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        if not destination.exists():
            destination.symlink_to(path)
    no_test_goals = args.work / "diagnostic_no_test_exclusion.tsv"
    no_test_goals.write_text("goal_id\tspec\n")
    subprocess.run([
        "python3", str(ROOT / "experiments/fixpoint_policy/train_fixpoint_q.py"),
        "--traces", str(train_root), "--out", str(args.work / "models"),
        "--test-goals", str(no_test_goals),
    ], cwd=ROOT, check=True, env=dict(os.environ, PYTHONHASHSEED="0"))

    model = args.work / "models/q_numeric.meta"
    for goal_id in ("p34", "p21"):
        name = f"{goal_id}/alternative_model"
        output = args.work / "runs" / name
        command = command_for(goals[goal_id], output) + [
            "--policy", "learned", "--value-model", str(model),
        ]
        base.run(name, command, args.work / "runs")

    result = {
        "status": "complete", "goal": "p34", "stride": args.stride,
        "alternatives_per_state": args.alternatives,
        "rollouts": len(rollout_rows),
        "successful_rollouts": sum(row["hit"] for row in rollout_rows),
        "alternative_better": sum(row["hit"] and row["improvement"] > 0
                                  for row in rollout_rows),
        "alternative_worse": sum(row["hit"] and row["improvement"] < 0
                                 for row in rollout_rows),
        "training_rows": len(training_rows),
        "diagnostic_warning": "p34 participates in rollout training; p34 is an in-sample capacity check, not generalization evidence.",
        "runs": {},
    }
    for name in ("p34/base", "p34/alternative_model", "p21/alternative_model"):
        output = args.work / "runs" / name
        total, meta = evaluate_count(output)
        result["runs"][name] = {
            "wall": json.loads((output / "resource.json").read_text())["wall_sec"],
            "evaluations": total, "derived": int(number(meta, "derived")),
            "hit": meta.get("goal_derived") == "true",
            "termination": meta.get("termination"),
        }
    (args.work / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
