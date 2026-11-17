#!/usr/bin/env python3
import argparse
import csv
import json
from pathlib import Path
import statistics


def read_rows(path):
    with path.open() as stream:
        return list(csv.DictReader(stream, delimiter="\t"))


def number(row, key):
    return float(row[key])


def aggregate_by_goal(rows):
    goals = sorted({row["goal"] for row in rows})
    result = {}
    for goal in goals:
        group = [row for row in rows if row["goal"] == goal]
        result[goal] = {
            "wall": statistics.median(number(row, "wall") for row in group),
            "evaluations": statistics.median(number(row, "evaluations") for row in group),
            "hit": all(row["hit"] == "1" for row in group),
            "sound": all(row["sound"] == "1" for row in group),
            "fallbacks": sum(int(row["guide_fallbacks"]) for row in group),
        }
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--guided", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    probe_rows = [row for row in read_rows(args.probe / "rows.tsv")
                  if row["arm"] == "jev"]
    guided_rows = [row for row in read_rows(args.guided / "rows.tsv")
                   if row["arm"] == "jev"]
    probe = aggregate_by_goal(probe_rows)
    guided = aggregate_by_goal(guided_rows)
    if set(probe) != set(guided):
        raise ValueError("probe and guided goal sets differ")
    costs = {
        goal: statistics.median(number(row, "guide_estimated_cost")
                                for row in probe_rows if row["goal"] == goal)
        for goal in probe
    }
    ordered_costs = sorted(set(costs.values()))
    thresholds = [0.0]
    thresholds.extend((left + right) / 2
                      for left, right in zip(ordered_costs, ordered_costs[1:]))
    thresholds.append(ordered_costs[-1])
    thresholds.append(float("inf"))
    candidates = []
    for threshold in thresholds:
        selected = []
        for goal in sorted(probe):
            use_guide = costs[goal] >= threshold
            row = dict(guided[goal] if use_guide else probe[goal])
            row.update({"goal": goal, "cost": costs[goal],
                        "guided": use_guide})
            selected.append(row)
        valid = all(row["hit"] and row["sound"] for row in selected)
        valid = valid and sum(row["fallbacks"] for row in selected) == 0
        candidates.append({
            "threshold": threshold,
            "valid": valid,
            "guided_goals": sum(row["guided"] for row in selected),
            "macro_wall": statistics.mean(row["wall"] for row in selected),
            "macro_evaluations": statistics.mean(
                row["evaluations"] for row in selected),
            "rows": selected,
        })
    valid = [row for row in candidates if row["valid"]]
    if not valid:
        raise RuntimeError("no valid threshold on the training goals")
    best = min(valid, key=lambda row: row["macro_wall"])
    baseline = statistics.mean(row["wall"] for row in probe.values())
    report = {
        "train_goals": len(probe),
        "guide_max_options": 64,
        "selected_threshold": best["threshold"],
        "selected_guided_goals": best["guided_goals"],
        "selected_macro_wall": best["macro_wall"],
        "deterministic_macro_wall": baseline,
        "train_speedup": baseline / best["macro_wall"],
        "candidates": candidates,
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({key: value for key, value in report.items()
                      if key != "candidates"}, indent=2))


if __name__ == "__main__":
    main()
