#!/usr/bin/env python3
import argparse
import csv
import json
from pathlib import Path
import statistics
import subprocess

import run_fixpoint_q as base
from aggregate import facts, internal_time, number, summary

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / ".work_hdcv/proof_gate_train_cpu"
TEST = ROOT / ".work_hdcv/proof_star36_cpu_gated"
GOALS = ROOT / ".work_hdcv/fixpoint_q640/train_goals.tsv"
MODEL = ROOT / ".work_hdcv/proof_dag_validation/model/proof_star.meta"
GT = ROOT / ".work_hdcv/fixpoint_rules640/runs/chase/rep1"


def section(path, name):
    result = {}
    active = False
    for line in path.read_text().splitlines():
        if line == name + ":":
            active = True
        elif active and line.startswith(" ") and ":" in line:
            key, value = line.strip().split(":", 1)
            result[key] = value
        elif line and not line.startswith(" "):
            active = False
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--work", type=Path, default=WORK)
    parser.add_argument("--test-on-gain", action="store_true")
    args = parser.parse_args()
    work = args.work.resolve()
    with GOALS.open() as stream:
        goals = list(csv.DictReader(stream, delimiter="\t"))
    for goal in goals:
        for arm in ("baseline", "proof"):
            name = f"{goal['goal_id']}/{arm}/seed7"
            output = work / "runs" / name
            command = base.agent_command(goal, output) + [
                "--engine", "cpu", "--fair-interval", "0", "--policy-seed", "7",
                "--policy", "deterministic", "--scorer", "exact",
            ]
            if arm == "proof":
                command += ["--proof-star-model", str(MODEL),
                            "--proof-star-weight", "0.5",
                            "--proof-star-min-cost", "0"]
            base.run(name, command, work / "runs")

    gt_facts = facts(GT / "closure_edges.tsv")
    rows = []
    for goal in goals:
        for arm in ("baseline", "proof"):
            run = work / "runs" / goal["goal_id"] / arm / "seed7"
            resource = json.loads((run / "resource.json").read_text())
            meta = summary(run / "summary.yaml")
            proof = section(run / "summary.yaml", "proof_star")
            rows.append({
                "goal": goal["goal_id"], "arm": arm,
                "wall": resource["wall_sec"],
                "internal_wall": float(internal_time(run)),
                "hit": int(meta.get("goal_derived") == "true"),
                "termination": meta.get("termination"),
                "sound": int(facts(run / "closure_edges.tsv") <= gt_facts),
                "evaluations": int(number(meta, "evaluate_events")),
                "derived": int(number(meta, "derived")),
                "caps": int(number(meta, "candidate_caps")),
                "estimated_cost": float(proof.get("estimated_cost", 0)),
                "active": int(proof.get("active") == "true"),
            })
    baseline = {row["goal"]: row for row in rows if row["arm"] == "baseline"}
    proof = {row["goal"]: row for row in rows if row["arm"] == "proof"}
    costs = {goal: proof[goal]["estimated_cost"] for goal in proof}
    ordered = sorted(set(costs.values()))
    thresholds = [0.0]
    thresholds.extend((a + b) / 2 for a, b in zip(ordered, ordered[1:]))
    thresholds.append(float("inf"))
    baseline_wall = statistics.mean(row["wall"] for row in baseline.values())
    baseline_hits = sum(row["hit"] for row in baseline.values())
    candidates = []
    for threshold in thresholds:
        selected = [proof[goal] if costs[goal] >= threshold else baseline[goal]
                    for goal in sorted(baseline)]
        candidate = {
            "threshold": threshold,
            "guided_goals": sum(costs[goal] >= threshold for goal in costs),
            "wall": statistics.mean(row["wall"] for row in selected),
            "evaluations": statistics.mean(row["evaluations"] for row in selected),
            "hits": sum(row["hit"] for row in selected),
            "sound": all(row["sound"] for row in selected),
        }
        candidate["valid"] = (candidate["sound"] and
                              candidate["hits"] >= baseline_hits)
        candidates.append(candidate)
    valid = [row for row in candidates if row["valid"]]
    best = min(valid, key=lambda row: row["wall"])
    report = {
        "status": "complete", "goals": len(goals), "runs": len(rows),
        "baseline_wall": baseline_wall,
        "proof_wall": statistics.mean(row["wall"] for row in proof.values()),
        "baseline_hits": baseline_hits,
        "proof_hits": sum(row["hit"] for row in proof.values()),
        "selected_threshold": best["threshold"],
        "selected_wall": best["wall"],
        "selected_guided_goals": best["guided_goals"],
        "training_speedup": baseline_wall / best["wall"],
        "training_gain": best["wall"] < baseline_wall,
        "candidates": candidates, "rows": rows,
    }
    work.mkdir(parents=True, exist_ok=True)
    (work / "selection.json").write_text(json.dumps(report, indent=2) + "\n")
    fields = list(rows[0])
    with (work / "rows.tsv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, delimiter="\t",
                                lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    lines = [
        "# Proof-star 成本门控：训练 goal 选择", "",
        f"12 goals、24 runs；baseline hits={baseline_hits}，"
        f"proof hits={report['proof_hits']}。", "",
        f"D wall={baseline_wall:.3f}s；全 proof wall={report['proof_wall']:.3f}s。",
        f"冻结阈值={best['threshold']:.6g}；启用 {best['guided_goals']}/12 goals。",
        f"门控训练 wall={best['wall']:.3f}s；加速={report['training_speedup']:.3f}x。",
        "", "阈值只使用训练 goals 选择，不读取 36 个测试 goal 的结果。", "",
    ]
    (work / "summary.md").write_text("\n".join(lines))
    print(json.dumps({key: value for key, value in report.items()
                      if key not in ("rows", "candidates")}, indent=2))
    if args.test_on_gain and report["training_gain"]:
        subprocess.run([
            "python3", str(ROOT / "experiments/fixpoint_policy/run_proof_star36.py"),
            "--work", str(TEST), "--weight", "0.5", "--seed", "7",
            "--min-cost", str(best["threshold"]),
        ], cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
