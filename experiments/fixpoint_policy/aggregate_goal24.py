#!/usr/bin/env python3
import argparse
import csv
import json
from pathlib import Path
import statistics

from aggregate import facts, internal_time, number, sha256, summary

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / ".work_hdcv/fixpoint_goal24"
RUNS = WORK / "runs"
GT = ROOT / ".work_hdcv/full_policy_matrix/gt_closure512"


def median(rows, key):
    values = [float(row[key]) for row in rows]
    return statistics.median(values) if values else 0.0


def main():
    global WORK, RUNS, GT
    parser = argparse.ArgumentParser()
    parser.add_argument("--work", type=Path, default=WORK)
    parser.add_argument("--gt", type=Path, default=GT)
    args = parser.parse_args()
    WORK = args.work.resolve()
    RUNS = WORK / "runs"
    GT = args.gt.resolve()
    with (WORK / "goals.tsv").open() as stream:
        goals = list(csv.DictReader(stream, delimiter="\t"))
    goal_count = len(goals)
    planned_runs = 3 + 6 * goal_count
    gt_facts = facts(GT / "closure_edges.tsv")
    gt_hash = sha256(GT / "closure_edges.tsv")
    rows = []
    missing = []
    for goal in goals:
        arms = [("det", f"rep{rep}") for rep in range(1, 4)]
        arms += [("random", f"seed{seed}") for seed in (7, 13, 42)]
        for arm, repetition in arms:
            run = RUNS / "fixpoint" / goal["goal_id"] / arm / repetition
            if not (run / "resource.json").exists():
                missing.append(str(run.relative_to(RUNS)))
                continue
            resource = json.loads(run.joinpath("resource.json").read_text())
            meta = summary(run / "summary.yaml")
            produced = facts(run / "closure_edges.tsv")
            hit = meta.get("goal_derived") == "true"
            rows.append({
                "goal": goal["goal_id"], "stratum": goal["stratum"],
                "arm": arm, "repetition": repetition,
                "wall": resource["wall_sec"], "internal_wall": internal_time(run),
                "hit": int(hit), "termination": meta.get("termination", ""),
                "sound": int(produced <= gt_facts),
                "matches": int(number(meta, "fx_match_embeddings")),
                "ready": int(number(meta, "fx_ready_insertions")),
                "evaluations": int(number(meta, "fx_evaluate_events")),
                "derived": int(number(meta, "derived")),
                "caps": int(number(meta, "fx_candidate_caps")),
                "selection_ms": number(meta, "fx_selection_wall_ms"),
                "score_ms": number(meta, "fx_score_wall_ms"),
                "delta_ms": number(meta, "fx_delta_wall_ms"),
                "rss": resource["peak_tree_rss_bytes"] / (1 << 30),
            })
    chase = []
    for rep in range(1, 4):
        run = RUNS / "chase" / f"rep{rep}"
        if not (run / "resource.json").exists():
            missing.append(str(run.relative_to(RUNS)))
            continue
        resource = json.loads((run / "resource.json").read_text())
        meta = summary(run / "summary.yaml")
        chase.append({
            "wall": resource["wall_sec"],
            "matches": int(number(meta, "embeddings_total")),
            "x_satisfied": int(number(meta, "x_satisfied_total")),
            "candidates": int(number(meta, "derivation_candidates_total")),
            "derived": int(number(meta, "derived_total")),
            "rounds": int(number(meta, "rounds")),
            "fixpoint": meta.get("fixpoint") == "true",
            "closure_match": sha256(run / "closure_edges.tsv") == gt_hash,
            "rss": resource["peak_tree_rss_bytes"] / (1 << 30),
        })
    truncated = [row for row in rows
                 if row["caps"] > 0 or row["termination"] != "goal"]
    status = ("incomplete" if missing else
              "complete_with_truncation" if truncated else "complete")
    report = {
        "status": status,
        "planned_runs": planned_runs, "observed_runs": len(rows) + len(chase),
        "missing_runs": missing, "chase": chase, "fixpoint": {},
        "truncated_fixpoint_runs": len(truncated),
        "validation_failures": sum(not row["sound"] for row in rows) +
                               sum(not row["closure_match"] or not row["fixpoint"]
                                   for row in chase),
    }
    per_goal_rows = []
    for arm in ("det", "random"):
        arm_rows = [row for row in rows if row["arm"] == arm]
        per_goal = []
        for goal in goals:
            group = [row for row in arm_rows if row["goal"] == goal["goal_id"]]
            if not group:
                continue
            item = {"goal": goal["goal_id"], "stratum": goal["stratum"],
                    "runs": len(group)}
            for key in ("wall", "matches", "ready", "evaluations", "derived",
                        "selection_ms", "score_ms", "delta_ms"):
                item[key] = median(group, key)
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
        report["fixpoint"][arm] = {
            "goals": len(per_goal), "runs": len(arm_rows),
            "goal_hit_rate": statistics.mean(row["hit"] for row in arm_rows),
            "soundness_rate": statistics.mean(row["sound"] for row in arm_rows),
            "macro": {key: statistics.mean(item[key] for item in per_goal)
                      for key in ("wall", "matches", "ready", "evaluations",
                                  "derived", "selection_ms", "score_ms", "delta_ms")},
            "batch_wall": sum(item["wall"] for item in per_goal),
            "observed_wall_total": sum(row["wall"] for row in arm_rows),
            "total_caps": sum(item["caps"] for item in per_goal),
            "timeout_runs": sum(row["termination"] == "timeout" for row in arm_rows),
            "max_rss": max(row["rss"] for row in arm_rows),
            "per_goal": per_goal,
        }
    with (WORK / "rows.tsv").open("w") as stream:
        fields = ["goal", "stratum", "arm", "repetition", "wall", "internal_wall",
                  "hit", "termination", "sound", "matches", "ready", "evaluations",
                  "derived", "caps", "selection_ms", "score_ms", "delta_ms", "rss"]
        writer = csv.DictWriter(stream, fieldnames=fields, delimiter="\t",
                                extrasaction="ignore", lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    with (WORK / "per_goal.tsv").open("w") as stream:
        fields = ["arm", "goal", "stratum", "runs", "wall", "matches", "ready",
                  "evaluations", "derived", "hit", "sound", "caps", "timeouts",
                  "selection_ms", "score_ms", "delta_ms"]
        writer = csv.DictWriter(stream, fieldnames=fields, delimiter="\t",
                                extrasaction="ignore", lineterminator="\n")
        writer.writeheader()
        writer.writerows(per_goal_rows)
    if chase:
        chase_wall = statistics.median(row["wall"] for row in chase)
        chase_matches = statistics.median(row["matches"] for row in chase)
        chase_derived = statistics.median(row["derived"] for row in chase)
        for item in report["fixpoint"].values():
            macro = item["macro"]
            item["comparison"] = {
                "cold_speedup": chase_wall / macro["wall"],
                "match_reduction": 1.0 - macro["matches"] / chase_matches,
                "derived_reduction": 1.0 - macro["derived"] / chase_derived,
                "batch_to_chase": item["batch_wall"] / chase_wall,
            }
    (WORK / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    lines = [f"# Fixpoint / Chase {goal_count}-goal 扩展实验", "",
             f"状态：{report['status']}；观测 {report['observed_runs']}/{planned_runs} run；"
             f"soundness failures={report['validation_failures']}。", "",
             "只含可派生 goal；hit 是正例命中，不是正负例分类准确率。", "",
             "## Chase 完整闭包", "",
             "| run | wall(s) | 全轮完整匹配 | X满足 | 去重前派生候选 | 派生 | closure=GT |",
             "|---|---:|---:|---:|---:|---:|---|"]
    for index, row in enumerate(chase, 1):
        lines.append(f"| {index} | {row['wall']:.3f} | {row['matches']} | "
                     f"{row['x_satisfied']} | {row['candidates']} | {row['derived']} | "
                     f"{row['closure_match']} |")
    lines += ["", f"## Fixpoint（逐 goal 中位数后做 {goal_count}-goal 宏平均）", "",
              "| 方法 | goal hit | soundness | macro wall(s) | macro完整匹配 | macro ready插入 | macro求值 | macro派生 | cap | timeout | 峰值RSS GiB |",
              "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for arm, label in (("det", "Fixpoint-D"), ("random", "Fixpoint-R")):
        if arm not in report["fixpoint"]:
            continue
        item = report["fixpoint"][arm]
        macro = item["macro"]
        lines.append(
            f"| {label} | {item['goal_hit_rate']:.3f} | {item['soundness_rate']:.3f} | "
            f"{macro['wall']:.3f} | {macro['matches']:.1f} | {macro['ready']:.1f} | "
            f"{macro['evaluations']:.1f} | {macro['derived']:.1f} | "
            f"{item['total_caps']} | {item['timeout_runs']} | {item['max_rss']:.3f} |")
    if truncated:
        detail = ", ".join(
            f"{row['goal']}/{row['arm']}/{row['repetition']}="
            f"{row['termination']}(caps={row['caps']})" for row in truncated)
        lines += ["", f"截断明细：{detail}。命中不消除候选截断，"
                  "这些 run 不用于完备性声明。"]
    if chase:
        chase_wall = statistics.median(row["wall"] for row in chase)
        observed_chase = sum(row["wall"] for row in chase)
        observed_all = observed_chase + sum(
            item["observed_wall_total"] for item in report["fixpoint"].values())
        lines += ["", "## 实际运行总时间（包含所有重复 run）", "",
                  "| 方法 | run 数 | 外部 wall 总和(s) |",
                  "|---|---:|---:|",
                  f"| Chase | {len(chase)} | {observed_chase:.3f} |"]
        for arm, label in (("det", "Fixpoint-D"), ("random", "Fixpoint-R")):
            if arm in report["fixpoint"]:
                item = report["fixpoint"][arm]
                lines.append(f"| {label} | {item['runs']} | "
                             f"{item['observed_wall_total']:.3f} |")
        lines += [f"| 合计 | {report['observed_runs']} | {observed_all:.3f} |", "",
                  "这是各进程 `resource.wall_sec` 的总和；串行队列另有 Python 调度开销。", "",
                  f"Chase cold 中位数：{chase_wall:.3f}s；{goal_count}-goal 摊销参考："
                  f"{chase_wall / goal_count:.3f}s/goal。", "",
                  f"| 方法 | 单目标 cold 加速 | 完整匹配减少 | 派生减少 | {goal_count}-goal 中位数时间之和 | 批量/Chase |",
                  "|---|---:|---:|---:|---:|---:|"]
        for arm, label in (("det", "Fixpoint-D"), ("random", "Fixpoint-R")):
            if arm in report["fixpoint"]:
                item = report["fixpoint"][arm]
                comparison = item["comparison"]
                lines.append(
                    f"| {label} | {comparison['cold_speedup']:.3f}x | "
                    f"{100 * comparison['match_reduction']:.3f}% | "
                    f"{100 * comparison['derived_reduction']:.3f}% | "
                    f"{item['batch_wall']:.3f}s | {comparison['batch_to_chase']:.3f}x |")
    lines += ["", "完整匹配是物化工作量，不是搜索树节点数；Chase 为全规则全轮，"
              "Fixpoint 为 goal-cone 增量物化。派生数不得当作分支数。", ""]
    (WORK / "summary.md").write_text("\n".join(lines))
    print(json.dumps({"status": report["status"],
                      "observed": report["observed_runs"],
                      "validation_failures": report["validation_failures"]}, indent=2))


if __name__ == "__main__":
    main()
