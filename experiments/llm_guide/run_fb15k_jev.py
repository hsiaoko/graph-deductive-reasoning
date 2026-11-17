#!/usr/bin/env python3
import argparse
import csv
import hashlib
import json
from pathlib import Path
import shutil
import statistics
import sys
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "experiments/fixpoint_policy"))

from aggregate import facts, internal_time, number
import run_fixpoint_q as base

WORK = ROOT / ".work_hdcv/fb15k_jev640"
GT = ROOT / ".work_hdcv/fixpoint_rules640/runs/chase/rep1"
SEEDS = (7, 13, 42)
ARMS = ("baseline", "jev")
LABELS = {"baseline": "Deterministic", "jev": "Deterministic + local Jev"}
MEDIAN_KEYS = ("wall", "internal_wall", "branches", "ready", "matches",
               "evaluations", "derived", "guide_wall")


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def load_goals(args):
    with args.goals.open() as stream:
        return list(csv.DictReader(stream, delimiter="\t"))


def read_summary(path):
    result = {}
    guide = False
    for line in path.read_text().splitlines():
        if line == "guide_overhead:":
            guide = True
            continue
        if guide and line.startswith(" "):
            key, value = line.strip().split(":", 1)
            result["guide_" + key] = value
        elif ":" in line and not line.startswith(" "):
            guide = False
            key, value = line.split(":", 1)
            result[key] = value.strip()
    return result


def command_for(args, goal, arm, seed):
    name = f"{goal['goal_id']}/{arm}/seed{seed}"
    output = args.work / "runs" / name
    command = base.agent_command(goal, output)
    command[3:3] = ["--engine", args.engine]
    policy = ("deterministic" if args.base_policy == "adaptive" and
              arm == "baseline" else args.base_policy)
    command += ["--fair-interval", "0", "--policy-seed", str(seed),
                "--policy", policy, "--scorer", "exact"]
    if args.value_model is not None and (args.base_policy != "adaptive" or
                                         arm == "jev"):
        command += ["--value-model", str(args.value_model)]
    if args.hdc_value_backend is not None:
        command += ["--hdc-value-backend", args.hdc_value_backend]
    if arm == "jev":
        command += [
            "--llm-guide", "--llm-backend", "jev",
            "--llm-base-url", args.llm_base_url,
            "--llm-model", args.llm_model,
            "--llm-timeout-ms", str(args.llm_timeout_ms),
            "--guide-strength", str(args.guide_strength),
            "--guide-max-options", str(args.guide_max_options),
            "--guide-min-cost", str(args.guide_min_cost),
        ]
        if args.llm_mock:
            command.append("--llm-mock")
    return name, command


def check_service(args):
    base_url = args.llm_base_url.rstrip("/")
    with urlopen(base_url + "/health", timeout=10) as response:
        if response.status != 200:
            raise RuntimeError(f"Jev health returned HTTP {response.status}")
    with urlopen(base_url + "/model_info", timeout=10) as response:
        info = json.load(response)
    if info.get("served_model_name") != args.llm_model:
        raise RuntimeError(
            f"Jev serves {info.get('served_model_name')!r}, expected {args.llm_model!r}")
    return info


def manifest_for(args, model_info):
    return {
        "protocol": "experiments/fixpoint_policy/UNIFIED640_PREREGISTRATION.md",
        "deviations": [
            f"Both arms use the current {args.engine.upper()} AdaDeduce backend; historical tables are not reused as baselines.",
            "Only goal-reachable stars are offered to the guide, capped at the first 255 in baseline star order.",
            "LLM2Jev include_choice_candidates=false avoids repeating the full candidate list in every candidate prompt.",
        ],
        "planned_runs": len(load_goals(args)) * len(ARMS) * len(SEEDS),
        "seeds": SEEDS,
        "arms": ARMS,
        "csr": base.CSR,
        "rules": str(base.RULES),
        "goals": str(args.goals),
        "ground_truth": str(GT),
        "goals_sha256": sha256(args.goals),
        "rule_ids_sha256": sha256(base.RULES.parent / "rule_ids.txt"),
        "ground_truth_sha256": sha256(GT / "closure_edges.tsv"),
        "binary_sha256": sha256(ROOT / "build/bin/gar_reason"),
        "engine": args.engine,
        "policy": args.base_policy,
        "value_model": str(args.value_model) if args.value_model else None,
        "value_model_sha256": sha256(args.value_model) if args.value_model else None,
        "hdc_value_backend": args.hdc_value_backend,
        "scorer": "exact",
        "star_order": "goal",
        "budget": 1000000000,
        "candidate_cap": 100000,
        "fair_interval": 0,
        "guide_strength": args.guide_strength,
        "guide_max_options": args.guide_max_options,
        "guide_min_cost": args.guide_min_cost,
        "llm_mock": args.llm_mock,
        "llm_base_url": args.llm_base_url,
        "llm_model": args.llm_model,
        "llm_timeout_ms": args.llm_timeout_ms,
        "model_info": model_info,
    }


def write_or_check_manifest(args, model_info):
    manifest = manifest_for(args, model_info)
    path = args.work / "manifest.json"
    if path.exists():
        saved = json.loads(path.read_text())
        for key in ("goals_sha256", "rule_ids_sha256", "ground_truth_sha256",
                    "binary_sha256", "llm_model", "guide_strength", "policy",
                    "value_model_sha256", "hdc_value_backend",
                    "guide_max_options", "guide_min_cost", "llm_mock"):
            if saved.get(key) != manifest[key]:
                raise RuntimeError(
                    f"manifest mismatch for {key}: use a new --work directory")
        return
    args.work.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.goals, args.work / "goals.tsv")
    path.write_text(json.dumps(manifest, indent=2) + "\n")


def evaluate(args):
    goals = load_goals(args)
    rule_count = sum(path.is_dir() for path in base.RULES.iterdir())
    if not goals or rule_count != 640:
        raise RuntimeError(f"expected nonempty goals and 640 rules, got {len(goals)} and {rule_count}")
    model_info = None
    if args.arm in (None, "jev") and not args.llm_mock:
        model_info = check_service(args)
    write_or_check_manifest(args, model_info)
    base.gate()
    for goal in goals:
        if args.goal_id is not None and goal["goal_id"] != args.goal_id:
            continue
        for seed in SEEDS:
            if args.seed is not None and seed != args.seed:
                continue
            for arm in ARMS:
                if args.arm is not None and arm != args.arm:
                    continue
                name, command = command_for(args, goal, arm, seed)
                base.run(name, command, args.work / "runs")


def read_run(goal, arm, seed, work):
    run = work / "runs" / goal / arm / f"seed{seed}"
    resource = json.loads((run / "resource.json").read_text())
    meta = read_summary(run / "summary.yaml")
    internal = internal_time(run)
    if not meta or internal == "":
        raise RuntimeError(f"incomplete execution output: {run}")
    return {
        "goal": goal,
        "arm": arm,
        "seed": seed,
        "status": resource["status"],
        "wall": resource["wall_sec"],
        "internal_wall": float(internal),
        "hit": int(meta.get("goal_derived") == "true"),
        "termination": meta.get("termination", resource["status"]),
        "branches": int(number(meta, "select_events")),
        "matches": int(number(meta, "match_embeddings")),
        "ready": int(number(meta, "ready_insertions")),
        "evaluations": int(number(meta, "evaluate_events")),
        "derived": int(number(meta, "derived")),
        "caps": int(number(meta, "candidate_caps")),
        "rss": resource["peak_tree_rss_bytes"] / (1 << 30),
        "chain_sha256": sha256(run / "chain.tsv"),
        "guide_calls": int(number(meta, "guide_calls")),
        "guide_wall": number(meta, "guide_wall_ms") / 1000.0,
        "guide_fallbacks": int(number(meta, "guide_fallbacks")),
        "guide_reachable": int(number(meta, "guide_reachable")),
        "guide_options": int(number(meta, "guide_options")),
        "guide_active": int(meta.get("guide_active") == "true"),
        "guide_estimated_cost": number(meta, "guide_estimated_cost"),
    }


def per_goal_summary(rows, goals, arm):
    result = []
    for goal in goals:
        group = [row for row in rows
                 if row["arm"] == arm and row["goal"] == goal["goal_id"]]
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
            "chain_unique": len({row["chain_sha256"] for row in group}),
            "guide_fallbacks": sum(row["guide_fallbacks"] for row in group),
            "guide_calls": sum(row["guide_calls"] for row in group),
            "guide_reachable": statistics.median(
                row["guide_reachable"] for row in group),
            "guide_options": statistics.median(row["guide_options"] for row in group),
        })
        result.append(item)
    return result


def subset_comparison(rows, seeds):
    selected = [row for row in rows if row["seed"] in seeds]
    result = {}
    for arm in ARMS:
        arm_rows = [row for row in selected if row["arm"] == arm]
        if not arm_rows:
            continue
        result[arm] = {
            "runs": len(arm_rows),
            "wall": statistics.mean(row["wall"] for row in arm_rows),
            "internal_wall": statistics.mean(row["internal_wall"] for row in arm_rows),
            "evaluations": statistics.mean(row["evaluations"] for row in arm_rows),
            "guide_wall": statistics.mean(row["guide_wall"] for row in arm_rows),
            "fallbacks": sum(row["guide_fallbacks"] for row in arm_rows),
        }
    if len(result) == 2:
        result["wall_speedup"] = result["baseline"]["wall"] / result["jev"]["wall"]
        result["evaluation_reduction_pct"] = 100 * (
            1 - result["jev"]["evaluations"] / result["baseline"]["evaluations"])
    return result


def aggregate(args):
    goals = load_goals(args)
    gt_facts = facts(GT / "closure_edges.tsv")
    rows, missing = [], []
    for goal in goals:
        for arm in ARMS:
            for seed in SEEDS:
                run = args.work / "runs" / goal["goal_id"] / arm / f"seed{seed}"
                if not (run / "resource.json").exists():
                    missing.append(f"{goal['goal_id']}/{arm}/seed{seed}")
                    continue
                row = read_run(goal["goal_id"], arm, seed, args.work)
                output = run / "closure_edges.tsv"
                produced = facts(output) if output.exists() else set()
                row["stratum"] = goal["stratum"]
                row["sound"] = int(produced <= gt_facts)
                rows.append(row)
    report = {
        "status": "incomplete" if missing else "complete",
        "planned_runs": len(goals) * len(ARMS) * len(SEEDS),
        "observed_runs": len(rows),
        "missing_runs": missing,
        "validation_failures": sum(not row["sound"] for row in rows),
        "protocol": "experiments/fixpoint_policy/UNIFIED640_PREREGISTRATION.md",
        "cache_note": "p00 was warmed by the pilot before the formal matrix; other seed7 goals are first-time matrix requests.",
        "arms": {},
        "rows": rows,
    }
    per_goal_rows = []
    for arm in ARMS:
        arm_rows = [row for row in rows if row["arm"] == arm]
        per_goal = per_goal_summary(rows, goals, arm)
        per_goal_rows.extend({"arm": arm, **item} for item in per_goal)
        if not per_goal:
            continue
        report["arms"][arm] = {
            "goals": len(per_goal),
            "runs": len(arm_rows),
            "goal_hit_rate": statistics.mean(row["hit"] for row in arm_rows),
            "soundness_rate": statistics.mean(row["sound"] for row in arm_rows),
            "macro": {key: statistics.mean(item[key] for item in per_goal)
                      for key in MEDIAN_KEYS},
            "batch_wall": sum(item["wall"] for item in per_goal),
            "batch_internal_wall": sum(item["internal_wall"] for item in per_goal),
            "observed_wall_total": sum(row["wall"] for row in arm_rows),
            "total_caps": sum(row["caps"] for row in arm_rows),
            "timeout_runs": sum(row["termination"] == "timeout" for row in arm_rows),
            "guide_calls": sum(row["guide_calls"] for row in arm_rows),
            "guide_fallbacks": sum(row["guide_fallbacks"] for row in arm_rows),
            "max_rss": max(row["rss"] for row in arm_rows),
            "identical_chain_goals": sum(item["chain_unique"] == 1
                                         for item in per_goal),
            "per_goal": per_goal,
        }
    truncated = [row for row in rows
                 if row["caps"] or row["termination"] != "goal"]
    if not missing and truncated:
        report["status"] = "complete_with_truncation"
    report["truncated_runs"] = len(truncated)
    report["seed7_new_goal"] = subset_comparison(rows, {7})
    report["repeated_requests"] = subset_comparison(rows, {13, 42})

    paired = []
    if all(arm in report["arms"] for arm in ARMS):
        baseline = {item["goal"]: item for item in report["arms"]["baseline"]["per_goal"]}
        guided = {item["goal"]: item for item in report["arms"]["jev"]["per_goal"]}
        for goal_id in sorted(set(baseline) & set(guided)):
            before, after = baseline[goal_id], guided[goal_id]
            paired.append({
                "goal": goal_id,
                "stratum": before["stratum"],
                "baseline_wall": before["wall"],
                "jev_wall": after["wall"],
                "wall_speedup": before["wall"] / after["wall"],
                "baseline_evaluations": before["evaluations"],
                "jev_evaluations": after["evaluations"],
                "evaluation_reduction_pct": 100 * (
                    1 - after["evaluations"] / before["evaluations"]),
                "guide_wall": after["guide_wall"],
                "guide_fallbacks": after["guide_fallbacks"],
            })
        if paired:
            base_macro = report["arms"]["baseline"]["macro"]
            jev_macro = report["arms"]["jev"]["macro"]
            report["paired"] = {
                "goals": len(paired),
                "wall_speedup": base_macro["wall"] / jev_macro["wall"],
                "evaluation_reduction_pct": 100 * (
                    1 - jev_macro["evaluations"] / base_macro["evaluations"]),
                "median_wall_speedup": statistics.median(
                    row["wall_speedup"] for row in paired),
                "median_evaluation_reduction_pct": statistics.median(
                    row["evaluation_reduction_pct"] for row in paired),
                "jev_wall_wins": sum(row["wall_speedup"] > 1 for row in paired),
                "jev_evaluation_wins": sum(
                    row["evaluation_reduction_pct"] > 0 for row in paired),
            }
    report["strata"] = {}
    for stratum in ("round1", "round2", "deep"):
        group = [row for row in paired if row["stratum"] == stratum]
        if not group:
            continue
        baseline_wall = statistics.mean(row["baseline_wall"] for row in group)
        jev_wall = statistics.mean(row["jev_wall"] for row in group)
        baseline_evaluations = statistics.mean(
            row["baseline_evaluations"] for row in group)
        jev_evaluations = statistics.mean(row["jev_evaluations"] for row in group)
        report["strata"][stratum] = {
            "goals": len(group),
            "baseline_wall": baseline_wall,
            "jev_wall": jev_wall,
            "wall_speedup": baseline_wall / jev_wall,
            "baseline_evaluations": baseline_evaluations,
            "jev_evaluations": jev_evaluations,
            "evaluation_reduction_pct": 100 * (
                1 - jev_evaluations / baseline_evaluations),
            "guide_wall": statistics.mean(row["guide_wall"] for row in group),
            "jev_wall_wins": sum(row["wall_speedup"] > 1 for row in group),
            "jev_evaluation_wins": sum(
                row["evaluation_reduction_pct"] > 0 for row in group),
        }
    report["paired_rows"] = paired

    fields = ["goal", "stratum", "arm", "seed", "status", "wall",
              "internal_wall", "hit", "termination", "sound", "branches",
              "matches", "ready", "evaluations", "derived", "caps", "rss",
              "guide_calls", "guide_wall", "guide_fallbacks",
              "guide_reachable", "guide_options", "guide_active",
              "guide_estimated_cost"]
    args.work.mkdir(parents=True, exist_ok=True)
    with (args.work / "rows.tsv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, delimiter="\t",
                                extrasaction="ignore", lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    with (args.work / "paired.tsv").open("w", newline="") as stream:
        fields = ["goal", "stratum", "baseline_wall", "jev_wall", "wall_speedup",
                  "baseline_evaluations", "jev_evaluations",
                  "evaluation_reduction_pct", "guide_wall", "guide_fallbacks"]
        writer = csv.DictWriter(stream, fieldnames=fields, delimiter="\t",
                                extrasaction="ignore", lineterminator="\n")
        writer.writeheader()
        writer.writerows(paired)
    (args.work / "summary.json").write_text(json.dumps(report, indent=2) + "\n")

    labels = dict(LABELS)
    if args.base_policy == "random":
        labels = {"baseline": "Random", "jev": "Random + local Jev"}
    elif args.base_policy == "adaptive":
        labels = {"baseline": "Deterministic",
                  "jev": "Adaptive numeric-Q + local Jev"}
    elif args.base_policy == "learned":
        name = "HDC-Q" if args.hdc_value_backend else "numeric-Q"
        labels = {"baseline": f"Learned {name}",
                  "jev": f"Learned {name} + local Jev"}
    lines = [f"# FB15K-237 local Jev guide（rules640，{len(goals)} goal）", "",
             f"状态：{report['status']}；观测 {len(rows)}/{report['planned_runs']}；"
             f"soundness failures={report['validation_failures']}。", "",
             f"两臂均使用当前 {args.engine.upper()} AdaDeduce；逐 goal 对 seeds {{7,13,42}} 取中位数后再宏平均。", "",
             "| 方法 | goal hit | soundness | 外部 wall(s) | 内部 wall(s) | 求值 | 派生 | guide(s) | fallback | cap | timeout | chain 一致 |",
             "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for arm in ARMS:
        if arm not in report["arms"]:
            continue
        item = report["arms"][arm]
        macro = item["macro"]
        lines.append(
            f"| {labels[arm]} | {item['goal_hit_rate']:.3f} | "
            f"{item['soundness_rate']:.3f} | {macro['wall']:.3f} | "
            f"{macro['internal_wall']:.3f} | {macro['evaluations']:.1f} | "
            f"{macro['derived']:.1f} | {macro['guide_wall']:.3f} | "
            f"{item['guide_fallbacks']} | {item['total_caps']} | "
            f"{item['timeout_runs']} | {item['identical_chain_goals']}/{item['goals']} |")
    if "paired" in report:
        paired_summary = report["paired"]
        lines += ["", "## 配对比较", "",
                  f"- 宏平均外部 wall 加速（baseline/Jev）：{paired_summary['wall_speedup']:.3f}x。",
                  f"- 宏平均求值减少：{paired_summary['evaluation_reduction_pct']:.3f}%。",
                  f"- Jev wall 胜出：{paired_summary['jev_wall_wins']}/{paired_summary['goals']} goals。",
                  f"- Jev 求值减少：{paired_summary['jev_evaluation_wins']}/{paired_summary['goals']} goals。", "",
                  "| 请求状态 | baseline wall(s) | Jev wall(s) | wall 加速 | 求值减少 | guide(s) |",
                  "|---|---:|---:|---:|---:|---:|"]
        for label, key in (("seed7：矩阵首轮（主矩阵 p00 已预热）", "seed7_new_goal"),
                           ("seed13/42：相同请求重复", "repeated_requests")):
            item = report[key]
            if len(item) >= 2:
                lines.append(
                    f"| {label} | {item['baseline']['wall']:.3f} | "
                    f"{item['jev']['wall']:.3f} | {item['wall_speedup']:.3f}x | "
                    f"{item['evaluation_reduction_pct']:.3f}% | "
                    f"{item['jev']['guide_wall']:.3f} |")
        lines += ["", "## 按 Chase round 分层", "",
                  "| 分层 | goals | baseline wall(s) | Jev wall(s) | wall 加速 | 求值减少 | wall 胜出 | 求值胜出 |",
                  "|---|---:|---:|---:|---:|---:|---:|---:|"]
        for stratum in ("round1", "round2", "deep"):
            item = report["strata"].get(stratum)
            if item is None:
                continue
            lines.append(
                f"| {stratum} | {item['goals']} | {item['baseline_wall']:.3f} | "
                f"{item['jev_wall']:.3f} | {item['wall_speedup']:.3f}x | "
                f"{item['evaluation_reduction_pct']:.3f}% | "
                f"{item['jev_wall_wins']}/{item['goals']} | "
                f"{item['jev_evaluation_wins']}/{item['goals']} |")
    lines += ["", "所有派生均与 rules640 的既有完整 Chase 闭包对账。"
              "seed13/42 是相同 goal 的重复模型请求，不能解释为未见 goal 的冷启动性能。", ""]
    (args.work / "summary.md").write_text("\n".join(lines))
    print(json.dumps({"status": report["status"], "observed": len(rows),
                      "validation_failures": report["validation_failures"],
                      "paired": report.get("paired")}, indent=2))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--work", type=Path, default=WORK)
    parser.add_argument("--goals", type=Path, default=base.TEST_GOALS)
    parser.add_argument("--engine", choices=("cpu", "gpu"), default="cpu")
    parser.add_argument("--phase", choices=("eval", "aggregate", "all"), default="all")
    parser.add_argument("--goal-id")
    parser.add_argument("--arm", choices=ARMS)
    parser.add_argument("--seed", type=int, choices=SEEDS)
    parser.add_argument("--base-policy",
                        choices=("deterministic", "random", "learned", "adaptive"),
                        default="deterministic")
    parser.add_argument("--value-model", type=Path)
    parser.add_argument("--hdc-value-backend",
                        choices=("original", "simd", "bit-lut"))
    parser.add_argument("--llm-base-url", default="http://127.0.0.1:30000")
    parser.add_argument("--llm-model", default="local-model")
    parser.add_argument("--llm-timeout-ms", type=int, default=120000)
    parser.add_argument("--guide-strength", type=float, default=1.0)
    parser.add_argument("--guide-max-options", type=int, default=255)
    parser.add_argument("--guide-min-cost", type=float, default=0.0)
    parser.add_argument("--llm-mock", action="store_true")
    args = parser.parse_args()
    args.work = args.work.resolve()
    args.goals = args.goals.resolve()
    model_policies = ("learned", "adaptive")
    if args.base_policy in model_policies and args.value_model is None:
        parser.error(f"--base-policy {args.base_policy} requires --value-model")
    if args.base_policy not in model_policies and args.value_model is not None:
        parser.error("--value-model requires --base-policy learned or adaptive")
    if args.base_policy != "learned" and args.hdc_value_backend is not None:
        parser.error("--hdc-value-backend requires --base-policy learned")
    if args.value_model is not None:
        args.value_model = args.value_model.resolve()
    if args.phase in ("eval", "all"):
        evaluate(args)
    if args.phase in ("aggregate", "all") and args.goal_id is None:
        aggregate(args)


if __name__ == "__main__":
    main()
