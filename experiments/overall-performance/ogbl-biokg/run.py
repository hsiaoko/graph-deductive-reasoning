#!/usr/bin/env python3
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
import time

import yaml

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "experiments/fixpoint_policy"))
import run_fixpoint_q as base
from aggregate import facts, internal_time, number, summary

DATASET = Path("/ssd_data/zhuxk/graphs/KG/ogbl-biokg")
CSR = DATASET / "immutable_csr"
RAW_RULES = DATASET / "rules"
WORK = ROOT / ".work_hdcv/overall-performance/ogbl-biokg"
STATUS = WORK / "status.json"
SEED = 7
SEEDS = (7, 13, 42)
# Per-stratum instance-disjoint test quotas (count, max_per_relation). The
# relation-disjoint cohort adds 4 round1 + 4 round2 = 8 goals on top. Default
# 8+8+12=28 instance + 8 relation = 36 goals; --instance-quota 14,14,14 gives
# 42 instance + 8 relation = 50 goals (deep cap 7 covers its 2 relations).
INSTANCE_QUOTAS = {"round1": (8, 2), "round2": (8, 2), "deep": (12, 6)}
RULE_QUOTA = 32
METHODS = ("DepOnly", "Deterministic", "Random", "Learned", "AdaDeduceStar")


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def stable(key, salt):
    return hashlib.sha256(f"{salt}|{key}".encode()).hexdigest()


def write_status(state, **extra):
    WORK.mkdir(parents=True, exist_ok=True)
    payload = {"state": state,
               "updated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
               **extra}
    STATUS.write_text(json.dumps(payload, indent=2) + "\n")


def run(name, command, timeout=1000):
    base.run(name, command, WORK / "runs")


def rule_record(path):
    data = yaml.safe_load((path / "rule.yaml").read_text())["rule"]
    consequent = data.get("consequent", {})
    if consequent.get("kind", "edge") != "edge":
        return None
    relation = consequent.get("label")
    if not isinstance(relation, str):
        return None
    body = data["name"].split(" → ")[0]
    body_relations = [rel[:-3] if rel.endswith("^-1") else rel
                      for rel in re.findall(r"([^\s]+)\(x\d+", body)]
    body_edges = body.count("(x")
    if body_edges < 1:
        return None
    predicates = data.get("predicates", [])
    if any(pred.get("kind") == "ml" for pred in predicates):
        return None
    nodes = data.get("pattern", {}).get("nodes", [])
    signature = tuple(node.get("label", "*") for node in nodes)
    quality = data.get("quality", {})
    return {
        "path": path, "id": path.name, "relation": relation,
        "body_edges": body_edges, "signature": signature,
        "body_relations": body_relations,
        "confidence": float(quality.get("confidence", 0)),
        "support": float(quality.get("support", 0)),
    }


def annotate_dependency_structure(records):
    adjacency = {}
    for record in records:
        adjacency.setdefault(record["relation"], set())
        for body in record["body_relations"]:
            adjacency.setdefault(body, set()).add(record["relation"])
    index = 0
    indices, low, stack, on_stack = {}, {}, [], set()
    components = []

    def visit(node):
        nonlocal index
        indices[node] = low[node] = index
        index += 1
        stack.append(node)
        on_stack.add(node)
        for neighbor in adjacency[node]:
            if neighbor not in indices:
                visit(neighbor)
                low[node] = min(low[node], low[neighbor])
            elif neighbor in on_stack:
                low[node] = min(low[node], indices[neighbor])
        if low[node] != indices[node]:
            return
        component = []
        while True:
            member = stack.pop()
            on_stack.remove(member)
            component.append(member)
            if member == node:
                break
        components.append(component)

    for node in adjacency:
        if node not in indices:
            visit(node)
    component_of = {node: i for i, component in enumerate(components)
                    for node in component}
    predecessors = [set() for _ in components]
    for record in records:
        head = component_of[record["relation"]]
        for body in record["body_relations"]:
            source = component_of[body]
            if source != head:
                predecessors[head].add(source)
    depth_cache = {}

    def depth(component):
        if component not in depth_cache:
            depth_cache[component] = 1 + max(
                (depth(pred) for pred in predecessors[component]), default=0)
        return depth_cache[component]

    for record in records:
        component = component_of[record["relation"]]
        node = record["relation"]
        record["dg_depth"] = depth(component)
        record["recursive"] = (len(components[component]) > 1 or
                               node in adjacency[node])


def freeze_rules():
    output = WORK / "rules"
    manifest_path = WORK / "rules_manifest.json"
    if manifest_path.exists():
        return
    groups = {}
    skipped = 0
    for path in sorted(RAW_RULES.iterdir()):
        if not path.is_dir() or not (path / "rule.yaml").exists():
            continue
        record = rule_record(path)
        if record is None:
            skipped += 1
            continue
        groups.setdefault(record["relation"], []).append(record)
    annotate_dependency_structure([row for group in groups.values()
                                   for row in group])
    selected = []
    for relation in sorted(groups):
        structures = {}
        for record in groups[relation]:
            key = (record["body_edges"], record["signature"],
                   record["recursive"], record["dg_depth"])
            structures.setdefault(key, []).append(record)
        ordered = []
        for key in sorted(structures, key=str):
            structures[key].sort(key=lambda row: stable(
                row["id"], "20261117-biokg-overall-rules-v2"))
        while any(structures.values()):
            for key in sorted(structures, key=str):
                if structures[key]:
                    ordered.append(structures[key].pop(0))
        selected.extend(ordered[:RULE_QUOTA])
    output.mkdir(parents=True, exist_ok=True)
    for record in selected:
        shutil.copytree(record["path"], output / record["id"])
    with (output / "rule_ids.txt").open("w") as stream:
        for record in selected:
            stream.write(record["id"] + "\n")
    manifest = {
        "raw_rules": len(groups), "raw_rule_directories": 56601,
        "selected_rules": len(selected), "budget": 51 * RULE_QUOTA,
        "per_relation_quota": RULE_QUOTA, "skipped": skipped,
        "relations_with_rules": len(groups),
        "selected_per_relation": {
            relation: sum(row["relation"] == relation for row in selected)
            for relation in sorted(groups)},
        "body_length_counts": {
            str(length): sum(row["body_edges"] == length for row in selected)
            for length in sorted({row["body_edges"] for row in selected})},
        "recursive_rules": sum(row["recursive"] for row in selected),
        "dg_depth_counts": {
            str(depth): sum(row["dg_depth"] == depth for row in selected)
            for depth in sorted({row["dg_depth"] for row in selected})},
        "salt": "20261117-biokg-overall-rules-v2",
    }
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")


def run_gt():
    output = WORK / "ground_truth/chase"
    result = output / "resource.json"
    if not result.exists():
        output.mkdir(parents=True, exist_ok=True)
        command = [
            "./build/bin/gar_reason", "--mode", "chase", "--engine", "cpu",
            "--csr", str(CSR), "--rules", str(WORK / "rules"),
            "--out", str(output), "--max-rounds", "100",
            "--max-derivations", "1000000000", "--max-matches", "1000000000",
            "--no-matches", "--print", "0",
        ]
        bounded = [
            "python3", str(base.BOUNDED), "--log", str(output / "run.log"),
            "--result", str(result), "--timeout-sec", "7200",
            "--memory-gb", "256", "--system-memory-gb", "256", "--", *command,
        ]
        subprocess.run(bounded, cwd=ROOT, check=False)
    resource = json.loads(result.read_text())
    meta = summary(output / "summary.yaml")
    if resource["returncode"] != 0 or meta.get("fixpoint") != "true":
        raise RuntimeError("BioKG ground truth did not reach a complete fixpoint")


def read_closure():
    path = WORK / "ground_truth/chase/closure_edges.tsv"
    with path.open() as stream:
        return list(csv.DictReader(stream, delimiter="\t"))


def goal_row(raw, cohort):
    relation = raw["relation"]
    src, dst = raw["# src"], raw["dst"]
    return {"stratum": "round1" if raw["round"] == "1" else
            ("round2" if raw["round"] == "2" else "deep"),
            "relation": relation, "src": src, "dst": dst,
            "spec": f"{relation}({src},{dst})", "round": raw["round"],
            "cohort": cohort}


def choose(candidates, quotas, salt, excluded_specs, excluded_relations):
    # quotas maps stratum -> (count, max_per_relation). Per-stratum caps let the
    # sparse BioKG deep stratum (only 2 relations) use a relaxed cap while the
    # dense round1/round2 strata keep tight diversity caps.
    groups = {}
    for raw in candidates:
        row = goal_row(raw, "candidate")
        if row["spec"] in excluded_specs or row["relation"] in excluded_relations:
            continue
        groups.setdefault(row["stratum"], []).append(row)
    selected = []
    for stratum, (count, cap) in quotas.items():
        rows = groups.get(stratum, [])
        rows.sort(key=lambda row: stable(row["spec"], salt))
        used_relations = {}
        chosen = 0
        for row in rows:
            if used_relations.get(row["relation"], 0) >= cap:
                continue
            selected.append(row)
            excluded_specs.add(row["spec"])
            used_relations[row["relation"]] = used_relations.get(row["relation"], 0) + 1
            chosen += 1
            if chosen == count:
                break
        if chosen != count:
            raise RuntimeError(
                f"cannot sample {count} {stratum} goals (cap {cap})")
    return selected


def freeze_goals():
    output = WORK / "goals.tsv"
    if output.exists():
        return
    closure = read_closure()
    # BioKG's deep stratum has only 2 relations (protein-protein_binding /
    # reaction), which also occur in round1/round2. A relation-disjoint deep
    # cohort is infeasible, and holding these relations out would leave no deep
    # relation for train. So the relation-disjoint cohort spans round1/round2
    # relations EXCLUDING the deep ones; deep goals are all instance-disjoint
    # with a relaxed per-relation cap.
    deep_relations = {goal_row(raw, "candidate")["relation"] for raw in closure
                      if goal_row(raw, "candidate")["stratum"] == "deep"}
    relation = choose(closure, {"round1": (4, 2), "round2": (4, 2)},
                      "20261117-biokg-overall-relation-split-v1", set(),
                      deep_relations)
    for row in relation:
        row["cohort"] = "relation-disjoint"
    heldout = {row["relation"] for row in relation}
    test_specs = {row["spec"] for row in relation}
    instance = choose(closure, INSTANCE_QUOTAS,
                      "20261117-biokg-overall-test-v1", test_specs, set())
    for row in instance:
        row["cohort"] = "instance-disjoint"
    test = instance + relation
    train = choose(closure,
                   {"round1": (8, 2), "round2": (8, 2), "deep": (8, 4)},
                   "20261117-biokg-overall-train-v1", test_specs, heldout)
    for row in train:
        row["cohort"] = "train"
    for index, row in enumerate(test):
        row["goal_id"] = f"p{index:02d}"
    for index, row in enumerate(train):
        row["goal_id"] = f"t{index:02d}"
    fields = ["goal_id", "stratum", "cohort", "relation", "src", "dst",
              "spec", "round"]
    for name, rows in (("goals.tsv", test), ("train_goals.tsv", train)):
        with (WORK / name).open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=fields, delimiter="\t",
                                    lineterminator="\n")
            writer.writeheader()
            writer.writerows(rows)
    manifest = {
        "test_goals": len(test), "train_goals": len(train),
        "test_cohorts": {cohort: sum(row["cohort"] == cohort for row in test)
                         for cohort in ("instance-disjoint", "relation-disjoint")},
        "heldout_relations": sorted(heldout),
        "goals_sha256": sha256(output),
        "train_goals_sha256": sha256(WORK / "train_goals.tsv"),
        "ground_truth_sha256": sha256(WORK / "ground_truth/chase/closure_edges.tsv"),
    }
    (WORK / "goals_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


def agent_command(goal, output, seed=SEED):
    return [
        "./build/bin/gar_reason", "--mode", "agent", "--engine", "cpu",
        "--csr", str(CSR), "--rules", str(WORK / "rules"),
        "--goal", goal["spec"], "--out", str(output), "--print", "0",
        "--star-order", "goal", "--scorer", "exact", "--budget", "1000000000",
        "--wall-limit", "900", "--max-candidates-per-rule", "10000000",
        "--fair-interval", "0", "--policy-seed", str(seed),
        "--hdc-dim", "128",
    ]


def train_models():
    with (WORK / "train_goals.tsv").open() as stream:
        goals = list(csv.DictReader(stream, delimiter="\t"))
    for goal in goals:
        for policy in ("deterministic", "random"):
            name = f"traces/{goal['goal_id']}/{policy}"
            output = WORK / "runs" / name
            command = agent_command(goal, output) + [
                "--policy", policy,
                "--policy-trace", str(output / "policy_trace.json"),
                "--policy-trace-negatives", "8",
                "--decision-trace", str(output / "decision_trace.tsv"),
            ]
            run(name, command)
    if not (WORK / "models/relevance.meta").exists():
        subprocess.run([
            "./build/bin/gar_train_policy",
            "--csr", str(CSR), "--rules", str(WORK / "rules"),
            "--traces", str(WORK / "runs/traces"),
            "--out", str(WORK / "models/relevance.meta"),
            "--test-goals", str(WORK / "goals.tsv"),
            "--epochs", "8", "--batch-size", "32", "--seed", "42",
        ], cwd=ROOT, check=True)
    if not (WORK / "proof_efficiency_model/model/proof_star.meta").exists():
        subprocess.run([
            "python3", str(ROOT / "experiments/fixpoint_policy/validate_proof_dag.py"),
            "--traces", str(WORK / "runs/traces"), "--rules", str(WORK / "rules"),
            "--work", str(WORK / "proof_efficiency_model"),
            "--work-weights", "0.1",
        ], cwd=ROOT, check=True, env=dict(os.environ, PYTHONHASHSEED="0"))


def evaluate():
    with (WORK / "goals.tsv").open() as stream:
        goals = list(csv.DictReader(stream, delimiter="\t"))
    for goal in goals:
        for method in METHODS:
            for seed in SEEDS:
                base.gate()
                name = f"{goal['goal_id']}/{method}/seed{seed}"
                output = WORK / "runs" / name
                if method == "DepOnly":
                    command = [
                        "./build/bin/gar_reason", "--mode", "dep-only",
                        "--csr", str(CSR), "--rules", str(WORK / "rules"),
                        "--goal", goal["spec"],
                        "--out", str(output), "--max-rounds", "100",
                        "--max-derivations", "1000000000",
                        "--max-matches", "1000000000",
                        "--no-matches", "--print", "0",
                    ]
                else:
                    command = agent_command(goal, output, seed)
                    if method == "Deterministic":
                        command += ["--policy", "deterministic"]
                    elif method == "Random":
                        command += ["--policy", "random"]
                    elif method == "Learned":
                        command += ["--policy", "learned", "--relevance-model",
                                    str(WORK / "models/relevance.meta")]
                    else:
                        command += [
                            "--policy", "deterministic", "--proof-star-model",
                            str(WORK / "proof_efficiency_model/model/proof_star.meta"),
                            "--proof-star-weight", "0.5", "--proof-star-min-cost", "0",
                        ]
                run(name, command)


def read_run(goal, method, seed):
    run_dir = WORK / "runs" / goal["goal_id"] / method / f"seed{seed}"
    resource = json.loads((run_dir / "resource.json").read_text())
    meta = summary(run_dir / "summary.yaml")
    if method == "DepOnly":
        hit = int(meta.get("goal_hit") == "true")
        derived = int(number(meta, "derived_total"))
        evaluations = None
        internal = None
        termination = "goal" if meta.get("stopped_early") == "true" else "fixpoint"
        caps = (run_dir / "summary.yaml").read_text().count("match_capped: true")
    else:
        hit = int(meta.get("goal_derived") == "true")
        derived = int(number(meta, "derived"))
        evaluations = int(number(meta, "evaluate_events"))
        internal = float(internal_time(run_dir))
        termination = meta.get("termination")
        caps = int(number(meta, "candidate_caps"))
    return {
        "goal": goal["goal_id"], "stratum": goal["stratum"],
        "cohort": goal["cohort"], "method": method, "seed": seed,
        "wall": resource["wall_sec"], "internal_wall": internal,
        "hit": hit, "termination": termination,
        "sound": int(facts(run_dir / "closure_edges.tsv") <= GT_FACTS),
        "evaluations": evaluations, "derived": derived, "caps": caps,
        "rss": resource["peak_tree_rss_bytes"] / (1 << 30),
        "returncode": resource["returncode"],
    }


def aggregate():
    global GT_FACTS
    GT_FACTS = facts(WORK / "ground_truth/chase/closure_edges.tsv")
    with (WORK / "goals.tsv").open() as stream:
        goals = list(csv.DictReader(stream, delimiter="\t"))
    rows = [read_run(goal, method, seed) for goal in goals
            for method in METHODS for seed in SEEDS]
    # UNIFIED640 protocol: per-goal median across the 3 seeds, then macro mean
    # (and sum for the "total time" big-table column).
    per_goal = {}
    for row in rows:
        per_goal.setdefault((row["goal"], row["method"]), []).append(row)
    goal_medians = []
    for (gid, method), grp in per_goal.items():
        evs = [r["evaluations"] for r in grp if r["evaluations"] is not None]
        goal_medians.append({
            "goal": gid, "method": method,
            "wall": statistics.median(r["wall"] for r in grp),
            "derived": statistics.median(r["derived"] for r in grp),
            "evaluations": statistics.median(evs) if evs else None,
            "hit": min(r["hit"] for r in grp),
            "sound": min(r["sound"] for r in grp),
            "termination": grp[0]["termination"],
            "caps": sum(r["caps"] for r in grp),
            "rss": max(r["rss"] for r in grp),
        })
    arms = {}
    for method in METHODS:
        selected = [g for g in goal_medians if g["method"] == method]
        evaluated = [g["evaluations"] for g in selected
                     if g["evaluations"] is not None]
        arms[method] = {
            "runs": len(selected) * len(SEEDS), "goals": len(selected),
            "wall": statistics.mean(g["wall"] for g in selected),
            "total_wall": sum(g["wall"] for g in selected),
            "derived": statistics.mean(g["derived"] for g in selected),
            "total_derived": sum(g["derived"] for g in selected),
            "evaluations": statistics.mean(evaluated) if evaluated else None,
            "hit_rate": statistics.mean(g["hit"] for g in selected),
            "soundness_rate": statistics.mean(g["sound"] for g in selected),
            "timeouts": sum(g["termination"] == "timeout" for g in selected),
            "caps": sum(g["caps"] for g in selected),
            "max_rss": max(g["rss"] for g in selected),
            "wall_speedup_vs_deterministic": None,
        }
    for method in METHODS:
        arms[method]["wall_speedup_vs_deterministic"] = (
            arms["Deterministic"]["wall"] / arms[method]["wall"])
    planned = len(goals) * len(METHODS) * len(SEEDS)
    report = {
        "status": "complete" if len(rows) == planned else "incomplete",
        "runs": len(rows), "planned_runs": planned, "seeds": list(SEEDS),
        "validation_failures": sum(not row["sound"] for row in rows),
        "arms": arms, "rows": rows,
        "rules": json.loads((WORK / "rules_manifest.json").read_text()),
        "goals": json.loads((WORK / "goals_manifest.json").read_text()),
    }
    (WORK / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    fields = list(rows[0])
    with (WORK / "rows.tsv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, delimiter="\t",
                                lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    lines = [
        "# OGBL-BioKG overall-performance（CPU，36 goal × 3 seeds，UNIFIED640）", "",
        f"状态：{report['status']}；观测 {len(rows)}/{planned}；"
        f"soundness failures={report['validation_failures']}。", "",
        "逐 goal 取 3 seed 中位数；总时间/总派生 = 36 goal 中位数之和。", "",
        "| 方法 | 总时间(s) | 均wall(s) | 相对 D | 总派生 | 均派生 | hit | sound |",
        "|---|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for method in METHODS:
        arm = arms[method]
        lines.append(
            f"| {method} | {arm['total_wall']:.2f} | {arm['wall']:.3f} | "
            f"{arm['wall_speedup_vs_deterministic']:.3f}x | {arm['total_derived']:.0f} | "
            f"{arm['derived']:.1f} | {arm['hit_rate']:.3f} | "
            f"{arm['soundness_rate']:.3f} |")
    lines += ["", "逐 goal 数据见 rows.tsv；所有派生均与冻结规则集的完整 Chase 闭包对账。", ""]
    (WORK / "summary.md").write_text("\n".join(lines))
    print(json.dumps({"status": report["status"], "runs": len(rows),
                      "validation_failures": report["validation_failures"],
                      "arms": arms}, indent=2))


def main():
    global WORK, STATUS, RULE_QUOTA, INSTANCE_QUOTAS
    parser = argparse.ArgumentParser()
    parser.add_argument("--phase", choices=("all", "prepare", "train", "evaluate",
                                            "aggregate"), default="all")
    parser.add_argument("--work", type=Path, default=WORK)
    parser.add_argument("--rule-quota", type=int, default=RULE_QUOTA)
    parser.add_argument("--instance-quota", default=None,
                        help="per-stratum instance test counts r1,r2,deep "
                             "(e.g. 14,14,14 for 50 goals); deep cap auto")
    args = parser.parse_args()
    if args.rule_quota <= 0:
        parser.error("--rule-quota must be positive")
    if args.instance_quota:
        r1, r2, deep = (int(x) for x in args.instance_quota.split(","))
        INSTANCE_QUOTAS = {"round1": (r1, 2), "round2": (r2, 2),
                           "deep": (deep, max(2, (deep + 1) // 2))}
    WORK = args.work.resolve()
    STATUS = WORK / "status.json"
    RULE_QUOTA = args.rule_quota
    try:
        if args.phase in ("all", "prepare"):
            write_status("freeze_rules")
            freeze_rules()
            write_status("ground_truth")
            run_gt()
            write_status("freeze_goals")
            freeze_goals()
        if args.phase in ("all", "train"):
            write_status("train")
            train_models()
        if args.phase in ("all", "evaluate"):
            write_status("evaluate")
            evaluate()
        if args.phase in ("all", "aggregate"):
            write_status("aggregate")
            aggregate()
        write_status("complete")
    except Exception as error:
        write_status("failed", error=str(error))
        raise


if __name__ == "__main__":
    main()
