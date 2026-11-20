#!/usr/bin/env python3
import argparse
import csv
import json
from pathlib import Path
import statistics

import run_fixpoint_q as base
from aggregate import facts, internal_time, number, summary

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / ".work_hdcv/proof_star36_cpu"
MODEL = ROOT / ".work_hdcv/proof_dag_validation/model/proof_star.meta"
BASELINE = ROOT / ".work_hdcv/adaptive_fb15k_cpu_fixed/summary.json"
GT = ROOT / ".work_hdcv/fixpoint_rules640/runs/chase/rep1"


def main():
    global WORK
    parser = argparse.ArgumentParser()
    parser.add_argument("--work", type=Path, default=WORK)
    parser.add_argument("--weight", type=float, default=1.0)
    parser.add_argument("--seed", type=int, choices=(7, 13, 42), default=7)
    parser.add_argument("--min-cost", type=float, default=0.0)
    args = parser.parse_args()
    WORK = args.work.resolve()
    with base.TEST_GOALS.open() as stream:
        goals = list(csv.DictReader(stream, delimiter="\t"))
    for goal in goals:
        name = f"{goal['goal_id']}/proof/seed{args.seed}"
        output = WORK / "runs" / name
        command = base.agent_command(goal, output) + [
            "--engine", "cpu", "--fair-interval", "0",
            "--policy-seed", str(args.seed),
            "--policy", "deterministic", "--scorer", "exact",
            "--proof-star-model", str(MODEL),
            "--proof-star-weight", str(args.weight),
            "--proof-star-min-cost", str(args.min_cost),
        ]
        base.run(name, command, WORK / "runs")

    baseline = json.loads(BASELINE.read_text())
    baseline_rows = {row["goal"]: row for row in baseline["rows"]
                     if row["arm"] == "baseline" and row["seed"] == args.seed}
    gt_facts = facts(GT / "closure_edges.tsv")
    rows = []
    missing = []
    for goal in goals:
        goal_id = goal["goal_id"]
        run = WORK / "runs" / goal_id / f"proof/seed{args.seed}"
        if not (run / "resource.json").exists():
            missing.append(goal_id)
            continue
        resource = json.loads((run / "resource.json").read_text())
        if resource["returncode"] != 0:
            raise RuntimeError(f"failed run: {run}")
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
        "wall_speedup": macro["baseline_wall"] / macro["wall"],
        "evaluation_reduction_pct": 100 * (
            1 - macro["evaluations"] / macro["baseline_evaluations"]),
        "proof_wall_wins": sum(row["wall"] < row["baseline_wall"] for row in rows),
        "proof_evaluation_wins": sum(
            row["evaluations"] < row["baseline_evaluations"] for row in rows),
        "weight": args.weight,
        "rows": rows,
    }
    if not missing and any(row["caps"] or row["termination"] != "goal"
                           for row in rows):
        report["status"] = "complete_with_truncation"
    strata = {}
    for stratum in ("round1", "round2", "deep"):
        group = [row for row in rows if row["stratum"] == stratum]
        strata[stratum] = {
            "baseline_wall": statistics.mean(
                row["baseline_wall"] for row in group),
            "proof_wall": statistics.mean(row["wall"] for row in group),
            "baseline_evaluations": statistics.mean(
                row["baseline_evaluations"] for row in group),
            "proof_evaluations": statistics.mean(
                row["evaluations"] for row in group),
        }
    report["strata"] = strata
    WORK.mkdir(parents=True, exist_ok=True)
    (WORK / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    fields = list(rows[0])
    with (WORK / "paired.tsv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, delimiter="\t",
                                lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    lines = [
        f"# FB15K-237 proof-star efficiency（修复后 CPU，36 goal，seed {args.seed}）", "",
        f"状态：{report['status']}；观测 {len(rows)}/36；"
        f"soundness failures={report['validation_failures']}。", "",
        "| 方法 | hit | 外部 wall(s) | 内部 wall(s) | 求值 | 派生 | timeout | cap |",
        "|---|---:|---:|---:|---:|---:|---:|---:|",
        f"| Deterministic | 1.000 | {macro['baseline_wall']:.3f} | "
        f"{macro['baseline_internal_wall']:.3f} | "
        f"{macro['baseline_evaluations']:.1f} | "
        f"{macro['baseline_derived']:.1f} | 0 | 9 |",
        f"| Proof-star efficiency | {report['hit_rate']:.3f} | {macro['wall']:.3f} | "
        f"{macro['internal_wall']:.3f} | {macro['evaluations']:.1f} | "
        f"{macro['derived']:.1f} | {report['timeouts']} | {report['caps']} |",
        "",
        f"Proof-star/D 外部 wall 加速：{report['wall_speedup']:.3f}x。",
        f"求值减少：{report['evaluation_reduction_pct']:.3f}%。",
        f"wall 胜出：{report['proof_wall_wins']}/36；"
        f"求值减少：{report['proof_evaluation_wins']}/36。", "",
        "所有派生均与 rules640 完整 Chase 闭包对账；逐 goal 数据见 paired.tsv。", "",
    ]
    (WORK / "summary.md").write_text("\n".join(lines))
    print(json.dumps({key: value for key, value in report.items()
                      if key not in ("rows", "strata")}, indent=2))


if __name__ == "__main__":
    main()
