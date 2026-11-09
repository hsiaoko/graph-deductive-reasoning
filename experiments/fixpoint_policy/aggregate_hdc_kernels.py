#!/usr/bin/env python3
import csv
import json
from pathlib import Path
import re
import statistics

from aggregate import facts, internal_time, number, summary

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / ".work_hdcv/fixpoint_q640_kernels"
REF = ROOT / ".work_hdcv/fixpoint_q640"
GT = ROOT / ".work_hdcv/fixpoint_rules640/runs/chase/rep1"
ARMS = ("original", "simd", "bit-lut")
VALUE = re.compile(
    r"\[fixpoint\] value-model: calls=(\d+) infer_ms=([0-9.]+) avg_us=([0-9.]+)")


def main():
    with (REF / "test_goals.tsv").open() as stream:
        goals = list(csv.DictReader(stream, delimiter="\t"))
    gt_facts = facts(GT / "closure_edges.tsv")
    rows, missing = [], []
    for goal in goals:
        goal_id = goal["goal_id"]
        reference_chain = (REF / "runs" / goal_id / "q_hdc/chain.tsv").read_bytes()
        for arm in ARMS:
            run = WORK / "runs" / goal_id / arm
            if not (run / "resource.json").exists():
                missing.append(f"{goal_id}/{arm}")
                continue
            resource = json.loads((run / "resource.json").read_text())
            meta = summary(run / "summary.yaml")
            produced_path = run / "closure_edges.tsv"
            produced = facts(produced_path) if produced_path.exists() else set()
            value = VALUE.findall((run / "run.log").read_text(errors="replace"))
            chain = run / "chain.tsv"
            rows.append({
                "goal": goal_id, "arm": arm, "status": resource["status"],
                "wall": resource["wall_sec"],
                "internal_wall": float(internal_time(run) or "nan"),
                "hit": int(meta.get("goal_derived") == "true"),
                "termination": meta.get("termination", resource["status"]),
                "sound": int(produced <= gt_facts),
                "chain_same": int(chain.exists() and
                                  chain.read_bytes() == reference_chain),
                "matches": int(number(meta, "fx_match_embeddings")),
                "ready": int(number(meta, "fx_ready_insertions")),
                "evaluations": int(number(meta, "fx_evaluate_events")),
                "derived": int(number(meta, "derived")),
                "caps": int(number(meta, "fx_candidate_caps")),
                "value_calls": int(value[-1][0]) if value else 0,
                "value_infer_ms": float(value[-1][1]) if value else 0.0,
                "value_avg_us": float(value[-1][2]) if value else 0.0,
                "rss": resource["peak_tree_rss_bytes"] / (1 << 30),
            })
    report = {
        "status": "incomplete" if missing else "complete",
        "planned_runs": len(goals) * len(ARMS), "observed_runs": len(rows),
        "missing_runs": missing,
        "soundness_failures": sum(not row["sound"] for row in rows),
        "chain_mismatches": sum(not row["chain_same"] for row in rows),
        "arms": {}, "rows": rows,
    }
    for arm in ARMS:
        selected = [row for row in rows if row["arm"] == arm]
        if not selected:
            continue
        item = {
            "runs": len(selected),
            "goal_hit_rate": statistics.mean(row["hit"] for row in selected),
            "soundness_rate": statistics.mean(row["sound"] for row in selected),
            "chain_identity_rate": statistics.mean(row["chain_same"]
                                                   for row in selected),
            "macro": {key: statistics.mean(row[key] for row in selected)
                      for key in ("wall", "internal_wall", "matches", "ready",
                                  "evaluations", "derived", "value_infer_ms")},
            "wall_total": sum(row["wall"] for row in selected),
            "internal_wall_total": sum(row["internal_wall"] for row in selected),
            "value_calls": sum(row["value_calls"] for row in selected),
            "caps": sum(row["caps"] for row in selected),
            "timeouts": sum(row["termination"] == "timeout" for row in selected),
            "max_rss": max(row["rss"] for row in selected),
        }
        item["weighted_value_us"] = (
            sum(row["value_infer_ms"] for row in selected) * 1000 /
            item["value_calls"] if item["value_calls"] else 0.0)
        report["arms"][arm] = item
    if "original" in report["arms"]:
        original = report["arms"]["original"]
        for arm in ARMS[1:]:
            if arm not in report["arms"]:
                continue
            item = report["arms"][arm]
            item["vs_original"] = {
                "wall_speedup": original["macro"]["wall"] / item["macro"]["wall"],
                "internal_speedup": original["macro"]["internal_wall"] /
                                    item["macro"]["internal_wall"],
                "inference_speedup": original["macro"]["value_infer_ms"] /
                                     item["macro"]["value_infer_ms"],
            }
    fields = ["goal", "arm", "status", "wall", "internal_wall", "hit",
              "termination", "sound", "chain_same", "matches", "ready",
              "evaluations", "derived", "caps", "value_calls", "value_infer_ms",
              "value_avg_us", "rss"]
    with (WORK / "rows.tsv").open("w") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, delimiter="\t",
                                extrasaction="ignore", lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    (WORK / "summary.json").write_text(json.dumps(report, indent=2) + "\n")

    lines = ["# Fixpoint HDC value-model kernel 对照（rules640，36 goal）", "",
             f"状态：{report['status']}；观测 {len(rows)}/{len(goals) * len(ARMS)}；"
             f"soundness failures={report['soundness_failures']}；"
             f"chain mismatches={report['chain_mismatches']}。", "",
             "三种后端使用同一个 q_hdc 模型；chain_same 表示与修复后原始 HDC-Q 的正式 chain 逐字节一致。", "",
             "| HDC 后端 | goal hit | soundness | chain 一致 | 宏平均外部 wall(s) | 宏平均内部 wall(s) | 宏平均求值 | 宏平均模型推理(ms) | 加权 us/call | 总 wall(s) | cap | timeout |",
             "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for arm in ARMS:
        if arm not in report["arms"]:
            continue
        item = report["arms"][arm]
        macro = item["macro"]
        lines.append(
            f"| {arm} | {item['goal_hit_rate']:.3f} | {item['soundness_rate']:.3f} | "
            f"{item['chain_identity_rate']:.3f} | {macro['wall']:.3f} | "
            f"{macro['internal_wall']:.3f} | {macro['evaluations']:.1f} | "
            f"{macro['value_infer_ms']:.3f} | {item['weighted_value_us']:.4f} | "
            f"{item['wall_total']:.3f} | {item['caps']} | {item['timeouts']} |")
    lines += ["", "## 相对原始后端", "",
              "| 后端 | 外部 wall 加速 | 内部 wall 加速 | 模型推理加速 |",
              "|---|---:|---:|---:|"]
    for arm in ARMS[1:]:
        if arm in report["arms"]:
            comparison = report["arms"][arm]["vs_original"]
            lines.append(f"| {arm} | {comparison['wall_speedup']:.3f}x | "
                         f"{comparison['internal_speedup']:.3f}x | "
                         f"{comparison['inference_speedup']:.3f}x |")
    reference_rows = list(csv.DictReader((REF / "rows.tsv").open(), delimiter="\t"))
    lines += ["", "## 原正式对照（不同批次，作为参照）", "",
              "| 方法 | 宏平均外部 wall(s) | 宏平均内部 wall(s) | 宏平均求值 |",
              "|---|---:|---:|---:|"]
    for arm, label in (("det", "D"), ("q_numeric", "numeric-Q"),
                       ("q_hdc", "HDC-Q original")):
        selected = [row for row in reference_rows if row["arm"] == arm]
        lines.append(f"| {label} | {statistics.mean(float(row['wall']) for row in selected):.3f} | "
                     f"{statistics.mean(float(row['internal_wall']) for row in selected):.3f} | "
                     f"{statistics.mean(int(row['evaluations']) for row in selected):.1f} |")
    lines += ["", "bit-LUT 只把有限分箱词表的加权点积移到加载期；"
              "SIMD/LUT 不改变训练数据、HDC 维度或逻辑求值。", ""]
    (WORK / "summary.md").write_text("\n".join(lines))
    print(json.dumps({"status": report["status"], "observed": len(rows),
                      "soundness_failures": report["soundness_failures"],
                      "chain_mismatches": report["chain_mismatches"]}, indent=2))


if __name__ == "__main__":
    main()
