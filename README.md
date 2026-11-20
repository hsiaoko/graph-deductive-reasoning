# GARAgent

GARAgent is a C++20 research library for mining Graph Association Rules (GARs) and applying them to knowledge graphs. It provides full-closure Chase, goal-directed deduction with AdaDeduce, CPU and GPU matching, learned action policies, and optional LLM guidance.

The implementation is organized around one invariant: **models may rank symbolic candidates, but only the symbolic engine may derive facts.**

## Highlights

- Mine chain rules and arbitrary small connected patterns from knowledge graphs.
- Evaluate constant, variable, existence, edge, and registered ML predicates.
- Compute a full closure with CPU or GPU Chase.
- Answer a grounded goal with dependency-only Chase or policy-guided AdaDeduce.
- Compare Deterministic, Random, Learned, and LLM-guided variants with one executor.
- Run independent goals concurrently without copying the base graph for each worker.
- Validate agent output against the exact Chase closure.

## Documentation

- [Architecture and extension points](docs/architecture.md)
- [Experiment variants and reproducibility](docs/experiments.md)
- [JEV and LLM guidance](docs/jev_guide.md)
- [Paper](paper/paper.pdf)

## Contents

- [Build](#build)
- [Quickstart](#quickstart)
- [Prepare your own data](#prepare-your-own-data)
- [Rule format](#rule-format)
- [Reasoning modes](#reasoning-modes)
- [Outputs](#outputs)
- [Analysis tools](#analysis-tools)
- [Repository layout](#repository-layout)
- [Validation and current limits](#validation-and-current-limits)

## Requirements

- Linux
- CMake 3.20 or newer
- A C++20 compiler, currently validated with GCC 12.4
- CUDA 12 or newer and an NVIDIA driver
- libcurl development files
- Python 3 for dataset and experiment scripts

MatrixGraph and LLM2Jev are Git submodules. MatrixGraph supplies graph arrays, matching components, CUDA buffers, and bundled yaml-cpp/oneTBB dependencies. CUDA is required to build the repository even when an experiment uses only the CPU backend.

## Build

```bash
git clone --recursive https://github.com/hsiaoko/graph-deductive-reasoning.git
cd graph-deductive-reasoning

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 8
```

Useful options:

```bash
# Select CUDA architectures.
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CUDA_ARCHITECTURES=70

# Build without example programs.
cmake -S . -B build -DGAR_BUILD_EXAMPLES=OFF

# Explicitly enable native CPU instructions for HDC kernels.
cmake -S . -B build -DGAR_HDC_NATIVE=ON
```

Release builds use `-O3 -DNDEBUG`. Native HDC instructions are disabled by default so that binaries remain portable.

## Quickstart

The following commands mine rules from the bundled synthetic graph, convert the graph to MatrixGraph format, compute its closure, and answer a goal. Run them from the repository root.

```bash
WORK="$(mktemp -d)"

# 1. Mine rules and convert the graph.
./build/bin/mine_rules \
  --graph data/synth \
  --out "$WORK/rules" \
  --min-supp 2 --min-conf 0.5

./build/bin/kg_loader convert \
  --graph data/synth \
  --out "$WORK/csr"

# 2. Inspect the rule set and compute the full closure.
./build/bin/rule_set_info "$WORK/rules"

./build/bin/gar_reason --mode chase --engine cpu \
  --csr "$WORK/csr" --rules "$WORK/rules" \
  --out "$WORK/chase"

# 3. Answer a goal with the deterministic agent.
./build/bin/gar_reason --mode agent --engine cpu \
  --csr "$WORK/csr" --rules "$WORK/rules" \
  --goal 'lives_in(p0,t1)' --budget 20 \
  --policy deterministic --scorer exact \
  --out "$WORK/agent" \
  --verify-against-closure

# 4. Check the agent, goal, and closure together.
./build/bin/gar_analyze verify --run \
  --csr "$WORK/csr" --rules "$WORK/rules" \
  --goal 'lives_in(p0,t1)' --budget 20
```

A successful agent run reports `goal_derived: true` in `$WORK/agent/summary.yaml`. Closure verification reports `soundness=pass`, and `gar_analyze verify` reports `CONSISTENT`.

Run `./build/bin/gar_reason` without arguments to print its usage. The executable currently does not implement a `--help` option.

## Prepare your own data

### 1. Convert triples to a Graph directory

```bash
python3 tools/prepare_triples.py \
  --format tsv \
  --in /path/to/train.txt \
  --out /path/to/triple_graph
```

The input uses tab-separated `subject`, `predicate`, and `object` fields. The output contains the graph arrays and entity/relation dictionaries. N-Triples and OGB BioKG inputs are also supported by the preparation script.

### 2. Convert Graph to ImmutableCSR

```bash
./build/bin/kg_loader convert \
  --graph /path/to/triple_graph \
  --out /path/to/immutable_csr
```

For typed graphs such as OGB BioKG, add `--label-from-prefix`. For example, an entity named `drug:CID1` receives the `drug` vertex label.

Inspect the converted graph with:

```bash
./build/bin/kg_loader show \
  --csr /path/to/immutable_csr --print 3
```

### 3. Mine rules

Mine chain rules:

```bash
./build/bin/mine_rules \
  --csr /path/to/immutable_csr \
  --out /path/to/rules \
  --min-supp 50 --min-conf 0.3
```

Mine arbitrary connected patterns instead:

```bash
./build/bin/mine_graph_rules \
  --csr /path/to/immutable_csr \
  --out /path/to/pattern_rules \
  --min-supp 50 --min-conf 0.3 \
  --max-edges 3
```

Use `--max-body-len 2` with `mine_rules` to include two-edge chains. With `mine_graph_rules`, use `--min-body-edges 2` to exclude one-edge output rules. General pattern mining can grow quickly; start with `--max-edges 2` or a high support threshold on a large graph.

Each output rule is stored in its own directory:

```text
rules/gar_000001/
├── rule.yaml
├── csr_offsets.bin
├── csr_indices.bin
└── csr_edge_labels.bin    # when relation labels are present
```

Inspect rules or recompute support/confidence with:

```bash
./build/bin/rule_set_info /path/to/rules 10
./build/bin/validate_rules \
  --csr /path/to/immutable_csr --rules /path/to/rules
```

## Rule format

A rule contains a pattern `Q`, predicates `X`, an optional consequent `p0`, and quality metadata. A minimal YAML example is:

```yaml
rule:
  id: "gar_example_001"
  labels: {Person: 1, Company: 2, works_at: 10}
  pattern:
    nodes:
      - {id: 0, var: "x", label: "Person"}
      - {id: 1, var: "y", label: "Company"}
    edges:
      - {src: 0, dst: 1, label: "works_at"}
    pivot: [0]
  predicates:
    - {var: 0, attr: "age", op: ">=", value: 18}
  consequent:
    {src: 0, dst: 1, label: "works_at"}
  quality:
    {support: 123, confidence: 0.87}
```

Predicate forms are:

| Kind | Example | Meaning |
|---|---|---|
| `const` | `x.age >= 18` | Compare an attribute with a constant |
| `var` | `x.city = y.city` | Compare attributes across variables |
| `exists` | `exists(x.phone)` | Require an attribute to exist |
| `edge` | `works_at(x,y)` | Require a grounded pattern fact |
| `ml` | `M_anom(x,y,anom)` | Evaluate a registered model predicate |

### ML predicates

Register an exact ML model with `--ml-config`. The built-in `constant` adapter is intended for controlled validation:

```yaml
models:
  - {name: M_anom, kind: constant, score: 0.9}
```

```bash
./build/bin/gar_reason --mode agent \
  --csr /path/to/immutable_csr --rules /path/to/rules \
  --goal 'relation(a,b)' --budget 100 \
  --ml-config /path/to/models.yaml \
  --out /tmp/garagent-ml-agent
```

An unregistered model is an error; GARAgent does not silently treat it as false or skip its rule. CPU Chase and CPU/GPU agent share the exact predicate interface. GPU Chase currently requires an explicit host adapter for ML predicates.

Attribute mining is available through `mine_predicates` after numeric vertex attributes have been added with `kg_loader add-vertex-attrs`.

## Reasoning modes

| Mode | Purpose | Backend |
|---|---|---|
| `chase` | Compute the full closure, optionally with resource caps | CPU or GPU |
| `dep-only` | Run fixed-order, single-threaded goal-cone Chase and stop at the goal | CPU |
| `agent` | Run goal-directed AdaDeduce for one goal | CPU or GPU |
| `agent-batch` | Run independent goals with shared immutable inputs | CPU or GPU |

### Full Chase

```bash
./build/bin/gar_reason --mode chase --engine cpu \
  --csr /path/to/immutable_csr --rules /path/to/rules \
  --out /tmp/garagent-chase \
  --max-rounds 100 \
  --max-derivations 10000000 \
  --max-matches 1000000 \
  --threads 0
```

Select the GPU explicitly with `--engine gpu`. Add `--no-matches` to avoid writing per-rule match tables and `--print 0` to suppress rule summaries.

Reaching a resource cap is not the same as reaching a fixpoint. Always inspect `summary.yaml` before treating a closure as complete. Uncapped CPU and GPU runs derive the same fact set; capped runs may retain different subsets because enumeration order differs.

### Dependency-only goal reasoning

```bash
./build/bin/gar_reason --mode dep-only \
  --csr /path/to/immutable_csr --rules /path/to/rules \
  --goal 'relation(a,b)' \
  --out /tmp/garagent-dep-only
```

DepOnly uses only dependency-cone filtering and the fixed-order CPU Chase. It does not use policy scoring, learned models, the ready heap, or the agent's suspended-set scheduler.

### Deterministic and Random agents

```bash
# Deterministic.
./build/bin/gar_reason --mode agent --engine cpu \
  --csr /path/to/immutable_csr --rules /path/to/rules \
  --goal 'relation(a,b)' --budget 1000 \
  --policy deterministic --scorer exact --star-order goal \
  --out /tmp/garagent-deterministic

# Seeded softmax sampling over the same handcrafted score.
./build/bin/gar_reason --mode agent --engine cpu \
  --csr /path/to/immutable_csr --rules /path/to/rules \
  --goal 'relation(a,b)' --budget 1000 \
  --policy random --policy-seed 7 --scorer exact --star-order goal \
  --out /tmp/garagent-random
```

`--scorer hdc` uses set-based dense HDC evidence for ordering. `--scorer null` is an ablation, not a DepOnly implementation.

### Learned policy

```bash
./build/bin/gar_reason --mode agent --engine cpu \
  --csr /path/to/immutable_csr --rules /path/to/rules \
  --goal 'relation(a,b)' --budget 1000 \
  --policy learned --scorer exact --star-order goal \
  --value-model /path/to/q_numeric.meta \
  --fair-interval 0 \
  --out /tmp/garagent-learned
```

The model file declares its feature schema. Incompatible legacy models are rejected rather than evaluated with different feature semantics. Training and trace-generation workflows are described in [the experiment guide](docs/experiments.md).

### AdaDeduce with an initial LLM guide

AdaDeduce uses the learned policy and, optionally, one restricted action-level guide per goal:

```bash
export DEEPSEEK_API_KEY='your-key'

./build/bin/gar_reason --mode agent --engine cpu \
  --csr /path/to/immutable_csr --rules /path/to/rules \
  --goal 'relation(a,b)' --budget 1000 \
  --policy learned --scorer exact --star-order goal \
  --value-model /path/to/q_numeric.meta \
  --paper-guide --llm-backend openai \
  --llm-base-url https://api.deepseek.com \
  --llm-model deepseek-chat \
  --out /tmp/garagent-ada-deduce
```

The guide is compiled into a restricted priority language. It cannot create a fact, introduce a rule, or bypass predicate evaluation. See [the JEV/LLM guide](docs/jev_guide.md) for local models, mock runs, caching, protocols, and troubleshooting.

### Ordered action batches

Agent mode uses `--batch-size 1` by default. Set `--batch-size K` to select an ordered top-k action batch from batch-start scores:

```bash
./build/bin/gar_reason --mode agent \
  --csr /path/to/immutable_csr --rules /path/to/rules \
  --goal 'relation(a,b)' --budget 1000 \
  --policy deterministic --scorer exact \
  --batch-size 4 \
  --out /tmp/garagent-batch4
```

This is an action batch within one goal. It is different from `agent-batch`, which runs multiple independent goals.

### Multiple goals

Create a tab-separated task file:

```text
goal_id	spec
g0	lives_in(p0,t1)
g1	relation(a,b)
```

Run it with:

```bash
./build/bin/gar_reason --mode agent-batch --engine cpu \
  --csr /path/to/immutable_csr --rules /path/to/rules \
  --tasks /path/to/goals.tsv \
  --workers 4 \
  --policy deterministic --scorer exact \
  --gt /path/to/chase-output \
  --out /tmp/garagent-goals
```

Workers share immutable graph data and the dependency graph. Each goal owns its derived delta and tombstones. `--gt` is optional; when provided, each task's derived facts are checked against `closure_edges.tsv` in that directory.

## Outputs

A Chase output directory contains:

| File | Contents |
|---|---|
| `summary.yaml` | Rounds, derived count, per-rule statistics, caps, and fixpoint status |
| `closure_edges.tsv` | Derived source, relation, destination, round, and source rule |
| `matches/` | Optional per-rule match tables |

An agent output directory contains:

| File | Contents |
|---|---|
| `summary.yaml` | Goal status, termination, costs, execution counters, model overhead, and soundness when requested |
| `chain.tsv` | Selected actions, success, cost, and derived fact |
| `closure_edges.tsv` | Facts derived by this goal-directed run |
| `deduce_trace.tsv` | Ready, suspend, revive, evaluate, and derive events |
| `paper_guide.json` | Raw guide response when `--paper-guide` is active |

Agent-batch writes one directory per `goal_id` and an aggregate `summary.yaml` in the output root.

Important distinctions:

- `logical_cost` is charged symbolic work.
- `execution_cost_ms` sums measured executor phases.
- `proof_cost` counts distinct ancestors in the goal proof DAG.
- External wall time additionally includes process startup, graph loading, output, and online model calls.
- A successful process exit does not imply that the goal was derived. Check `goal_derived` and `termination`.

## Analysis tools

```bash
# Dependency statistics.
./build/bin/gar_analyze rules-stats --rules /path/to/rules

# Compare an agent output with a closure output.
./build/bin/gar_analyze chain-cmp \
  --agent-out /tmp/garagent-deterministic \
  --closure-out /tmp/garagent-chase \
  --goal 'relation(a,b)'

# Verify existing output directories.
./build/bin/gar_analyze verify --from-dirs \
  /tmp/garagent-deterministic /tmp/garagent-chase \
  --goal 'relation(a,b)'
```

`gar_analyze gen-goals` and `gen-workload` can construct held-out goal workloads. See the command's built-in usage text and [the experiment guide](docs/experiments.md).

## Repository layout

```text
core/
├── include/ and src/       # Rules, predicates, YAML loading
├── graph/                  # Immutable base graph, per-goal delta, matching, predicates
├── analysis/               # Predicate table and dependency graph
├── hdc/                    # Dense and bit-packed HDC kernels
└── engine/
    ├── chase/              # Full-closure CPU/GPU Chase
    └── agent/              # AdaDeduce, policies, scoring, learning, guides

tools/
├── cli/                    # gar_reason, gar_analyze, loaders, matchers
├── miner/                  # Chain and general-pattern miners
└── prepare_*.py            # Dataset conversion

examples/                   # Library and model examples
experiments/                # Experiment definitions and historical reports
third_party/MatrixGraph/    # Graph arrays, matching, CUDA infrastructure
third_party/llm2jev/        # Optional structured decision-model service
paper/                      # Paper and experimental settings
```

Downstream C++ consumers can link the umbrella `gar::core` target or individual targets such as `gar::core::graph`, `gar::core::engine::chase`, and `gar::core::engine::agent`.

## Validation and current limits

Useful validation commands are:

```bash
cmake --build build -j 8
./build/bin/load_rule data/rules/example
./build/bin/matrixgraph_bridge data/rules/example
./build/bin/gar_analyze verify --run \
  --csr /path/to/immutable_csr --rules /path/to/rules \
  --goal 'relation(a,b)' --budget 100
```

Keep these limitations in mind:

- CUDA is a build dependency even for CPU-only experiments.
- A candidate cap, timeout, or memory failure can make a search incomplete. These conditions are reported and must not be hidden in evaluations.
- Soundness means that derived facts belong to the exact closure. It does not prove goal-level completeness under caps or finite budgets.
- GPU Chase does not provide the same ML-predicate path as GPU agent; an explicit host adapter is required.
- LLM mock clients validate control flow, not semantic benefit.
- Warm LLM caches measure repeated-goal execution, not cold-start cost.

For implementation details and extension guidance, continue with [docs/architecture.md](docs/architecture.md).
