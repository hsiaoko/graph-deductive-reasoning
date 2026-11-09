#!/usr/bin/env python3
import argparse
import csv
import json
from pathlib import Path
import statistics

from aggregate import facts, internal_time, number, summary
import run_fixpoint_q as base

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / ".work_hdcv/fixpoint_goal_chase640"
GT = ROOT / ".work_hdcv/fixpoint_rules640/runs/chase/rep1"


def evaluate(goal_id=None):
    base.gate()
    with base.TEST_GOALS.open() as stream:
        goals = list(csv.DictReader(stream, delimiter="\t"))
    for goal in goals:
        if goal_id is not None and goal["goal_id"] != goal_id:
            continue
        name = f"{goal['goal_id']}/goal_chase"
        output = WORK / "runs" / name
        command = base.agent_command(goal, output) + [
            "--policy", "deterministic", "--scorer", "null",
        ]
        base.run(name, command, WORK / "runs")


def aggregate():
    with base.TEST_GOALS.open() as stream:
        goals = list(csv.DictReader(stream, delimiter="\t"))
    gt_facts = facts(GT / "closure_edges.tsv")
    rows, missing = [], []
    for goal in goals:
        run = WORK / "runs" / goal["goal_id"] / "goal_chase"
        if not (run / "resource.json").exists():
            missing.append(goal["goal_id"])
            continue
        resource = json.loads((run / "resource.json").read_text())
        meta = summary(run / "summary.yaml")
        output = run / "closure_edges.tsv"
        produced = facts(output) if output.exists() else set()
        rows.append({
            "goal": goal["goal_id"], "stratum": goal["stratum"],
            "status": resource["status"], "wall": resource["wall_sec"],
            "internal_wall": float(internal_time(run) or "nan"),
            "hit": int(meta.get("goal_derived") == "true"),
            "termination": meta.get("termination", resource["status"]),
            "sound": int(produced <= gt_facts),
            "branches": int(number(meta, "fx_select_events")),
            "matches": int(number(meta, "fx_match_embeddings")),
            "ready": int(number(meta, "fx_ready_insertions")),
            "evaluations": int(number(meta, "fx_evaluate_events")),
            "derived": int(number(meta, "derived")),
            "caps": int(number(meta, "fx_candidate_caps")),
            "rss": resource["peak_tree_rss_bytes"] / (1 << 30),
        })
    report = {
        "status": "incomplete" if missing else "complete",
        "planned_runs": len(goals), "observed_runs": len(rows),
        "missing_runs": missing,
        "soundness_failures": sum(not row["sound"] for row in rows),
        "rows": rows,
    }
    if rows:
        report["macro"] = {
            key: statistics.mean(row[key] for row in rows)
            for key in ("wall", "internal_wall", "branches", "matches",
                        "ready", "evaluations", "derived")
        }
        report["goal_hit_rate"] = statistics.mean(row["hit"] for row in rows)
        report["soundness_rate"] = statistics.mean(row["sound"] for row in rows)
        report["wall_total"] = sum(row["wall"] for row in rows)
        report["caps"] = sum(row["caps"] for row in rows)
        report["timeouts"] = sum(row["termination"] == "timeout" for row in rows)
        report["max_rss"] = max(row["rss"] for row in rows)
    fields = ["goal", "stratum", "status", "wall", "internal_wall", "hit",
              "termination", "sound", "branches", "matches", "ready",
              "evaluations", "derived", "caps", "rss"]
    WORK.mkdir(parents=True, exist_ok=True)
    with (WORK / "rows.tsv").open("w") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, delimiter="\t",
                                extrasaction="ignore", lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    (WORK / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    lines = ["# GoalChase（rules640，36 goal）", "",
             "实现：Fixpoint + deterministic + NullScorer；相同 DG 过滤、"
             "goal star order、候选上限和提前停止；action 不使用证据或成本排序。", "",
             f"状态：{report['status']}；观测 {len(rows)}/{len(goals)}；"
             f"soundness failures={report['soundness_failures']}。", ""]
    if rows:
        macro = report["macro"]
        lines += [
            "| goal hit | soundness | 宏平均外部 wall(s) | 宏平均内部 wall(s) | "
            "宏平均选择分支 | 宏平均 ready 候选 | 宏平均完整匹配 | 宏平均求值/派生 | "
            "总外部 wall(s) | cap | timeout |",
            "|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
            f"| {report['goal_hit_rate']:.3f} | {report['soundness_rate']:.3f} | "
            f"{macro['wall']:.3f} | {macro['internal_wall']:.3f} | "
            f"{macro['branches']:.1f} | {macro['ready']:.1f} | "
            f"{macro['matches']:.1f} | {macro['evaluations']:.1f} | "
            f"{report['wall_total']:.3f} | {report['caps']} | "
            f"{report['timeouts']} |", "",
            "选择分支采用 `fx_select_events`，包括零成本跳过已知结论；"
            "真实前提求值采用 `fx_evaluate_events`。", "",
        ]
    (WORK / "summary.md").write_text("\n".join(lines))
    print(json.dumps({"status": report["status"], "observed": len(rows),
                      "soundness_failures": report["soundness_failures"]}, indent=2))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--phase", choices=("eval", "aggregate", "all"), default="all")
    parser.add_argument("--goal-id")
    args = parser.parse_args()
    if args.phase in ("eval", "all"):
        evaluate(args.goal_id)
    if args.phase in ("aggregate", "all") and args.goal_id is None:
        aggregate()


if __name__ == "__main__":
    main()
