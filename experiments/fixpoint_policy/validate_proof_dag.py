#!/usr/bin/env python3
import argparse
import csv
import json
import re
from collections import Counter
from pathlib import Path

import numpy as np
import yaml

ROOT = Path(__file__).resolve().parents[2]
TRACES = ROOT / ".work_hdcv/fixpoint_q640/traces"
RULES = ROOT / ".work_hdcv/fixpoint_rules640/rules640"
WORK = ROOT / ".work_hdcv/proof_dag_validation"
FEATURES = ["price", "pattern_edges", "log_support", "relfam", "conf", "dp0"]
CONJ = [(1, 2), (2, 3), (2, 4), (1, 3), (2, 5)]
EPSILON = 1e-6
ATOM = re.compile(
    r"([^\s]+)\(x(\d+)(?::[^,)]*)?,x(\d+)(?::[^)]*)?\)")
FACT = re.compile(r"^(.*)\(([^,()]*),([^,()]*)\)$")


def normalize_fact(relation, src, dst):
    if relation.endswith("^-1"):
        return relation[:-3], dst, src
    return relation, src, dst


def parse_fact(text):
    match = FACT.match(text)
    if match is None:
        raise ValueError(f"bad fact: {text}")
    return normalize_fact(*match.groups())


def parse_rule(path):
    data = yaml.safe_load(path.read_text())["rule"]
    name = data["name"]
    body, consequent = name.split(" → ")
    edges = []
    for relation, src, dst in ATOM.findall(body):
        edges.append((relation, int(src), int(dst)))
    head = ATOM.findall(consequent)
    if not edges or len(head) != 1:
        raise ValueError(f"cannot parse rule topology: {name}")
    return edges


def select_trace(goal_dir):
    for policy in ("deterministic", "random"):
        run = goal_dir / policy
        if (run / "chain.tsv").exists():
            meta = (run / "summary.yaml").read_text()
            if "goal_derived: true" in meta:
                return run
    return None


def extract_proof(run, rule_edges):
    fact_step = {}
    steps = {}
    with (run / "chain.tsv").open() as stream:
        for row in csv.DictReader(stream, delimiter="\t"):
            step = int(row["# step"])
            embedding = row["embedding"].split(",")
            fact = parse_fact(row["derived_fact"])
            if fact in fact_step:
                raise RuntimeError(f"duplicate derived fact in {run}: {fact}")
            fact_step[fact] = step
            steps[step] = (row["rule_id"], embedding)
    command = json.loads((run / "resource.json").read_text())["command"]
    goal = parse_fact(command[command.index("--goal") + 1])
    if goal not in fact_step:
        raise RuntimeError(f"goal not found in chain: {goal}")

    proof_steps = set()
    depth_cache = {}

    def visit(fact):
        if fact not in fact_step:
            return 0
        step = fact_step[fact]
        if step in depth_cache:
            return depth_cache[step]
        rule_id, embedding = steps[step]
        proof_steps.add(step)
        depth = 1
        for relation, src, dst in rule_edges[rule_id]:
            premise = normalize_fact(relation, embedding[src], embedding[dst])
            depth = max(depth, 1 + visit(premise))
        depth_cache[step] = depth
        return depth

    depth = visit(goal)
    total_by_rule = Counter(rule_id for rule_id, _ in steps.values())
    proof_by_rule = Counter(steps[step][0] for step in proof_steps)
    return proof_steps, depth, len(steps), total_by_rule, proof_by_rule


def static_features(run, rule_ids):
    result = {}
    with (run / "decision_trace.tsv").open() as stream:
        reader = csv.DictReader(stream, delimiter="\t")
        for row in reader:
            rule = int(row["rule"])
            if rule not in result:
                result[rule] = [float(row[name]) for name in FEATURES]
    return {rule_ids[rule]: values for rule, values in result.items()}


def expand_features(z):
    return np.hstack([z] + [(z[:, a] * z[:, b])[:, None] for a, b in CONJ])


def fit_components(x, labels, total_facts, groups, lam=10.0):
    mean = x.mean(axis=0)
    std = x.std(axis=0)
    std[std == 0] = 1.0
    z = expand_features((x - mean) / std)
    differences = []
    for group in sorted(set(groups)):
        selected = groups == group
        positive = z[selected & (labels == 1)]
        negative = z[selected & (labels == 0)]
        if len(positive) and len(negative):
            differences.extend((p[None, :] - negative) for p in positive)
    d = np.vstack(differences)
    proof_weights = np.linalg.solve(d.T @ d + lam * np.eye(d.shape[1]),
                                    d.T @ np.ones(len(d)))
    proof_weights = np.asarray([*proof_weights, 0.0])

    design = np.hstack([z, np.ones((len(z), 1))])
    penalty = lam * np.eye(design.shape[1])
    penalty[-1, -1] = 0.0
    work_weights = np.linalg.solve(design.T @ design + penalty,
                                   design.T @ np.log1p(total_facts))
    return proof_weights, work_weights, mean, std


def predict(x, weights, mean, std):
    z = expand_features((x - mean) / std)
    return z @ weights[:-1] + weights[-1]


def binary_ranking_metrics(scores, labels):
    positive = scores[labels == 1]
    negative = scores[labels == 0]
    if not len(positive) or not len(negative):
        return None
    comparisons = positive[:, None] - negative[None, :]
    order = np.argsort(-scores, kind="stable")
    ordered_labels = labels[order]
    result = {"proof_pairwise_accuracy": float(np.mean(comparisons > 0))}
    for k in (1, 5, 10):
        result[f"recall_at_{k}"] = float(
            ordered_labels[:k].sum() / len(positive))
    return result


def efficiency_pairwise_accuracy(scores, efficiency):
    if len(scores) < 2:
        return None
    left, right = np.triu_indices(len(scores), 1)
    different = efficiency[left] != efficiency[right]
    if not different.any():
        return None
    left, right = left[different], right[different]
    agreements = ((scores[left] - scores[right]) *
                  (efficiency[left] - efficiency[right]) > 0)
    return float(agreements.mean())


def ranking_metrics(scores, labels, efficiency):
    result = binary_ranking_metrics(scores, labels)
    if result is None:
        return None
    result["efficiency_pairwise_accuracy"] = efficiency_pairwise_accuracy(
        scores, efficiency)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--work", type=Path, default=WORK)
    parser.add_argument("--traces", type=Path, default=TRACES)
    parser.add_argument("--rules", type=Path, default=RULES)
    parser.add_argument("--work-weights", type=float, nargs="+",
                        default=[0.0, 0.03, 0.1, 0.3, 1.0])
    args = parser.parse_args()
    if any(weight < 0 for weight in args.work_weights):
        parser.error("--work-weights must be nonnegative")
    args.work = args.work.resolve()
    args.work.mkdir(parents=True, exist_ok=True)
    rule_ids = sorted(path.name for path in args.rules.iterdir()
                      if path.is_dir())
    rule_edges = {rule_id: parse_rule(args.rules / rule_id / "rule.yaml")
                  for rule_id in rule_ids}

    rows = []
    extraction = []
    for goal_dir in sorted(args.traces.iterdir()):
        if not goal_dir.is_dir():
            continue
        run = select_trace(goal_dir)
        if run is None:
            extraction.append({"goal": goal_dir.name, "trace": "none",
                               "chain_steps": 0, "proof_steps": 0,
                               "proof_depth": 0, "observed_rules": 0,
                               "proof_rules": 0})
            continue
        proof_steps, depth, chain_steps, total_by_rule, proof_by_rule = \
            extract_proof(run, rule_edges)
        features = static_features(run, rule_ids)
        missing = proof_by_rule.keys() - features.keys()
        if missing:
            raise RuntimeError(f"proof rules lack features in {run}: {missing}")
        for rule_id, values in features.items():
            proof_hits = proof_by_rule[rule_id]
            total_facts = total_by_rule[rule_id]
            efficiency = ((proof_hits + EPSILON) /
                          (1 + total_facts))
            rows.append({"goal": goal_dir.name, "rule": rule_id,
                         "label": int(proof_hits > 0),
                         "proof_hits": proof_hits,
                         "total_facts": total_facts,
                         "proof_efficiency": efficiency,
                         "log_proof_efficiency": np.log(efficiency),
                         **dict(zip(FEATURES, values))})
        extraction.append({
            "goal": goal_dir.name,
            "trace": run.parent.name + "/" + run.name,
            "chain_steps": chain_steps,
            "proof_steps": len(proof_steps),
            "proof_depth": depth,
            "observed_rules": len(features),
            "proof_rules": len(proof_by_rule),
        })

    fields = ["goal", "rule", "label", "proof_hits", "total_facts",
              "proof_efficiency", "log_proof_efficiency", *FEATURES]
    with (args.work / "labels.tsv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, delimiter="\t",
                                lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    (args.work / "extraction.json").write_text(
        json.dumps(extraction, indent=2) + "\n")

    x = np.asarray([[row[name] for name in FEATURES] for row in rows])
    labels = np.asarray([row["label"] for row in rows], dtype=int)
    efficiency = np.asarray([row["proof_efficiency"] for row in rows])
    total_facts = np.asarray([row["total_facts"] for row in rows])
    groups = np.asarray([row["goal"] for row in rows])
    fold_data = []
    for goal in sorted(set(groups)):
        train = groups != goal
        test = groups == goal
        proof_weights, work_weights, mean, std = fit_components(
            x[train], labels[train], total_facts[train], groups[train])
        fold_data.append({
            "goal": goal, "test": test,
            "proof": predict(x[test], proof_weights, mean, std),
            "work": predict(x[test], work_weights, mean, std),
        })

    metrics = ("proof_pairwise_accuracy", "efficiency_pairwise_accuracy",
               "recall_at_1", "recall_at_5", "recall_at_10")

    def macro(scores_by_goal):
        scored = []
        for item in fold_data:
            test = item["test"]
            values = ranking_metrics(scores_by_goal[item["goal"]],
                                     labels[test], efficiency[test])
            if values is not None:
                scored.append(values)
        return {
            metric: float(np.mean([row[metric] for row in scored
                                   if row[metric] is not None]))
            for metric in metrics
        } | {"ranked_goals": len(scored)}

    sweep = {}
    for work_weight in args.work_weights:
        sweep[work_weight] = macro({
            item["goal"]: item["proof"] - work_weight * item["work"]
            for item in fold_data
        })
    if 0.0 not in sweep:
        if len(args.work_weights) != 1:
            parser.error("multi-weight selection requires --work-weights 0.0")
        selected_work_weight = args.work_weights[0]
    else:
        baseline = sweep[0.0]
        valid = [weight for weight in args.work_weights
                 if sweep[weight]["proof_pairwise_accuracy"] >=
                 baseline["proof_pairwise_accuracy"] - 0.02
                 and sweep[weight]["recall_at_10"] >=
                 baseline["recall_at_10"] - 0.02
                 and sweep[weight]["recall_at_1"] >=
                 baseline["recall_at_1"] - 0.05]
        selected_work_weight = max(
            valid,
            key=lambda weight: (sweep[weight]["efficiency_pairwise_accuracy"],
                                sweep[weight]["proof_pairwise_accuracy"],
                                sweep[weight]["recall_at_1"]))

    folds = []
    for item in fold_data:
        test = item["test"]
        scores = item["proof"] - selected_work_weight * item["work"]
        folds.append({
            "goal": item["goal"], "positive": int(labels[test].sum()),
            "negative": int((1 - labels[test]).sum()),
            "proof_efficiency": ranking_metrics(scores, labels[test],
                                                efficiency[test]),
            "dg_distance": ranking_metrics(
                -x[test, FEATURES.index("dp0")], labels[test], efficiency[test]),
            "exact_static": ranking_metrics(
                x[test, FEATURES.index("relfam")] +
                0.5 * x[test, FEATURES.index("conf")],
                labels[test], efficiency[test]),
        })

    proof_weights, work_weights, mean, std = fit_components(
        x, labels, total_facts, groups)
    weights = proof_weights - selected_work_weight * work_weights
    model_dir = args.work / "model"
    model_dir.mkdir(exist_ok=True)
    with (model_dir / "proof_star.meta").open("w") as stream:
        stream.write("format numeric_ridge_v1\n")
        stream.write("feature_schema proof_star_efficiency_v1\n")
        stream.write(f"n_features {len(FEATURES)}\n")
        stream.write("names " + " ".join(FEATURES) + "\n")
        for a, b in CONJ:
            stream.write(f"conj {a} {b}\n")
        stream.write("mean " + " ".join(f"{v:.17g}" for v in mean) + "\n")
        stream.write("std " + " ".join(f"{v:.17g}" for v in std) + "\n")
        stream.write("lam 10.0\ntarget proof_membership_minus_log_work\n")
        stream.write(f"work_weight {selected_work_weight:.17g}\n")
        stream.write(f"epsilon {EPSILON:.17g}\n")
        stream.write(f"train_nodes {len(rows)}\n")
    with (model_dir / "proof_star.bin").open("wb") as stream:
        stream.write(np.asarray(weights, dtype=np.float64).tobytes())

    summary = {"goals": len(folds), "rows": len(rows),
               "positive_rows": int(labels.sum()), "features": FEATURES,
               "epsilon": EPSILON, "selected_work_weight": selected_work_weight,
               "work_weight_sweep": {str(k): v for k, v in sweep.items()},
               "extraction": extraction, "folds": folds, "macro": {}}
    methods = ("proof_efficiency", "dg_distance", "exact_static")
    for method in methods:
        ranked = [fold for fold in folds if fold[method] is not None]
        summary["macro"][method] = {
            metric: float(np.mean([fold[method][metric] for fold in ranked
                                   if fold[method][metric] is not None]))
            for metric in metrics
        }
        summary["macro"][method]["ranked_goals"] = len(ranked)
    (args.work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    lines = [
        "# Proof-DAG proof-efficiency 标签与 leave-one-goal-out 验证", "",
        f"{len(folds)} goals 中提取 {len(rows)} 个规则样本，其中 "
        f"proof-positive={int(labels.sum())}。每个 fold 完全留出一个 goal。", "",
        f"效率标签为 (proof_hits + {EPSILON}) / (1 + total_facts)。"
        "运行时分数为 proof-membership 减去预测 log-work；"
        f"留出验证选择 work_weight={selected_work_weight}。", "",
        "| 方法 | proof pairwise | efficiency pairwise | recall@1 | recall@5 | recall@10 |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for method in methods:
        values = summary["macro"][method]
        lines.append(f"| {method} | " + " | ".join(
            f"{values[metric]:.3f}" for metric in metrics) + " |")
    lines += ["", "## 逐 goal", "",
              "| goal | chain steps | proof steps | depth | observed rules | proof rules |",
              "|---|---:|---:|---:|---:|---:|"]
    for row in extraction:
        lines.append(
            f"| {row['goal']} | {row['chain_steps']} | {row['proof_steps']} | "
            f"{row['proof_depth']} | {row['observed_rules']} | "
            f"{row['proof_rules']} |")
    lines += ["", "proof_hits=0 表示不在本次提取的证明 DAG 中；"
              "效率分母仍惩罚这些规则在轨迹中的高产展开。", ""]
    (args.work / "summary.md").write_text("\n".join(lines))
    print(json.dumps({"goals": len(folds), "rows": len(rows),
                      "positive_rows": int(labels.sum()),
                      "macro": summary["macro"]}, indent=2))


if __name__ == "__main__":
    main()
