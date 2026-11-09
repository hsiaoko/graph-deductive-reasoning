#!/usr/bin/env python3
import argparse
import csv
import json
from pathlib import Path
import statistics

from aggregate import facts, internal_time, number, summary
import run_fixpoint_q as base

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / ".work_hdcv/fixpoint_unified640"
GT = ROOT / ".work_hdcv/fixpoint_rules640/runs/chase/rep1"
SEEDS = (7, 13, 42)
ARMS = ("goal_chase", "deterministic", "random", "q_numeric", "q_hdc")
LABELS = {
    "goal_chase": "GoalChase",
    "deterministic": "Deterministic",
    "random": "Random",
    "q_numeric": "Learned numeric-Q",
    "q_hdc": "Learned HDC-Q",
}
MEDIAN_KEYS = ("wall", "internal_wall", "branches", "ready", "matches",
               "evaluations", "derived")


def command_for(goal, arm, seed):
    name = f"{goal['goal_id']}/{arm}/seed{seed}"
    output = WORK / "runs" / name
    command = base.agent_command(goal, output) + [
        "--fair-interval", "0", "--policy-seed", str(seed),
    ]
    if arm == "goal_chase":
        command += ["--policy", "deterministic", "--scorer", "null"]
    elif arm == "deterministic":
        command += ["--policy", "deterministic", "--scorer", "exact"]
    elif arm == "random":
        command += ["--policy", "random", "--scorer", "exact"]
    elif arm == "q_numeric":
        command += ["--policy", "q-numeric", "--scorer", "exact",
                    "--value-model", str(base.WORK / "models/q_numeric.meta")]
    elif arm == "q_hdc":
        command += ["--policy", "q-hdc", "--scorer", "exact",
                    "--value-model", str(base.WORK / "models/q_hdc.meta"),
                    "--hdc-value-backend", "original"]
    else:
        raise ValueError(f"unknown arm: {arm}")
    return name, command


def evaluate(goal_id=None, selected_arm=None):
    base.gate()
    with base.TEST_GOALS.open() as stream:
        goals = list(csv.DictReader(stream, delimiter="\t"))
    for goal in goals:
        if goal_id is not None and goal["goal_id"] != goal_id:
            continue
        for arm in ARMS:
            if selected_arm is not None and arm != selected_arm:
                continue
            for seed in SEEDS:
                name, command = command_for(goal, arm, seed)
                base.run(name, command, WORK / "runs")


def read_run(goal, arm, seed):
    run = WORK / "runs" / goal / arm / f"seed{seed}"
    resource = json.loads((run / "resource.json").read_text())
    meta = summary(run / "summary.yaml")
    internal = internal_time(run)
    if not meta or internal == "":
        raise RuntimeError(f"incomplete execution output: {run}")
    output = run / "closure_edges.tsv"
    produced = facts(output) if output.exists() else set()
    return {
        "goal": goal, "arm": arm, "seed": seed,
        "status": resource["status"], "wall": resource["wall_sec"],
        "internal_wall": float(internal),
        "hit": int(meta.get("goal_derived") == "true"),
        "termination": meta.get("termination", resource["status"]),
        "produced": produced,
        "branches": int(number(meta, "fx_select_events")),
        "matches": int(number(meta, "fx_match_embeddings")),
        "ready": int(number(meta, "fx_ready_insertions")),
        "evaluations": int(number(meta, "fx_evaluate_events")),
        "derived": int(number(meta, "derived")),
        "caps": int(number(meta, "fx_candidate_caps")),
        "rss": resource["peak_tree_rss_bytes"] / (1 << 30),
    }


def aggregate():
    with base.TEST_GOALS.open() as stream:
        goals = list(csv.DictReader(stream, delimiter="\t"))
    gt_facts = facts(GT / "closure_edges.tsv")
    rows, missing = [], []
    for goal in goals:
        for arm in ARMS:
            for seed in SEEDS:
                run = WORK / "runs" / goal["goal_id"] / arm / f"seed{seed}"
                if not (run / "resource.json").exists():
                    missing.append(f"{goal['goal_id']}/{arm}/seed{seed}")
                    continue
                row = read_run(goal["goal_id"], arm, seed)
                row["stratum"] = goal["stratum"]
                row["sound"] = int(row.pop("produced") <= gt_facts)
                rows.append(row)
    report = {
        "status": "incomplete" if missing else "complete",
        "planned_runs": len(goals) * len(ARMS) * len(SEEDS),
        "observed_runs": len(rows), "missing_runs": missing,
        "validation_failures": sum(not row["sound"] for row in rows),
        "protocol": "experiments/fixpoint_policy/UNIFIED640_PREREGISTRATION.md",
        "arms": {}, "rows": rows,
    }
    per_goal_rows = []
    for arm in ARMS:
        arm_rows = [row for row in rows if row["arm"] == arm]
        per_goal = []
        for goal in goals:
            group = [row for row in arm_rows if row["goal"] == goal["goal_id"]]
            if not group:
                continue
            item = {"goal": goal["goal_id"], "stratum": goal["stratum"],
                    "runs": len(group)}
            for key in MEDIAN_KEYS:
                item[key] = statistics.median(row[key] for row in group)
            item.update({
                "hit": statistics.mean(row["hit"] for row in group),
                "sound": all(row["sound"] for row in group),
                "caps": sum(row["caps"] for row in group),
                "timeouts": sum(row["termination"] == "timeout" for row in group),
            })
            per_goal.append(item)
            per_goal_rows.append({"arm": arm, **item})
        if not per_goal:
            continue
        report["arms"][arm] = {
            "goals": len(per_goal), "runs": len(arm_rows),
            "goal_hit_rate": statistics.mean(row["hit"] for row in arm_rows),
            "soundness_rate": statistics.mean(row["sound"] for row in arm_rows),
            "macro": {key: statistics.mean(item[key] for item in per_goal)
                      for key in MEDIAN_KEYS},
            "batch_wall": sum(item["wall"] for item in per_goal),
            "batch_internal_wall": sum(item["internal_wall"] for item in per_goal),
            "observed_wall_total": sum(row["wall"] for row in arm_rows),
            "total_caps": sum(row["caps"] for row in arm_rows),
            "timeout_runs": sum(row["termination"] == "timeout" for row in arm_rows),
            "max_rss": max(row["rss"] for row in arm_rows),
            "per_goal": per_goal,
        }
    truncated = [row for row in rows
                 if row["caps"] or row["termination"] != "goal"]
    if not missing and truncated:
        report["status"] = "complete_with_truncation"
    report["truncated_runs"] = len(truncated)
    fields = ["goal", "stratum", "arm", "seed", "status", "wall",
              "internal_wall", "hit", "termination", "sound", "branches",
              "matches", "ready", "evaluations", "derived", "caps", "rss"]
    WORK.mkdir(parents=True, exist_ok=True)
    with (WORK / "rows.tsv").open("w") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, delimiter="\t",
                                extrasaction="ignore", lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    with (WORK / "per_goal.tsv").open("w") as stream:
        fields = ["arm", "goal", "stratum", "runs", *MEDIAN_KEYS,
                  "hit", "sound", "caps", "timeouts"]
        writer = csv.DictWriter(stream, fieldnames=fields, delimiter="\t",
                                extrasaction="ignore", lineterminator="\n")
        writer.writeheader()
        writer.writerows(per_goal_rows)
    (WORK / "summary.json").write_text(json.dumps(report, indent=2) + "\n")

    lines = ["# Unified five-method comparison（rules640，36 goal）", "",
             f"状态：{report['status']}；观测 {len(rows)}/{report['planned_runs']}；"
             f"soundness failures={report['validation_failures']}。", "",
             "所有方法均为 36 goals × seeds {7,13,42}；逐 goal 三 run 中位数后再做宏平均。", "",
             "| 方法 | goal hit | soundness | 宏平均外部 wall(s) | 宏平均内部 wall(s) | "
             "宏平均选择分支 | 宏平均 ready 候选 | 宏平均完整匹配 | 宏平均求值/派生 | "
             "36-goal 代表外部 wall 合计(s) | cap | timeout |",
             "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for arm in ARMS:
        if arm not in report["arms"]:
            continue
        item = report["arms"][arm]
        macro = item["macro"]
        lines.append(
            f"| {LABELS[arm]} | {item['goal_hit_rate']:.3f} | "
            f"{item['soundness_rate']:.3f} | {macro['wall']:.3f} | "
            f"{macro['internal_wall']:.3f} | {macro['branches']:.1f} | "
            f"{macro['ready']:.1f} | {macro['matches']:.1f} | "
            f"{macro['evaluations']:.1f} | {item['batch_wall']:.3f} | "
            f"{item['total_caps']} | {item['timeout_runs']} |")
    if "deterministic" in report["arms"]:
        reference = report["arms"]["deterministic"]["macro"]
        lines += ["", "## 相对 Deterministic", "",
                  "| 方法 | 外部 wall 加速 | 内部 wall 加速 | 选择分支减少 | 真实求值减少 |",
                  "|---|---:|---:|---:|---:|"]
        for arm in ARMS:
            if arm == "deterministic" or arm not in report["arms"]:
                continue
            macro = report["arms"][arm]["macro"]
            lines.append(
                f"| {LABELS[arm]} | {reference['wall'] / macro['wall']:.3f}x | "
                f"{reference['internal_wall'] / macro['internal_wall']:.3f}x | "
                f"{100 * (1 - macro['branches'] / reference['branches']):.3f}% | "
                f"{100 * (1 - macro['evaluations'] / reference['evaluations']):.3f}% |")
    lines += ["", "选择分支为 `fx_select_events`，包括零成本跳过已知结论；"
              "真实求值为 `fx_evaluate_events`。所有方法使用相同 seeds、fair=0、"
              "budget、cap、star order 和资源限制。", ""]
    (WORK / "summary.md").write_text("\n".join(lines))
    print(json.dumps({"status": report["status"], "observed": len(rows),
                      "validation_failures": report["validation_failures"]}, indent=2))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--phase", choices=("eval", "aggregate", "all"), default="all")
    parser.add_argument("--goal-id")
    parser.add_argument("--arm", choices=ARMS)
    args = parser.parse_args()
    if args.phase in ("eval", "all"):
        evaluate(args.goal_id, args.arm)
    if args.phase in ("aggregate", "all") and args.goal_id is None and args.arm is None:
        aggregate()


if __name__ == "__main__":
    main()
