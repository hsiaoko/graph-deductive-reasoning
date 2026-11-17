#!/usr/bin/env python3
import argparse
import csv
import json
from pathlib import Path
import random
import statistics
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[2]
TOPICS = [
    "celebrity", "sports", "film", "location", "award", "organization",
    "television", "soccer", "music", "politics", "literature", "travel",
    "technology", "education", "medicine", "history",
]


class DataBuilder:
    def __init__(self):
        self.entities = {}
        self.relations = {}
        self.edges = []

    def entity(self, name):
        if name not in self.entities:
            self.entities[name] = len(self.entities)
        return self.entities[name]

    def relation(self, name):
        if name not in self.relations:
            self.relations[name] = len(self.relations)
        return self.relations[name]

    def edge(self, src, relation, dst):
        self.edges.append((self.entity(src), self.relation(relation),
                           self.entity(dst)))

    def write(self, directory):
        directory.mkdir(parents=True, exist_ok=True)
        with (directory / "entities.tsv").open("w") as stream:
            for name, index in self.entities.items():
                stream.write(f"{index}\t{name}\n")
        with (directory / "relations.tsv").open("w") as stream:
            for name, index in self.relations.items():
                stream.write(f"{index}\t{name}\n")
        with (directory / "edges.bin").open("wb") as stream:
            for edge in self.edges:
                stream.write(struct.pack("=III", *edge))


def rule_text(rule_id, family, stage, head, description, relation_ids):
    previous = f"{family}_stage{stage}"
    link = f"{family}_link{stage}"
    name = f"{previous}(x0,x1) ∧ {link}(x1,x2) → {head}(x0,x2)"
    return f"""rule:
  id: {rule_id}
  name: "{name}"
  description: "{description}"
  labels:
    {previous}: {relation_ids[previous]}
    {link}: {relation_ids[link]}
    {head}: {relation_ids[head]}
  pattern:
    nodes:
      - {{id: 0}}
      - {{id: 1}}
      - {{id: 2}}
    edges:
      - {{src: 0, dst: 1, label: {previous}}}
      - {{src: 1, dst: 2, label: {link}}}
    pivot: [0, 2]
  consequent:
    kind: edge
    src: 0
    dst: 2
    label: {head}
  quality:
    support: 100
    confidence: 0.8
    dataset: semantic-pilot
    miner: controlled-generator
"""


def generate_cell(work, distractors, fanout, goal_count):
    cell = f"D{distractors:02d}_F{fanout:04d}"
    root = work / "inputs" / cell
    graph = root / "graph"
    csr = root / "csr"
    rules = root / "rules"
    goals_path = root / "goals.tsv"
    if (goals_path.exists() and (csr / "meta.yaml").exists() and
            rules.exists()):
        return cell, csr, rules, goals_path

    builder = DataBuilder()
    goals = []
    for goal in range(goal_count):
        customer = f"customer_{goal}"
        target = f"risk_{goal}"
        previous = customer
        builder.edge(previous, "account_stage0", f"account_{goal}_0")
        previous = f"account_{goal}_0"
        for stage in range(4):
            following = target if stage == 3 else f"account_{goal}_{stage + 1}"
            builder.edge(previous, f"account_link{stage}", following)
            previous = following
        spec = f"financial_risk({customer},{target})"
        goals.append({"goal_id": f"g{goal}", "spec": spec})

    for topic in TOPICS[:distractors]:
        for item in range(fanout):
            previous = f"{topic}_source_{item}"
            builder.edge(previous, f"{topic}_stage0", f"{topic}_{item}_0")
            previous = f"{topic}_{item}_0"
            for stage in range(4):
                following = (f"{topic}_risk_{item}" if stage == 3 else
                             f"{topic}_{item}_{stage + 1}")
                builder.edge(previous, f"{topic}_link{stage}", following)
                previous = following
    builder.relation("financial_risk")
    for family in ["account", *TOPICS[:distractors]]:
        for stage in range(1, 4):
            builder.relation(f"{family}_stage{stage}")
    builder.write(graph)

    descriptions = []
    for stage in range(4):
        descriptions.append((
            "account", stage,
            "Verified customer account ownership and audited transaction "
            "evidence used to establish financial risk."))
    for topic in TOPICS[:distractors]:
        for stage in range(4):
            descriptions.append((
                topic, stage,
                f"{topic.capitalize()} relationships and entertainment or "
                "general-purpose contextual evidence."))
    rng = random.Random(1000 + distractors * 100 + fanout)
    rng.shuffle(descriptions)
    for index, (family, stage, description) in enumerate(descriptions):
        rule_id = f"gar_{index:06d}"
        directory = rules / rule_id
        directory.mkdir(parents=True, exist_ok=True)
        head = "financial_risk" if stage == 3 else f"{family}_stage{stage + 1}"
        (directory / "rule.yaml").write_text(
            rule_text(rule_id, family, stage, head, description,
                      builder.relations))

    with goals_path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=["goal_id", "spec"],
                                delimiter="\t", lineterminator="\n")
        writer.writeheader()
        writer.writerows(goals)
    subprocess.run([
        str(ROOT / "build/bin/kg_loader"), "convert", "--graph", str(graph),
        "--out", str(csr),
    ], cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
    return cell, csr, rules, goals_path


def command_for(args, csr, rules, goal, output, guided):
    command = [
        str(ROOT / "build/bin/gar_reason"), "--mode", "agent", "--engine",
        args.engine, "--csr", str(csr), "--rules", str(rules), "--goal",
        goal["spec"], "--out", str(output), "--print", "0", "--star-order",
        "input", "--policy", "deterministic", "--scorer", "exact", "--budget",
        "1000000", "--max-candidates-per-rule", "100000", "--fair-interval",
        "0", "--verify-against-closure",
    ]
    if guided:
        command += [
            "--llm-guide", "--llm-backend", args.llm_backend,
            "--llm-model", args.llm_model, "--llm-base-url",
            args.llm_base_url, "--llm-timeout-ms", str(args.llm_timeout_ms),
        ]
        if args.llm_mock:
            command.append("--llm-mock")
    return command


def run_arm(args, cell, csr, rules, goals, arm):
    for goal in goals:
        guided = arm == "deepseek"
        name = f"{cell}/{goal['goal_id']}/{arm}"
        output = args.work / "runs" / name
        resource = output / "resource.json"
        if resource.exists():
            print(f"skip {name}", flush=True)
            continue
        output.mkdir(parents=True, exist_ok=True)
        command = command_for(args, csr, rules, goal, output, guided)
        subprocess.run([
            "python3", str(ROOT / "experiments/fixpoint_policy/run_bounded.py"),
            "--log", str(output / "run.log"), "--result", str(resource),
            "--timeout-sec", "300", "--memory-gb", "256",
            "--system-memory-gb", "256", "--", *command,
        ], cwd=ROOT, check=False, stdout=subprocess.DEVNULL)
        print(f"done {name}", flush=True)


def read_summary(path):
    result = {}
    guide = False
    for line in path.read_text().splitlines():
        if line == "guide_overhead:":
            guide = True
            continue
        if guide and line.startswith("  "):
            key, value = line.strip().split(":", 1)
            result["guide_" + key] = value
        elif ":" in line and not line.startswith(" "):
            guide = False
            key, value = line.split(":", 1)
            result[key] = value.strip()
    return result


def aggregate(args):
    rows = []
    for resource in sorted(args.work.glob("runs/*/*/*/resource.json")):
        run = resource.parent
        cell, goal, arm = run.parts[-3:]
        meta = read_summary(run / "summary.yaml")
        saved = json.loads(resource.read_text())
        rows.append({
            "cell": cell, "goal": goal, "arm": arm,
            "wall": saved["wall_sec"], "hit": meta.get("goal_derived") == "true",
            "sound": meta.get("soundness") == "pass",
            "evaluations": int(meta["evaluate_events"]),
            "matches": int(meta["match_embeddings"]),
            "guide_wall": float(meta.get("guide_wall_ms", 0)) / 1000,
            "guide_calls": int(meta.get("guide_calls", 0)),
            "guide_fallbacks": int(meta.get("guide_fallbacks", 0)),
        })
    groups = []
    cells = sorted({row["cell"] for row in rows})
    for cell in cells:
        for arm in ("baseline", "deepseek"):
            selected = [row for row in rows if row["cell"] == cell and
                        row["arm"] == arm]
            if not selected:
                continue
            groups.append({
                "cell": cell, "arm": arm, "runs": len(selected),
                "wall": statistics.mean(row["wall"] for row in selected),
                "evaluations": statistics.mean(row["evaluations"] for row in selected),
                "matches": statistics.mean(row["matches"] for row in selected),
                "guide_wall": statistics.mean(row["guide_wall"] for row in selected),
                "hits": sum(row["hit"] for row in selected),
                "sound": sum(row["sound"] for row in selected),
                "fallbacks": sum(row["guide_fallbacks"] for row in selected),
            })
    report = {"rows": rows, "groups": groups}
    (args.work / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    for group in groups:
        print("{cell} {arm}: wall={wall:.3f}s eval={evaluations:.1f} "
              "matches={matches:.1f} api={guide_wall:.3f}s hits={hits} "
              "sound={sound} fallback={fallbacks}".format(**group))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--work", type=Path,
                        default=ROOT / ".work_hdcv/semantic_guide_pilot")
    parser.add_argument("--engine", choices=("cpu", "gpu"), default="cpu")
    parser.add_argument("--goals-per-cell", type=int, default=5)
    parser.add_argument("--llm-backend", choices=("openai", "jev"),
                        default="openai")
    parser.add_argument("--llm-model", default="deepseek-chat")
    parser.add_argument("--llm-base-url", default="https://api.deepseek.com")
    parser.add_argument("--llm-timeout-ms", type=int, default=120000)
    parser.add_argument("--llm-mock", action="store_true")
    parser.add_argument("--phase", choices=("generate", "baseline", "deepseek",
                                            "aggregate", "all"), default="all")
    args = parser.parse_args()
    args.work = args.work.resolve()
    cells = []
    for distractors in (0, 4, 16):
        for fanout in (100, 1000):
            cells.append(generate_cell(args.work, distractors, fanout,
                                       args.goals_per_cell))
    for cell, csr, rules, goals_path in cells:
        with goals_path.open() as stream:
            goals = list(csv.DictReader(stream, delimiter="\t"))
        if args.phase in ("baseline", "all"):
            run_arm(args, cell, csr, rules, goals, "baseline")
        if args.phase in ("deepseek", "all"):
            run_arm(args, cell, csr, rules, goals, "deepseek")
    if args.phase in ("aggregate", "all"):
        aggregate(args)


if __name__ == "__main__":
    main()
