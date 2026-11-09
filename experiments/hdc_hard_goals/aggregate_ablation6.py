#!/usr/bin/env python3
import csv
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / ".work_hdcv/hdc_ablation6"
RUNS = WORK / "runs"
GT = ROOT / ".work_hdcv/of_chase30m/closure_edges.tsv"
EXPAND = re.compile(r"\[bnb\] expanded=(\d+) open=(\d+) best_cost=(-?[0-9.]+)")
GOAL = re.compile(r"\[bnbl\] goal (\d+) ([0-9.]+)")
DEBUG_EXPAND = re.compile(r"\[bnbl\] expand (\d+) (\d+) (\d) ([0-9.]+)")
VALUE = re.compile(r"\[bnb\] value-model: calls=(\d+) infer_ms=([0-9.]+) avg_us=([0-9.]+)")


def meta(path):
    result = {}
    if not path.exists():
        return result
    for line in path.read_text().splitlines():
        if line and not line.startswith(" ") and ":" in line:
            key, value = line.split(":", 1)
            result[key] = value.strip()
    return result


def number(values, key, default=-1.0):
    try:
        return float(values.get(key, default))
    except ValueError:
        return default


def log_progress(path):
    text = path.read_text(errors="replace") if path.exists() else ""
    expansions = [int(m.group(1)) for m in EXPAND.finditer(text)]
    first_goal_expansions = None
    count = 0
    for line in text.splitlines():
        if DEBUG_EXPAND.search(line):
            count += 1
        elif GOAL.search(line) and first_goal_expansions is None:
            first_goal_expansions = count
    value = list(VALUE.finditer(text))
    return {
        "partial_expansions": expansions[-1] if expansions else count,
        "log_first_goal_expansions": first_goal_expansions,
        "value_calls": int(value[-1].group(1)) if value else 0,
        "value_infer_ms": float(value[-1].group(2)) if value else 0.0,
        "value_avg_us": float(value[-1].group(3)) if value else 0.0,
    }


def facts(path):
    if not path.exists():
        return set()
    with path.open() as stream:
        return {(row[0], row[1], row[2]) for row in csv.reader(stream, delimiter="\t")
                if row and not row[0].startswith("#")}


def main():
    with (WORK / "goals.tsv").open() as stream:
        goals = list(csv.DictReader(stream, delimiter="\t"))
    rows = []
    wanted = set()
    for goal in goals:
        for arm in ("utility", "numeric", "hdc"):
            run = RUNS / goal["goal_id"] / arm
            if not (run / "resource.json").exists():
                rows.append({"goal": goal["goal_id"], "split": goal["split"],
                             "arm": arm, "status": "missing"})
                continue
            resource = json.loads((run / "resource.json").read_text())
            values = meta(run / "summary.yaml")
            progress = log_progress(run / "run.log")
            produced = facts(run / "closure_edges.tsv")
            wanted |= produced
            hit = values.get("goal_derived") == "true"
            final_expansions = number(values, "expansions")
            first_expansions = number(values, "first_goal_expansions")
            if first_expansions < 0 and progress["log_first_goal_expansions"] is not None:
                first_expansions = progress["log_first_goal_expansions"]
            rows.append({
                "goal": goal["goal_id"], "split": goal["split"], "arm": arm,
                "status": resource["status"],
                "termination": values.get("termination", resource["status"]),
                "wall": resource["wall_sec"], "hit": int(hit),
                "certified": int(values.get("certified_optimal") == "true"),
                "proof_cost": number(values, "total_cost"),
                "expansions": final_expansions,
                "partial_expansions": progress["partial_expansions"],
                "first_goal_expansions": first_expansions,
                "first_goal_wall_ms": number(values, "first_goal_wall_ms"),
                "derived": int(number(values, "derived", 0)),
                "value_calls": progress["value_calls"],
                "value_infer_ms": progress["value_infer_ms"],
                "value_avg_us": progress["value_avg_us"],
                "rss": resource["peak_tree_rss_bytes"] / (1 << 30),
                "produced": produced,
            })
    found = set()
    if wanted:
        with GT.open() as stream:
            for row in csv.reader(stream, delimiter="\t"):
                if not row or row[0].startswith("#"):
                    continue
                fact = (row[0], row[1], row[2])
                if fact in wanted:
                    found.add(fact)
    missing_facts = wanted - found
    for row in rows:
        produced = row.pop("produced", set())
        row["sound"] = int(produced <= found) if produced else ""
    fields = ["goal", "split", "arm", "status", "termination", "wall", "hit",
              "certified", "proof_cost", "expansions", "partial_expansions",
              "first_goal_expansions", "first_goal_wall_ms", "derived", "sound",
              "value_calls", "value_infer_ms", "value_avg_us", "rss"]
    with (WORK / "rows.tsv").open("w") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, delimiter="\t",
                                extrasaction="ignore", lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    missing = [row for row in rows if row["status"] == "missing"]
    timeouts = [row for row in rows if row["status"] == "timeout"]
    report = {
        "planned_runs": 18, "observed_runs": 18 - len(missing),
        "status": "incomplete" if missing else (
            "complete_with_truncation" if timeouts else "complete"),
        "timeout_runs": len(timeouts), "soundness_failures": len(missing_facts),
        "missing_derived_facts": sorted(missing_facts), "rows": rows,
    }
    (WORK / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    lines = ["# BNB utility / numeric / HDC 六目标消融", "",
             f"状态：{report['status']}；观测 {report['observed_runs']}/18；"
             f"timeout={len(timeouts)}；soundness failures={len(missing_facts)}。", "",
             "| goal | arm | wall(s) | 展开 | 首个goal展开 | 首个goal wall(ms) | hit | cost | certified | 状态 |",
             "|---|---|---:|---:|---:|---:|---:|---:|---:|---|"]
    for row in rows:
        if row["status"] == "missing":
            continue
        lines.append(
            f"| {row['goal']} | {row['arm']} | {row['wall']:.3f} | "
            f"{row['expansions']:.0f} | {row['first_goal_expansions']:.0f} | "
            f"{row['first_goal_wall_ms']:.1f} | {row['hit']} | "
            f"{row['proof_cost']:.0f} | {row['certified']} | {row['termination']} |")
    lines += ["", "## 可配对比较的加速", ""]
    for goal in goals:
        arms = {row["arm"]: row for row in rows if row["goal"] == goal["goal_id"]}
        for baseline, learned in (("utility", "hdc"), ("utility", "numeric"),
                                  ("numeric", "hdc")):
            if baseline not in arms or learned not in arms:
                continue
            b, l = arms[baseline], arms[learned]
            if b["status"] == "completed" and l["status"] == "completed" and b["wall"] > 0:
                lines.append(f"- {goal['goal_id']} {baseline}/{learned}: "
                             f"wall={b['wall'] / l['wall']:.3f}x；"
                             f"首解展开比={b['first_goal_expansions'] / max(1, l['first_goal_expansions']):.3f}x。")
            elif b["status"] == "timeout" and l["status"] == "completed" and l["wall"] > 0:
                lines.append(f"- {goal['goal_id']} {baseline}/{learned}: "
                             f"wall>{1000.0 / l['wall']:.3f}x（保守下界；基线 timeout）。")
    (WORK / "summary.md").write_text("\n".join(lines) + "\n")
    print(json.dumps({"status": report["status"], "observed": report["observed_runs"],
                      "timeouts": len(timeouts),
                      "soundness_failures": len(missing_facts)}, indent=2))


if __name__ == "__main__":
    main()
