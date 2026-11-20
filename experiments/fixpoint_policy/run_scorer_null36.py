#!/usr/bin/env python3
import argparse
import csv
import json
from pathlib import Path
import statistics

import run_fixpoint_q as base
from aggregate import facts, internal_time, number, summary

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / ".work_hdcv/scorer_null36_cpu_fixed"
BASELINE = ROOT / ".work_hdcv/adaptive_fb15k_cpu_fixed/summary.json"
GT = ROOT / ".work_hdcv/fixpoint_rules640/runs/chase/rep1"


def main():
    global WORK
    parser = argparse.ArgumentParser()
    parser.add_argument("--work", type=Path, default=WORK)
    args = parser.parse_args()
    WORK = args.work.resolve()
    with base.TEST_GOALS.open() as stream:
        goals = list(csv.DictReader(stream, delimiter="\t"))
    for goal in goals:
        name = f"{goal['goal_id']}/null/seed7"
        output = WORK / "runs" / name
        command = base.agent_command(goal, output) + [
            "--engine", "cpu", "--fair-interval", "0", "--policy-seed", "7",
            "--policy", "deterministic", "--scorer", "null",
        ]
        base.run(name, command, WORK / "runs")

    baseline = json.loads(BASELINE.read_text())
    baseline_rows = {row["goal"]: row for row in baseline["rows"]
                     if row["arm"] == "baseline" and row["seed"] == 7}
    gt_facts = facts(GT / "closure_edges.tsv")
    rows = []
    missing = []
    for goal in goals:
        goal_id = goal["goal_id"]
        run = WORK / "runs" / goal_id / "null/seed7"
        if not (run / "resource.json").exists():
            missing.append(goal_id)
            continue
        resource = json.loads((run / "resource.json").read_text())
        meta = summary(run / "summary.yaml")
        produced = facts(run / "closure_edges.tsv")
        before = baseline_rows[goal_id]
        rows.append({
            "goal": goal_id, "stratum": goal["stratum"],
            "wall": resource["wall_sec"],
            "internal_wall": float(internal_time(run)),
            "hit": int(meta.get("goal_derived") == "true"),
            "termination": meta.get("termination", resource["status"]),
            "sound": int(produced <= gt_facts),
            "branches": int(number(meta, "select_events")),
            "matches": int(number(meta, "match_embeddings")),
            "ready": int(number(meta, "ready_insertions")),
            "evaluations": int(number(meta, "evaluate_events")),
            "derived": int(number(meta, "derived")),
            "caps": int(number(meta, "candidate_caps")),
            "baseline_wall": before["wall"],
            "baseline_internal_wall": before["internal_wall"],
            "baseline_evaluations": before["evaluations"],
            "baseline_derived": before["derived"],
        })
    keys = ("wall", "internal_wall", "branches", "matches", "ready",
            "evaluations", "derived", "baseline_wall",
            "baseline_internal_wall", "baseline_evaluations", "baseline_derived")
    macro = {key: statistics.mean(row[key] for row in rows) for key in keys}
    report = {
        "status": "incomplete" if missing else "complete",
        "planned_runs": len(goals), "observed_runs": len(rows),
        "missing_runs": missing,
        "validation_failures": sum(not row["sound"] for row in rows),
        "hit_rate": statistics.mean(row["hit"] for row in rows),
        "timeouts": sum(row["termination"] == "timeout" for row in rows),
        "caps": sum(row["caps"] for row in rows),
        "macro": macro,
        "wall_speedup_exact_over_null": macro["wall"] / macro["baseline_wall"],
        "evaluation_reduction_exact_pct": 100 * (
            1 - macro["baseline_evaluations"] / macro["evaluations"]),
        "rows": rows,
    }
    if not missing and any(row["caps"] or row["termination"] != "goal"
                           for row in rows):
        report["status"] = "complete_with_truncation"
    WORK.mkdir(parents=True, exist_ok=True)
    (WORK / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    fields = list(rows[0])
    with (WORK / "paired.tsv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, delimiter="\t",
                                lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    lines = [
        "# FB15K-237 scorer=null（修复后 CPU，36 goal，seed 7）", "",
        f"状态：{report['status']}；观测 {len(rows)}/36；"
        f"soundness failures={report['validation_failures']}。", "",
        "| scorer | hit | 外部 wall(s) | 内部 wall(s) | 求值 | 派生 | timeout | cap |",
        "|---|---:|---:|---:|---:|---:|---:|---:|",
        f"| exact | 1.000 | {macro['baseline_wall']:.3f} | "
        f"{macro['baseline_internal_wall']:.3f} | "
        f"{macro['baseline_evaluations']:.1f} | "
        f"{macro['baseline_derived']:.1f} | 0 | 9 |",
        f"| null | {report['hit_rate']:.3f} | {macro['wall']:.3f} | "
        f"{macro['internal_wall']:.3f} | {macro['evaluations']:.1f} | "
        f"{macro['derived']:.1f} | {report['timeouts']} | {report['caps']} |",
        "",
        f"exact 相对 null 的外部 wall 加速："
        f"{report['wall_speedup_exact_over_null']:.3f}x。",
        f"exact 相对 null 的求值减少："
        f"{report['evaluation_reduction_exact_pct']:.3f}%。", "",
        "逐 goal 数据见 paired.tsv；所有派生均与 rules640 完整 Chase 闭包对账。", "",
    ]
    (WORK / "summary.md").write_text("\n".join(lines))
    print(json.dumps({key: value for key, value in report.items()
                      if key != "rows"}, indent=2))


if __name__ == "__main__":
    main()
