#!/usr/bin/env python3
import csv
import json
from pathlib import Path
import re
import statistics

from aggregate import facts, internal_time, number, summary

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / ".work_hdcv/fixpoint_q640"
GT = ROOT / ".work_hdcv/fixpoint_rules640/runs/chase/rep1"
ARMS = (("det", "Fixpoint-D"), ("q_numeric", "Fixpoint-numeric-Q"),
        ("q_hdc", "Fixpoint-HDC-Q"))
VALUE = re.compile(r"\[fixpoint\] value-model: calls=(\d+) infer_ms=([0-9.]+) avg_us=([0-9.]+)")


def run_facts(run):
    path = run / "closure_edges.tsv"
    return facts(path) if path.exists() else set()


def main():
    with (WORK / "test_goals.tsv").open() as stream:
        goals = list(csv.DictReader(stream, delimiter="\t"))
    gt_facts = facts(GT / "closure_edges.tsv")
    rows, missing = [], []
    for goal in goals:
        for arm, _ in ARMS:
            run = WORK / "runs" / goal["goal_id"] / arm
            if not (run / "resource.json").exists():
                missing.append(f"{goal['goal_id']}/{arm}")
                continue
            resource = json.loads((run / "resource.json").read_text())
            meta = summary(run / "summary.yaml")
            produced = run_facts(run)
            value = VALUE.findall((run / "run.log").read_text(errors="replace"))
            rows.append({
                "goal": goal["goal_id"], "stratum": goal["stratum"], "arm": arm,
                "status": resource["status"], "wall": resource["wall_sec"],
                "internal_wall": internal_time(run),
                "hit": int(meta.get("goal_derived") == "true"),
                "termination": meta.get("termination", resource["status"]),
                "sound": int(produced <= gt_facts),
                "matches": int(number(meta, "fx_match_embeddings")),
                "ready": int(number(meta, "fx_ready_insertions")),
                "evaluations": int(number(meta, "fx_evaluate_events")),
                "derived": int(number(meta, "derived")),
                "caps": int(number(meta, "fx_candidate_caps")),
                "selection_ms": number(meta, "fx_selection_wall_ms"),
                "score_ms": number(meta, "fx_score_wall_ms"),
                "delta_ms": number(meta, "fx_delta_wall_ms"),
                "value_calls": int(value[-1][0]) if value else 0,
                "value_infer_ms": float(value[-1][1]) if value else 0.0,
                "value_avg_us": float(value[-1][2]) if value else 0.0,
                "rss": resource["peak_tree_rss_bytes"] / (1 << 30),
            })
    truncated = [row for row in rows if row["caps"] or row["termination"] != "goal"]
    report = {
        "status": "incomplete" if missing else (
            "complete_with_truncation" if truncated else "complete"),
        "planned_runs": len(goals) * len(ARMS), "observed_runs": len(rows),
        "missing_runs": missing, "truncated_runs": len(truncated),
        "validation_failures": sum(not row["sound"] for row in rows),
        "fixpoint": {}, "rows": rows,
    }
    for arm, _ in ARMS:
        selected = [row for row in rows if row["arm"] == arm]
        if not selected:
            continue
        report["fixpoint"][arm] = {
            "runs": len(selected),
            "goal_hit_rate": statistics.mean(row["hit"] for row in selected),
            "soundness_rate": statistics.mean(row["sound"] for row in selected),
            "macro": {key: statistics.mean(row[key] for row in selected)
                      for key in ("wall", "matches", "ready", "evaluations", "derived",
                                  "selection_ms", "score_ms", "delta_ms", "value_infer_ms")},
            "median_goal_wall_sum": sum(row["wall"] for row in selected),
            "observed_wall_total": sum(row["wall"] for row in selected),
            "total_caps": sum(row["caps"] for row in selected),
            "timeout_runs": sum(row["termination"] == "timeout" for row in selected),
            "max_rss": max(row["rss"] for row in selected),
            "value_calls": sum(row["value_calls"] for row in selected),
        }
    report["paired"] = {}
    for baseline, arm in (("det", "q_numeric"), ("det", "q_hdc"),
                          ("q_numeric", "q_hdc")):
        left = {row["goal"]: row for row in rows if row["arm"] == baseline}
        right = {row["goal"]: row for row in rows if row["arm"] == arm}
        common = sorted(left.keys() & right.keys())
        if not common:
            continue
        speedups = [left[g]["wall"] / right[g]["wall"] for g in common]
        reductions = [1 - right[g]["evaluations"] / left[g]["evaluations"]
                      for g in common if left[g]["evaluations"] > 0]
        report["paired"][f"{baseline}->{arm}"] = {
            "goals": len(common),
            "wall_wins": sum(right[g]["wall"] < left[g]["wall"]
                             for g in common),
            "median_wall_speedup": statistics.median(speedups),
            "median_evaluation_reduction": statistics.median(reductions),
        }
    chase_wall = []
    for rep in range(1, 4):
        resource_path = GT.parent / f"rep{rep}" / "resource.json"
        if resource_path.exists():
            chase_wall.append(json.loads(resource_path.read_text())["wall_sec"])
    chase_meta = summary(GT / "summary.yaml")
    if chase_wall and len(report["fixpoint"]) == len(ARMS):
        baseline = statistics.median(chase_wall)
        chase_matches = int(number(chase_meta, "embeddings_total"))
        for arm, _ in ARMS:
            item = report["fixpoint"][arm]
            item["comparison"] = {
                "cold_speedup": baseline / item["macro"]["wall"],
                "match_reduction": 1 - item["macro"]["matches"] / chase_matches,
                "batch_to_chase": item["median_goal_wall_sum"] / baseline,
            }
    fields = ["goal", "stratum", "arm", "status", "wall", "internal_wall", "hit",
              "termination", "sound", "matches", "ready", "evaluations", "derived",
              "caps", "selection_ms", "score_ms", "delta_ms", "value_calls",
              "value_infer_ms", "value_avg_us", "rss"]
    with (WORK / "rows.tsv").open("w") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, delimiter="\t",
                                extrasaction="ignore", lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    (WORK / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    lines = ["# Fixpoint numeric-Q / HDC-Q（rules640，36 goal）", "",
             f"状态：{report['status']}；观测 {len(rows)}/{len(goals) * len(ARMS)}；"
             f"soundness failures={report['validation_failures']}。", "",
             "| 方法 | goal hit | soundness | macro wall(s) | macro完整匹配 | macro ready插入 | macro求值 | macro派生 | cap | timeout | 模型推理ms | 峰值RSS GiB |",
             "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for arm, label in ARMS:
        if arm not in report["fixpoint"]:
            continue
        item = report["fixpoint"][arm]
        macro = item["macro"]
        lines.append(
            f"| {label} | {item['goal_hit_rate']:.3f} | {item['soundness_rate']:.3f} | "
            f"{macro['wall']:.3f} | {macro['matches']:.1f} | {macro['ready']:.1f} | "
            f"{macro['evaluations']:.1f} | {macro['derived']:.1f} | {item['total_caps']} | "
            f"{item['timeout_runs']} | {macro['value_infer_ms']:.3f} | {item['max_rss']:.3f} |")
    if truncated:
        detail = ", ".join(f"{row['goal']}/{row['arm']}={row['termination']}(caps={row['caps']})"
                           for row in truncated)
        lines += ["", f"截断明细：{detail}。"]
    lines += ["", "## 实际运行总时间", "",
              "| 方法 | run 数 | 外部 wall 总和(s) | 36-goal 中位数时间之和(s) |",
              "|---|---:|---:|---:|"]
    for arm, label in ARMS:
        if arm in report["fixpoint"]:
            item = report["fixpoint"][arm]
            lines.append(f"| {label} | {item['runs']} | {item['observed_wall_total']:.3f} | "
                         f"{item['median_goal_wall_sum']:.3f} |")
    if "det" in report["fixpoint"]:
        det = report["fixpoint"]["det"]["macro"]
        lines += ["", "## 相对 deterministic 的变化", "",
                  "| 方法 | wall加速 | 完整匹配减少 | 求值减少 |",
                  "|---|---:|---:|---:|"]
        for arm, label in ARMS[1:]:
            if arm not in report["fixpoint"]:
                continue
            macro = report["fixpoint"][arm]["macro"]
            lines.append(f"| {label} | {det['wall'] / macro['wall']:.3f}x | "
                         f"{100 * (1 - macro['matches'] / det['matches']):.3f}% | "
                         f"{100 * (1 - macro['evaluations'] / det['evaluations']):.3f}% |")
    if "q_numeric" in report["fixpoint"] and "q_hdc" in report["fixpoint"]:
        numeric = report["fixpoint"]["q_numeric"]["macro"]
        hdc = report["fixpoint"]["q_hdc"]["macro"]
        lines += ["", f"HDC 相对 numeric：wall={numeric['wall'] / hdc['wall']:.3f}x；"
                  f"求值减少={100 * (1 - hdc['evaluations'] / numeric['evaluations']):.3f}%；"
                  f"完整匹配减少={100 * (1 - hdc['matches'] / numeric['matches']):.3f}%。"]
    if report["paired"]:
        lines += ["", "## 逐 goal 配对结果", "",
                  "| 对照（前者→后者） | 后者 wall 胜出 goal | 配对 wall 加速中位数 | 配对求值减少中位数 |",
                  "|---|---:|---:|---:|"]
        for pair, item in report["paired"].items():
            lines.append(f"| {pair} | {item['wall_wins']}/{item['goals']} | "
                         f"{item['median_wall_speedup']:.3f}x | "
                         f"{100 * item['median_evaluation_reduction']:.3f}% |")
    if chase_wall:
        lines += ["", f"Chase cold 中位数：{statistics.median(chase_wall):.3f}s。", "",
                  "| 方法 | 单目标 cold 加速 | 完整匹配减少 | 36-goal/Chase |",
                  "|---|---:|---:|---:|"]
        for arm, label in ARMS:
            if arm in report["fixpoint"]:
                comparison = report["fixpoint"][arm]["comparison"]
                lines.append(f"| {label} | {comparison['cold_speedup']:.3f}x | "
                             f"{100 * comparison['match_reduction']:.3f}% | "
                             f"{comparison['batch_to_chase']:.3f}x |")
    model_manifest = WORK / "models/manifest.json"
    if model_manifest.exists():
        manifest = json.loads(model_manifest.read_text())
        lines += ["", f"训练：{manifest['train_nodes']} 行，成功 trace={len(manifest['successful_traces'])}，"
                  f"失败/空 trace={len(manifest['failed_or_empty_traces'])}；测试 goal 不进入训练。"]
    lines += ["", "Q 模型只改变 action 顺序，不改变精确派生；numeric/HDC 使用相同训练行。", ""]
    (WORK / "summary.md").write_text("\n".join(lines))
    print(json.dumps({"status": report["status"], "observed": len(rows),
                      "validation_failures": report["validation_failures"]}, indent=2))


if __name__ == "__main__":
    main()
