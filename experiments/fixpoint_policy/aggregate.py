#!/usr/bin/env python3
import argparse
import csv
import hashlib
import json
import re
import statistics
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / ".work_hdcv/fixpoint_policy"
RUNS = WORK / "runs_v1"
GT = ROOT / ".work_hdcv/full_policy_matrix/gt_closure512"
BNB_GT = ROOT / ".work_hdcv/of_chase30m/closure_edges.tsv"
DONE_RE = re.compile(r"\[gar_reason\] done:.* wall=([0-9.eE+-]+)s")


def summary(path):
    result = {}
    if not path.exists():
        return result
    for line in path.read_text().splitlines():
        if not line or line.startswith(" ") or ":" not in line:
            continue
        key, value = line.split(":", 1)
        result[key] = value.strip()
    return result


def number(values, key, default=0.0):
    try:
        return float(values.get(key, default))
    except ValueError:
        return default


def internal_wall(log):
    if not log.exists():
        return None
    matches = DONE_RE.findall(log.read_text(errors="replace"))
    return float(matches[-1]) if matches else None


def facts(path):
    result = set()
    with path.open() as stream:
        for row in csv.reader(stream, delimiter="\t"):
            if row and not row[0].startswith("#"):
                result.add((row[1], row[0], row[2]))
    return result


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def internal_time(run):
    value = internal_wall(run / "run.log")
    return "" if value is None else f"{value:.9f}"


def main():
    global WORK, RUNS
    parser = argparse.ArgumentParser()
    parser.add_argument("--work", type=Path, default=WORK,
                        help="Report directory containing the frozen goals.tsv.")
    parser.add_argument("--runs", type=Path, default=RUNS,
                        help="Run tree; may share Chase/BNB baselines by symlink.")
    args = parser.parse_args()
    WORK = args.work.resolve()
    RUNS = args.runs.resolve()
    with (WORK / "goals.tsv").open() as stream:
        goal_rows = list(csv.DictReader(stream, delimiter="\t"))
    goals = {row["goal_id"]: row for row in goal_rows}
    gt_facts = facts(GT / "closure_edges.tsv")
    rows = []
    fixpoint = {}
    planned = []
    interrupted = []

    for goal_id, goal in goals.items():
        arms = [("det", "rep1")]
        if goal["split"] == "test":
            arms += [("det", "rep2"), ("det", "rep3")]
            arms += [("random", f"seed{seed}") for seed in (7, 13, 42)]
        for arm, repetition in arms:
            run = RUNS / "fixpoint" / goal_id / arm / repetition
            planned.append(str(run.relative_to(RUNS)))
            if not (run / "resource.json").exists():
                if (run / "run.log").exists():
                    interrupted.append(str(run.relative_to(RUNS)))
                continue
            resource = json.loads((run / "resource.json").read_text())
            meta = summary(run / "summary.yaml")
            termination = meta.get("termination", "external_stop")
            settled = (resource["status"] == "completed" and
                       termination in ("goal", "fixpoint"))
            hit = meta.get("goal_derived") == "true"
            expected = int(goal["expected"])
            derived_path = run / "closure_edges.tsv"
            soundness = None
            derived_count = int(number(meta, "derived"))
            if derived_path.exists():
                produced = facts(derived_path)
                soundness = produced <= gt_facts
            row = {
                "workload": "fixpoint", "goal": goal_id, "arm": arm,
                "repetition": repetition, "stratum": goal["stratum"],
                "status": resource["status"], "wall_sec": resource["wall_sec"],
                "internal_wall_sec": internal_time(run), "expected": expected,
                "hit": int(hit), "correct": int(settled and hit == expected),
                "unresolved": int(not settled), "derived": derived_count,
                "evaluations": int(number(meta, "evaluate_events")),
                "expansions": "", "caps": int(number(meta, "candidate_caps")),
                "termination": termination,
                "peak_rss_gib": resource["peak_tree_rss_bytes"] / (1 << 30),
                "soundness": "" if soundness is None else int(soundness),
                "certified": "", "proof_cost": number(meta, "proof_cost"),
            }
            rows.append(row)
            fixpoint.setdefault((goal_id, arm), []).append(row)

    chase_rows = []
    reference_hash = sha256(GT / "closure_edges.tsv")
    for rep in range(1, 4):
        run = RUNS / "chase" / f"rep{rep}"
        planned.append(str(run.relative_to(RUNS)))
        if not (run / "resource.json").exists():
            if (run / "run.log").exists():
                interrupted.append(str(run.relative_to(RUNS)))
            continue
        resource = json.loads((run / "resource.json").read_text())
        meta = summary(run / "summary.yaml")
        closure_match = (resource["status"] == "completed" and
                         sha256(run / "closure_edges.tsv") == reference_hash)
        chase_rows.append({
            "wall": resource["wall_sec"],
            "rss": resource["peak_tree_rss_bytes"] / (1 << 30),
            "status": resource["status"], "closure_match": closure_match,
            "derived": int(number(meta, "derived_total")),
            "fixpoint": meta.get("fixpoint") == "true",
        })

    bnb_needed = set()
    bnb_facts_by_run = {}
    for goal_id in ("r2c_4", "r2c_8"):
        for arm in ("utility", "hdc", "numeric"):
            run = RUNS / "bnb" / goal_id / arm
            output = run / "closure_edges.tsv"
            bnb_facts_by_run[(goal_id, arm)] = facts(output) if output.exists() else set()
            bnb_needed |= bnb_facts_by_run[(goal_id, arm)]
    bnb_found = set()
    if bnb_needed:
        with BNB_GT.open() as stream:
            for row in csv.reader(stream, delimiter="\t"):
                if row and not row[0].startswith("#"):
                    fact = (row[1], row[0], row[2])
                    if fact in bnb_needed:
                        bnb_found.add(fact)

    bnb = {}
    for goal_id in ("r2c_4", "r2c_8"):
        for arm in ("utility", "hdc", "numeric"):
            run = RUNS / "bnb" / goal_id / arm
            planned.append(str(run.relative_to(RUNS)))
            if not (run / "resource.json").exists():
                if (run / "run.log").exists():
                    interrupted.append(str(run.relative_to(RUNS)))
                continue
            resource = json.loads((run / "resource.json").read_text())
            meta = summary(run / "summary.yaml")
            completed = (resource["status"] == "completed" and bool(meta) and
                         meta.get("termination") != "timeout")
            row = {
                "workload": "bnb", "goal": goal_id, "arm": arm,
                "repetition": "rep1", "stratum": "anchor",
                "status": resource["status"], "wall_sec": resource["wall_sec"],
                "internal_wall_sec": internal_time(run), "expected": 1,
                "hit": int(meta.get("goal_derived") == "true"),
                "correct": int(completed and meta.get("goal_derived") == "true"),
                "unresolved": int(meta.get("termination") == "timeout"),
                "derived": int(number(meta, "derived")), "evaluations": "",
                "expansions": int(number(meta, "expansions")), "caps": "",
                "termination": meta.get("termination", "external_stop"),
                "peak_rss_gib": resource["peak_tree_rss_bytes"] / (1 << 30),
                "soundness": (int(bnb_facts_by_run[(goal_id, arm)] <= bnb_found)
                              if (run / "closure_edges.tsv").exists() else ""),
                "certified": int(meta.get("certified_optimal") == "true"),
                "proof_cost": number(meta, "total_cost"),
            }
            rows.append(row)
            bnb[(goal_id, arm)] = row

    fieldnames = list(rows[0])
    with (WORK / "detail.tsv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames, delimiter="\t",
                                lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)

    positives = [row["goal_id"] for row in goal_rows if row["split"] == "test"]
    det_all = [row for row in rows if row["workload"] == "fixpoint" and
               row["arm"] == "det"]
    random_all = [row for row in rows if row["workload"] == "fixpoint" and
                  row["arm"] == "random"]
    chase_wall = statistics.median(row["wall"] for row in chase_rows)
    fx_summary = {}
    for arm in ("det", "random"):
        per_goal = []
        for goal_id in positives:
            group = fixpoint.get((goal_id, arm), [])
            if not group:
                continue
            per_goal.append({
                "goal": goal_id, "runs": len(group),
                "wall": statistics.median(row["wall_sec"] for row in group),
                "eval": statistics.median(row["evaluations"] for row in group),
                "derived": statistics.median(row["derived"] for row in group),
                "correct": all(row["correct"] for row in group),
                "unresolved": sum(row["unresolved"] for row in group),
                "sound": all(row["soundness"] == 1 for row in group),
                "caps": sum(row["caps"] for row in group),
            })
        selected = det_all if arm == "det" else random_all
        positive_rows = [row for row in selected if row["goal"] in positives]
        sound_rows = [row for row in selected if row["soundness"] != ""]
        fx_summary[arm] = {
            "goals": len(per_goal), "runs": len(selected),
            "accuracy": statistics.mean(row["correct"] for row in selected),
            "positive_hit_rate": statistics.mean(row["hit"] for row in positive_rows),
            "unresolved_runs": sum(row["unresolved"] for row in selected),
            "timeout_runs": sum(row["termination"] == "timeout" or
                                row["status"] == "timeout" for row in selected),
            "cap_runs": sum(row["termination"] == "candidate_cap" or
                            row["caps"] > 0 for row in selected),
            "soundness_rate": statistics.mean(row["soundness"] == 1 for row in sound_rows),
            "soundness_checked_runs": len(sound_rows),
            "macro_wall_sec": statistics.mean(row["wall"] for row in per_goal),
            "macro_evaluations": statistics.mean(row["eval"] for row in per_goal),
            "macro_derived": statistics.mean(row["derived"] for row in per_goal),
            "total_caps": sum(row["caps"] for row in selected),
            "max_rss_gib": max(row["peak_rss_gib"] for row in selected),
            "per_goal": per_goal,
        }

    missing = [name for name in planned if not (RUNS / name / "resource.json").exists()]
    resources = [json.loads((RUNS / name / "resource.json").read_text())
                 for name in planned if (RUNS / name / "resource.json").exists()]
    truncated = sum(row["workload"] == "fixpoint" and
                    (row["unresolved"] or row["caps"] > 0) for row in rows)
    validation_failures = sum(row["soundness"] == 0 for row in rows)
    validation_failures += sum(not row["closure_match"] for row in chase_rows)
    if validation_failures:
        status = "validation_failed"
    elif missing:
        status = "single_pass_probe_incomplete"
    elif truncated:
        status = "complete_with_truncation"
    else:
        status = "complete"
    result = {
        "status": status,
        "planned_runs": len(planned), "observed_runs": len(planned) - len(missing),
        "truncated_fixpoint_runs": truncated,
        "validation_failures": validation_failures,
        "missing_runs": missing, "interrupted_runs": interrupted,
        "chase": {
            "runs": chase_rows, "median_wall_sec": chase_wall,
            "amortized_wall_sec": chase_wall / len(positives),
            "derived": chase_rows[0]["derived"],
        },
        "fixpoint": fx_summary,
        "bnb": {f"{goal}/{arm}": row for (goal, arm), row in bnb.items()},
        "max_tree_rss_gib": max(row["peak_tree_rss_bytes"] for row in resources) / (1 << 30),
        "max_system_used_gib": max(row["peak_system_used_bytes"] for row in resources) / (1 << 30),
        "max_recorded_wall_sec": max(
            [row["wall_sec"] for row in rows] +
            [row["wall"] for row in chase_rows]),
    }
    (WORK / "summary.json").write_text(json.dumps(result, indent=2) + "\n")

    lines = ["# Fixpoint / Chase / BNB HDC 最小实验", "",
             "设置与限制见 `experiments/fixpoint_policy/PREREGISTRATION.md`。",
             f"状态：{result['status']}；已观测 {result['observed_runs']}/{len(planned)} 个计划 run。"
             "超时/cap 记为未解决，不能据此声称完备性。", "",
             "## Fixpoint vs Chase（12 个正例；det 另含 4 个负例）", "",
             "准确率将截断或限时内未解决记为未正确判定；宏平均只含已有结果的正例。", "",
             "| 方法 | 判定正确率 | 正例命中率 | 超时/cap run | soundness | 宏平均 wall(s) | 宏平均求值 | 宏平均派生 | 峰值RSS GiB |",
             "|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for arm, label in (("det", "Fixpoint-D"), ("random", "Fixpoint-R")):
        item = fx_summary[arm]
        lines.append(f"| {label} | {item['accuracy']:.3f} ({item['runs']} runs) | "
                     f"{item['positive_hit_rate']:.3f} | {item['timeout_runs']}/{item['cap_runs']} | "
                     f"{item['soundness_rate']:.3f} ({item['soundness_checked_runs']} checked) | "
                     f"{item['macro_wall_sec']:.3f} | {item['macro_evaluations']:.1f} | "
                     f"{item['macro_derived']:.1f} | {item['max_rss_gib']:.3f} |")
    lines.append(f"| Chase cold | GT | GT | {sum(row['status'] == 'timeout' for row in chase_rows)}/0 | "
                 f"closure {sum(row['closure_match'] for row in chase_rows)}/{len(chase_rows)} | "
                 f"{chase_wall:.3f} | - | {chase_rows[0]['derived']} | "
                 f"{max(row['rss'] for row in chase_rows):.3f} |")
    lines.append(f"| Chase amortized (T/12) | GT | GT | - | - | {chase_wall / 12:.3f} | - | - | - |")
    truncated_detail = ", ".join(
        f"{row['goal']}/{row['arm']}/{row['repetition']}={row['termination']}"
        f"(caps={row['caps']})"
        for row in rows if row["workload"] == "fixpoint" and
        (row["unresolved"] or row["caps"] > 0))
    lines += ["", f"Fixpoint cap 总数：det={fx_summary['det']['total_caps']}，"
              f"random={fx_summary['random']['total_caps']}。",
              f"截断明细：{truncated_detail or '无'}。", "",
              "## BNB value ordering（锚点复现）", "",
              "| goal | arm | wall(s) | expansions | hit | proof cost | certified | soundness | termination |",
              "|---|---|---:|---:|---:|---:|---:|---:|---|"]
    for goal_id in ("r2c_4", "r2c_8"):
        for arm in ("utility", "hdc", "numeric"):
            if (goal_id, arm) not in bnb:
                lines.append(f"| {goal_id} | {arm} | N/A | N/A | N/A | N/A | N/A | N/A | not run |")
                continue
            row = bnb[(goal_id, arm)]
            soundness = "N/A" if row["soundness"] == "" else row["soundness"]
            lines.append(f"| {goal_id} | {arm} | {row['wall_sec']:.3f} | "
                         f"{row['expansions']} | {row['hit']} | {row['proof_cost']:.0f} | "
                         f"{row['certified']} | {soundness} | {row['termination']} |")
    lines.append("")
    for goal_id in ("r2c_4", "r2c_8"):
        base = bnb.get((goal_id, "utility"))
        for arm in ("hdc", "numeric"):
            row = bnb.get((goal_id, arm))
            if base is None or row is None or not base["expansions"]:
                lines.append(f"- {goal_id} {arm} 展开减少：N/A。")
            else:
                reduction = 1 - row["expansions"] / base["expansions"]
                lines.append(f"- {goal_id} {arm} 展开减少：{reduction:.1%}。")
    lines += ["", "HDC 表只支持 BNB 分支排序结论，不支持 Fixpoint-HDC-Q；"
              "numeric 是同一学习目标的表示对照。", "",
              f"最大进程树 RSS：{result['max_tree_rss_gib']:.3f} GiB；"
              f"最大整机已用内存（含其他进程）：{result['max_system_used_gib']:.3f} GiB；"
              f"最长单 run：{result['max_recorded_wall_sec']:.3f} 秒。", ""]
    (WORK / "summary.md").write_text("\n".join(lines))
    print(json.dumps({"status": result["status"], "chase": result["chase"],
                      "fixpoint": fx_summary, "bnb": result["bnb"],
                      "max_tree_rss_gib": result["max_tree_rss_gib"],
                      "max_system_used_gib": result["max_system_used_gib"],
                      "max_recorded_wall_sec": result["max_recorded_wall_sec"]},
                     indent=2))


if __name__ == "__main__":
    main()
