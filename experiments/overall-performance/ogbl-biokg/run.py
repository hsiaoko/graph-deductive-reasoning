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


def choose(candidates, count, salt, max_per_relation, excluded_specs,
           excluded_relations, quota_per_stratum=False):
    groups = {}
    for raw in candidates:
        row = goal_row(raw, "candidate")
        if row["spec"] in excluded_specs or row["relation"] in excluded_relations:
            continue
        groups.setdefault(row["stratum"], []).append(row)
    selected = []
    used_relations = {}
    for stratum in ("round1", "round2", "deep"):
        if quota_per_stratum:
            used_relations = {}
        rows = groups[stratum]
        rows.sort(key=lambda row: stable(row["spec"], salt))
        chosen = 0
        for row in rows:
            if used_relations.get(row["relation"], 0) >= max_per_relation:
                continue
            selected.append(row)
            excluded_specs.add(row["spec"])
            used_relations[row["relation"]] = used_relations.get(row["relation"], 0) + 1
            chosen += 1
            if chosen == count:
                break
        if chosen != count:
            raise RuntimeError(f"cannot sample {count} {stratum} goals under frozen constraints")
    return selected


def freeze_goals():
    output = WORK / "goals.tsv"
    if output.exists():
        return
    closure = read_closure()
    relation = choose(closure, 4, "20261117-biokg-overall-relation-split-v1", 2,
                      set(), set(), quota_per_stratum=True)
    for row in relation:
        row["cohort"] = "relation-disjoint"
    heldout = {row["relation"] for row in relation}
    test_specs = {row["spec"] for row in relation}
    instance = choose(closure, 8, "20261117-biokg-overall-test-v1", 2,
                      test_specs, set())
    for row in instance:
        row["cohort"] = "instance-disjoint"
    test = instance + relation
    train = choose(closure, 8, "20261117-biokg-overall-train-v1", 2,
                   test_specs, heldout, quota_per_stratum=True)
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


def agent_command(goal, output):
    return [
        "./build/bin/gar_reason", "--mode", "agent", "--engine", "cpu",
        "--csr", str(CSR), "--rules", str(WORK / "rules"),
        "--goal", goal["spec"], "--out", str(output), "--print", "0",
        "--star-order", "goal", "--scorer", "exact", "--budget", "1000000000",
        "--wall-limit", "900", "--max-candidates-per-rule", "100000",
        "--fair-interval", "0", "--policy-seed", str(SEED),
    ]


def train_models():
    with (WORK / "train_goals.tsv").open() as stream:
        goals = list(csv.DictReader(stream, delimiter="\t"))
    for goal in goals:
        for policy in ("deterministic", "random"):
            name = f"traces/{goal['goal_id']}/{policy}"
            output = WORK / "runs" / name
            command = agent_command(goal, output) + [
                "--policy", policy, "--q-trace", str(output / "q_trace.tsv"),
            ]
            run(name, command)
    if not (WORK / "models/q_numeric.meta").exists():
        subprocess.run([
            "python3", str(ROOT / "experiments/fixpoint_policy/train_fixpoint_q.py"),
            "--traces", str(WORK / "runs/traces"),
            "--out", str(WORK / "models"),
            "--test-goals", str(WORK / "goals.tsv"),
        ], cwd=ROOT, check=True, env=dict(os.environ, PYTHONHASHSEED="0"))
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
            base.gate()
            name = f"{goal['goal_id']}/{method}/seed7"
            output = WORK / "runs" / name
            if method == "DepOnly":
                command = [
                    "./build/bin/gar_reason", "--mode", "dep-only",
                    "--csr", str(CSR), "--rules", str(WORK / "rules"),
                    "--goal", goal["spec"],
                    "--out", str(output), "--max-rounds", "100",
                    "--max-derivations", "1000000000", "--max-matches", "100000",
                    "--no-matches", "--print", "0",
                ]
            else:
                command = agent_command(goal, output)
                if method == "Deterministic":
                    command += ["--policy", "deterministic"]
                elif method == "Random":
                    command += ["--policy", "random"]
                elif method == "Learned":
                    command += ["--policy", "learned", "--value-model",
                                str(WORK / "models/q_numeric.meta")]
                else:
                    command += [
                        "--policy", "deterministic", "--proof-star-model",
                        str(WORK / "proof_efficiency_model/model/proof_star.meta"),
                        "--proof-star-weight", "0.5", "--proof-star-min-cost", "0",
                    ]
            run(name, command)


def read_run(goal, method):
    run_dir = WORK / "runs" / goal["goal_id"] / method / "seed7"
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
        "cohort": goal["cohort"], "method": method,
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
    rows = [read_run(goal, method) for goal in goals for method in METHODS]
    arms = {}
    for method in METHODS:
        selected = [row for row in rows if row["method"] == method]
        evaluated = [row["evaluations"] for row in selected
                     if row["evaluations"] is not None]
        arms[method] = {
            "runs": len(selected),
            "wall": statistics.mean(row["wall"] for row in selected),
            "derived": statistics.mean(row["derived"] for row in selected),
            "evaluations": statistics.mean(evaluated) if evaluated else None,
            "hit_rate": statistics.mean(row["hit"] for row in selected),
            "soundness_rate": statistics.mean(row["sound"] for row in selected),
            "timeouts": sum(row["termination"] == "timeout" for row in selected),
            "caps": sum(row["caps"] for row in selected),
            "max_rss": max(row["rss"] for row in selected),
            "wall_speedup_vs_deterministic": None,
        }
    for method in METHODS:
        arms[method]["wall_speedup_vs_deterministic"] = (
            arms["Deterministic"]["wall"] / arms[method]["wall"])
    report = {
        "status": "complete" if len(rows) == 180 else "incomplete",
        "runs": len(rows), "planned_runs": 180, "seed": 7,
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
        "# OGBL-BioKG overall-performance（CPU，36 goal，seed 7）", "",
        f"状态：{report['status']}；观测 {len(rows)}/180；"
        f"soundness failures={report['validation_failures']}。", "",
        "| 方法 | wall(s) | 相对 D 加速 | 求值 | 派生 | hit | soundness | timeout | cap |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for method in METHODS:
        arm = arms[method]
        evaluations = "N/A" if arm["evaluations"] is None else f"{arm['evaluations']:.1f}"
        lines.append(
            f"| {method} | {arm['wall']:.3f} | "
            f"{arm['wall_speedup_vs_deterministic']:.3f}x | {evaluations} | "
            f"{arm['derived']:.1f} | {arm['hit_rate']:.3f} | "
            f"{arm['soundness_rate']:.3f} | {arm['timeouts']} | {arm['caps']} |")
    lines += ["", "逐 goal 数据见 rows.tsv；所有派生均与冻结规则集的完整 Chase 闭包对账。", ""]
    (WORK / "summary.md").write_text("\n".join(lines))
    print(json.dumps({"status": report["status"], "runs": len(rows),
                      "validation_failures": report["validation_failures"],
                      "arms": arms}, indent=2))


def main():
    global WORK, STATUS, RULE_QUOTA
    parser = argparse.ArgumentParser()
    parser.add_argument("--phase", choices=("all", "prepare", "train", "evaluate",
                                            "aggregate"), default="all")
    parser.add_argument("--work", type=Path, default=WORK)
    parser.add_argument("--rule-quota", type=int, default=RULE_QUOTA)
    args = parser.parse_args()
    if args.rule_quota <= 0:
        parser.error("--rule-quota must be positive")
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
