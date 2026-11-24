# Experiments and reproducibility

This guide explains how to construct goal workloads, train action policies, compare variants, and report valid results. It describes the current interfaces rather than a future refactoring plan.

## Variant contract

All agent variants must use the same graph, frozen rules, goals, predicate semantics, and execution limits.

| Name | Executor and action selection | Additional input |
|---|---|---|
| Chase | Full rule-set closure to a fixpoint or explicit cap | None |
| DepOnly | Fixed-input-order, single-threaded goal-cone Chase | Dependency filter only |
| Deterministic | AdaDeduce with `--policy deterministic --scorer exact` | None |
| Random | AdaDeduce with `--policy random --scorer exact` | Policy seed |
| UniformRandom | AdaDeduce with `--policy uniform-random`; ignores all scores | Policy seed |
| Learned | AdaDeduce with `--policy learned --scorer exact` | Action-goal relevance model |
| AdaDeduce | Learned policy with optional `--paper-guide` | Relevance model and optional real LLM guide |
| AdaDeduceStar | Explicit action policy with proof-efficiency star ordering | Proof-star model |

Do not substitute `--scorer null` for DepOnly. Null scoring still uses the optimized AdaDeduce executor, while DepOnly deliberately uses the fixed-order Chase path.

Do not silently include proof-star ordering in AdaDeduce. It is a separate extension with its own name and model.

## Recommended workflow

```text
Prepare graph and rules
        |
        v
Freeze rule IDs, graph, and code identity
        |
        v
Run full Chase to produce ground truth
        |
        v
Freeze train/test goals before evaluating policies
        |
        +--------------------+
        |                    |
        v                    v
Collect training traces   Preserve untouched test goals
        |                    |
        v                    |
Train and freeze models      |
        |                    |
        +--------------------+
                           |
                           v
              Evaluate all variants with one protocol
                           |
                           v
              Validate soundness, caps, and resources
```

## 1. Freeze inputs

Record at least:

- source and graph conversion commands;
- graph directory and entity/relation counts;
- rule root and sorted rule IDs;
- SHA-256 for rules, goals, models, ground truth, and the `gar_reason` binary;
- build type, compiler, CUDA version, and CPU/GPU backend;
- policy, scorer, star order, fair interval, batch size, and candidate cap;
- seeds and external model identifiers.

`rule_set_info` and `gar_analyze rules-stats` provide basic inspection:

```bash
./build/bin/rule_set_info /path/to/rules
./build/bin/gar_analyze rules-stats --rules /path/to/rules
```

Do not select rules by looking at test-goal performance. A deterministic quota, structural stratification, or stable hash is easier to audit.

## 2. Produce ground truth

```bash
./build/bin/gar_reason --mode chase --engine cpu \
  --csr /path/to/immutable_csr --rules /path/to/rules \
  --out /path/to/ground-truth \
  --max-rounds 100 \
  --max-derivations 1000000000 \
  --max-matches 1000000000 \
  --no-matches --print 0
```

Confirm that the run reached a fixpoint. A capped closure is not complete ground truth for soundness or goal-generation claims.

The closure's first derivation round can be used as a difficulty proxy:

- `round1`: derived in the first round;
- `round2`: derived in the second round;
- `deep`: derived in round three or later.

This is not the same as shortest proof length. Report it as a derivation-round stratum.

## 3. Generate goals

`gar_analyze gen-goals` samples held-out triples, removes them from the input graph, runs Chase, and retains goals that the closure re-derives:

```bash
./build/bin/gar_analyze gen-goals \
  --csr /path/to/immutable_csr --rules /path/to/rules \
  --per-slot 2 --seed 7 \
  --out /path/to/goals.tsv
```

`gen-workload` creates a task/budget workload:

```bash
./build/bin/gar_analyze gen-workload \
  --csr /path/to/immutable_csr --rules /path/to/rules \
  --num-tasks 36 --seed 7 \
  --budgets 0.5,1,2,4 \
  --out /path/to/tasks.tsv
```

For learned policies, freeze train and test goals before training. Keep these forms of separation explicit:

- **Instance-disjoint:** no identical grounded goal appears in training and testing.
- **Relation-disjoint:** all training goals and traces for selected test relations are excluded.
- **Proof-disjoint:** test goals do not share training chains, proof DAGs, policy traces, or derivation journals.

If a requested stratum cannot satisfy the constraints, report the constraint failure instead of removing difficult goals after seeing method results.

## 4. Collect training traces and train

Use `--policy-trace` with exact incremental scoring. After execution, the trace keeps proof actions, grounded proof premises, and a random sample of successful actions outside the proof. The run does not record every candidate or derivation. Collection does not change the reasoning chain.

```bash
./build/bin/gar_reason --mode agent --engine cpu \
  --csr /path/to/immutable_csr --rules /path/to/rules \
  --goal 'relation(a,b)' --budget 1000000 \
  --policy deterministic --scorer exact --star-order goal \
  --fair-interval 0 \
  --policy-trace /path/to/traces/goal-000/deterministic/policy_trace.json \
  --policy-trace-negatives 256 \
  --out /path/to/traces/goal-000/deterministic
```

Only goal-hit traces produce training examples. The trainer extracts the goal proof DAG and assigns `label_gamma^distance` to proof actions. It assigns zero to sampled candidates outside the proof. Repeated fact-goal pairs use the smallest observed distance. Negative labels mean that a candidate was not observed in the proof, not that it is logically irrelevant.

Train the C++ relevance model:

```bash
./build/bin/gar_train_policy \
  --csr /path/to/immutable_csr --rules /path/to/rules \
  --traces /path/to/traces \
  --test-goals /path/to/test-goals.tsv \
  --validation-goals /path/to/validation-goals.tsv \
  --out /path/to/models/relevance.meta \
  --epochs 8 --batch-size 32 --hidden 32 \
  --label-gamma 0.5 --negative-ratio 4 --seed 42
```

The trainer rejects test-goal leakage. It writes `relevance.meta`, `relevance.bin`, and `relevance.report.json`. The model and report record the HDC configuration, graph fingerprint, feature schema, training goals, loss, and held-out ranking metrics.

Keep the model files, excluded goals, seeds, feature schema, and input hashes together.

For the frozen FB15K-237 rules640 workload, `experiments/fixpoint_policy/run_relevance.py` runs the same phases and preserves per-goal resources:

```bash
python3 experiments/fixpoint_policy/run_relevance.py --phase all
```

Use `--phase trace`, `--phase train`, `--phase eval`, `--phase verify`, or `--phase aggregate` to resume one phase. The driver writes to `.work_hdcv/relevance640` and does not use the 36 test goals for model selection.

## 5. Evaluate variants

Use one command template and change only the declared variant fields. For example:

```bash
# Deterministic.
./build/bin/gar_reason --mode agent --engine cpu \
  --csr /path/to/immutable_csr --rules /path/to/rules \
  --goal 'relation(a,b)' --budget 1000000000 \
  --policy deterministic --scorer exact --star-order goal \
  --max-candidates-per-rule 100000 --fair-interval 0 \
  --wall-limit 900 --policy-seed 7 \
  --out /path/to/runs/goal-000/Deterministic/seed7

# Learned.
./build/bin/gar_reason --mode agent --engine cpu \
  --csr /path/to/immutable_csr --rules /path/to/rules \
  --goal 'relation(a,b)' --budget 1000000000 \
  --policy learned --scorer exact --star-order goal \
  --relevance-model /path/to/models/relevance.meta \
  --max-candidates-per-rule 100000 --fair-interval 0 \
  --wall-limit 900 --policy-seed 7 \
  --out /path/to/runs/goal-000/Learned/seed7
```

For AdaDeduce, add the same relevance model and:

```text
--paper-guide --llm-backend openai --llm-model deepseek-chat
```

Use a real model response when evaluating semantic benefit. A mock guide is a control-flow check only.

CPU is the default agent backend. Add `--engine gpu` only for an explicitly declared GPU experiment.

## 6. Use resource guards

`experiments/fixpoint_policy/run_bounded.py` records external wall time, return code, process-tree memory, and guard outcomes:

```bash
python3 experiments/fixpoint_policy/run_bounded.py \
  --log /path/to/runs/example/run.log \
  --result /path/to/runs/example/resource.json \
  --timeout-sec 1000 \
  --memory-gb 256 \
  --system-memory-gb 256 \
  -- \
  ./build/bin/gar_reason --mode agent --engine cpu \
    --csr /path/to/immutable_csr --rules /path/to/rules \
    --goal 'relation(a,b)' --budget 1000000000 \
    --policy deterministic --scorer exact \
    --out /path/to/runs/example
```

A nonzero exit, `timeout`, `oom_system`, or missing summary must remain visible in aggregation. Do not rerun a difficult configuration with weaker limits and replace the original result without reporting the change.

## 7. Check correctness

For one goal, use:

```bash
./build/bin/gar_analyze verify --run \
  --csr /path/to/immutable_csr --rules /path/to/rules \
  --goal 'relation(a,b)' --budget 1000000
```

For a batch, pass the full closure directory through `--gt`:

```bash
./build/bin/gar_reason --mode agent-batch \
  --csr /path/to/immutable_csr --rules /path/to/rules \
  --tasks /path/to/test-goals.tsv --workers 4 \
  --policy learned --relevance-model /path/to/models/relevance.meta \
  --gt /path/to/ground-truth \
  --out /path/to/batch-output
```

Soundness means every reported derived fact belongs to the exact closure. It does not prove that the search explored the full closure or that every derivable goal would be hit under the same caps.

## Measurement protocol

### Repetitions and aggregation

- Keep at least three seeds or timing repetitions for the full matrix.
- For a small microbenchmark difference, use additional interleaved repetitions.
- Report the run count and range, not only a mean.
- A practical macro average is the mean over goals of each goal's seed median.
- Retain per-seed rows so that another aggregation can be audited.
- If run-to-run variation exceeds the method difference, call the result inconclusive.

Deterministic and learned policies may repeat the same logical trajectory across seeds. Their repeated runs measure timing variation, not new search distributions. Random and some guide policies can produce different trajectories and must retain every seed.

### Cold and warm model states

Report these separately:

1. service startup and model loading;
2. a real guide request for an unseen goal;
3. a repeated request or explicit response cache;
4. total lifecycle cost, including training when applicable.

`--paper-guide-cache-dir` reuses a real response for the same goal. It does not measure cold-start reasoning and must be labeled as warm-cache execution.

### Metrics

Include at least:

- external wall and executor phase times;
- goal hit rate and termination;
- derived facts and evaluated actions;
- match embeddings and ready insertions;
- proof-DAG cost where relevant;
- guide calls, guide wall time, fallbacks, and rejected entries;
- candidate caps, timeouts, OOM, and peak RSS;
- model identity, feature schema, training rows, and cache state.

A reduction in derived facts is not automatically a wall-time improvement. Matching, ready-set maintenance, policy overhead, model calls, and output can dominate the result.

## Experiment files

`experiments/` contains dataset-specific drivers, preregistrations, and historical reports. These files are experiment records, not a stable Python API. Check their paths and frozen inputs before reusing them.

Notable directories:

- `experiments/fixpoint_policy/`: policy training, bounded execution, frozen-goal experiments, and preregistrations;
- `experiments/fixpoint_policy/UNIFIED640_PREREGISTRATION.md`: the FB15K-237 rules640 experiment contract;
- `experiments/overall-performance/ogbl-biokg/`: BioKG experiment design and driver;
- `experiments/fb15k-237-results/`: historical performance reports;
- `experiments/llm_guide/`: LLM and local JEV comparison drivers.

Local `.work_hdcv/` outputs are intentionally ignored by Git. Copy required final reports and manifests to a deliberate publication location instead of relying on an ignored working directory.

## Reporting checklist

Before publishing a comparison, verify that:

- every method used the intended frozen rules and goals;
- training and testing are separated at the claimed level;
- no failed, capped, or inconvenient goal was silently removed;
- the work actually ran and the goal/closure checks passed;
- cold and warm model results are not mixed;
- per-goal and per-seed results are available;
- the binary, model, goal, and ground-truth hashes are recorded;
- the reported difference exceeds the relevant noise floor;
- the main limiter is supported by counters or profiling rather than speculation.
