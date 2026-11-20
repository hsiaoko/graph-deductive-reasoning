# Architecture

This document describes the current implementation, its state ownership, and the interfaces intended for future optimization. It is not a migration plan.

## Design invariants

1. **One symbolic deduction semantics.** Every agent variant uses the same executor, exact predicate checks, and fact derivation path.
2. **Policies only order work.** A policy, value model, or LLM cannot create a rule, bypass a precondition, or write a fact directly.
3. **Chase and agent are siblings.** The agent library does not depend on Chase. Closure validation is composed by the application layer.
4. **Immutable inputs are shared.** A goal owns its derived delta, tombstones, working sets, and policy state.
5. **MatrixGraph remains generic.** GARAgent-specific scheduling, delta handling, and learning stay outside MatrixGraph.
6. **Costs stay explicit.** Logical work, measured executor time, proof-DAG cost, and external wall time are different metrics.

## Dependency structure

```text
                         tools/cli and examples
                                  |
                    +-------------+--------------+
                    |                            |
             gar_engine_chase             gar_engine_agent
                    |                            |
                    +-------------+--------------+
                                  |
             +--------------------+--------------------+
             |                    |                    |
        gar_graph           gar_analysis            gar_hdc
             |                    |                    |
             +---------+----------+                    |
                       |                               |
                   gar_rule                            |
                       |                               |
                matrixgraph_core                       |
```

The public CMake targets are:

| Target | Responsibility |
|---|---|
| `gar::core::rule` | GAR representation, predicates, YAML loading, rule storage |
| `gar::core::graph` | Graph arrays, dictionaries, per-goal delta, matching, exact predicates |
| `gar::core::analysis` | Predicate table, dependency graph, goal distances |
| `gar::core::hdc` | Dense and bit-packed HDC representations and kernels |
| `gar::core::engine::chase` | Full-closure CPU and GPU Chase |
| `gar::core::engine::agent` | AdaDeduce, policies, scoring, value models, and guides |
| `gar::core` | Umbrella interface target, including the rule miners |

The agent target links only graph, analysis, HDC, and rule modules. Its MatrixGraph, CUDA, and libcurl dependencies are private implementation details.

## Core data shapes

### Rules and graphs

`GARule` combines a MatrixGraph-compatible pattern with semantic fields that MatrixGraph does not own: predicates, consequent, pivot, quality, descriptions, and dictionaries.

`Graph` separates:

- immutable base arrays and entity/relation dictionaries;
- per-goal derived facts;
- per-goal tombstones;
- dynamic indexes needed by matching and predicate evaluation.

Inverse relation slots are normalized by GARAgent. An inverse edge swaps its endpoints and uses the corresponding inverse slot.

### Actions and matches

A `Motif` is a rule index plus a complete grounding of its pattern vertices. `ActionRecord` adds executor-owned state:

- stable session-local ID and insertion sequence;
- grounded rule head;
- score and admissibility;
- learned selection priority and optional guide priority;
- premise subscriptions used for exact witness updates.

A `MatchRequest` contains a pattern, a candidate cap, optional vertex pins, and an optional delta seed. A `MatchResult` contains complete embeddings and a truncation flag.

These types keep scheduling state out of MatrixGraph and make CPU/GPU matching interchangeable at the executor boundary.

## ReasoningAgent composition

`ReasoningAgent` owns or references the components for one agent configuration:

```text
ReasoningAgent::Components
├── Scorer
├── Policy
├── Executor
├── CostModel
├── ValueModel              optional action cost-to-go model
├── ValueModel              optional proof-star model
├── LlmStarGuide            optional initial star preferences
└── PaperGuide              optional restricted action guide
```

For each run it:

1. returns immediately if the grounded goal is already present;
2. verifies that every ML predicate has a registered model;
3. computes or injects dependency distances to the goal;
4. creates per-run witness state;
5. invokes the configured executor.

The implementation is in `core/engine/agent/include/gar/core/agent/reasoning_agent.h`.

## AdaDeduce execution

`AdaDeduceExecutor` is the paper's Algorithm 6 implementation. `DeduceSession` owns the mutable state for one goal.

### Main loop

1. Build execution plans for goal-reachable rules.
2. Generate complete matches and supported partial matches.
3. Score complete actions and insert them into the ready set `R`.
4. Select one action or an ordered top-k batch using the configured policy.
5. Evaluate all exact preconditions.
6. Derive a new head fact if every precondition holds.
7. Suspend actions with missing dynamic preconditions or deferrable pattern edges.
8. Propagate derived deltas to matching and waiting actions.
9. Stop at the goal, an explicit limit, or exhaustion.

### Ready and suspended sets

`WorkingSet` owns two different collections:

- `R` contains complete actions eligible for policy scheduling.
- `W` contains actions waiting for a dynamic predicate, a ground pattern fact, or completion of an open-side partial match.

W is not a second policy queue. Revival is driven by arrived facts and producer liveness. Once all requirements of a suspended action hold, completion can derive its head without sending it through policy selection again.

The ready heap uses guide priority as an optional first key and learned/handcrafted selection priority as the next key. Position or admissibility updates must preserve the existing guide value.

### Ordered action batches

`Policy::SelectBatch` returns ordered candidate indices from a batch-start view. Deterministic, Random, Learned, and Guided policies share this interface. The executor evaluates selected actions in order and reuses static precondition results where possible.

Batch size one retains a heap-based fast path. Action batching must not be confused with multi-goal `agent-batch` execution.

## Policy variants

| Variant | Composition |
|---|---|
| Deterministic | Handcrafted score and deterministic maximum |
| Random | Seeded softmax over the handcrafted score |
| Learned | LearnedQPolicy with symbolic admissibility filtering |
| AdaDeduce | Learned policy plus an optional compiled PaperGuide |
| AdaDeduceStar | Selected action policy plus proof-efficiency star ordering |

`--scorer null` is a scoring ablation. It is not equivalent to DepOnly, which uses the fixed-order goal-cone Chase.

## Matching backends

`MatchBackend` exposes two operations:

```cpp
std::vector<MatchResult> RunBatch(
    const std::vector<MatchRequest>& requests);

void CommitDerived(
    const gar::core::graph::DerivedEdge& derived);
```

The CPU backend uses GARAgent's exact host-side matching. The GPU backend uses MatrixGraph components and GARAgent-owned dynamic GPU matching. Policy evaluation, ML predicates, R/W scheduling, and derivation remain on the host.

To add another backend, implement `MatchBackend` in `core/engine/agent/src` and extend the factory without changing policy or scorer code.

## MatrixGraph boundary

MatrixGraph provides:

- immutable graph and pattern arrays;
- attribute storage and I/O;
- generic matching primitives;
- CUDA buffers, streams, and kernels;
- yaml-cpp and oneTBB build dependencies.

GARAgent owns:

- rule semantics and dependency analysis;
- base-plus-delta graph state;
- partial and delta matching strategy;
- predicate evaluation and ML model registration;
- ready/suspended scheduling and revival;
- scoring, learning, value models, and LLM guidance;
- experiment definitions and correctness checks.

There is no requirement for MatrixGraph to implement `GARAgentMatch`, and agent changes must not alter full-Chase behavior.

## Model boundaries

### Value models

`ValueModel` predicts action cost-to-go for ordering. A serialized model declares a feature schema. The loader rejects a model trained with incompatible features.

A model prediction is never logical evidence. The executor still checks symbolic admissibility and every grounded precondition.

### ML predicates

`MlModelRegistry` maps a model name to an exact batched evaluator. The current CLI includes a constant adapter for controlled tests. Other evaluators can be registered by a downstream application without modifying rule semantics.

### LLM clients and guides

`LlmClient` isolates OpenAI-compatible, local System One, cached, and mock clients.

`PaperGuide` compiles model output into a restricted, typed priority language. `GuideFeatureCatalog` defines every feature the model may reference. Unknown features, invalid options, and malformed responses are rejected.

See [jev_guide.md](jev_guide.md) for the three model entry points and their protocols.

## State ownership

| State | Owner | Sharing rule |
|---|---|---|
| Base graph and dictionaries | Immutable application inputs | Shared across goals |
| Derived delta and tombstones | Per-goal `AgentState` | Never shared between workers |
| Dependency graph | Immutable rule-set analysis | Shared across goals |
| R, W, producer state | Per-goal `DeduceSession` | Never shared |
| Policy RNG and action priorities | Per-goal policy instance | Never shared |
| Witness and provenance | Per-goal execution history | Never shared |
| CPU/GPU scratch state | Match backend | Backend-specific |
| Loaded value model | Agent components | Read-only sharing is allowed |
| LLM response cache | Explicit external cache path | Keyed by goal, not by proof state |

## Cost accounting

`AgentResult` distinguishes:

- `logical_cost`: symbolic work charged to the configured budget;
- `execution_cost_ms`: measured non-overlapping executor phases;
- `proof_cost`: distinct ancestors in the goal proof DAG;
- `AgentResult::derived`: all facts derived during the search, including facts outside the goal proof.

CLI wall time includes additional work such as graph loading, initialization, online model calls, and output. An online guide consumes wall time but does not consume the logical action budget.

## Application layer

`gar_reason` provides Chase, DepOnly, single-goal agent, and multi-goal agent modes. `gar_analyze` provides dependency statistics, workload generation, chain comparison, and closure verification.

The CLI currently performs argument parsing and most component construction in `tools/cli/agent_mode.h`. The umbrella `gar::core` target also includes `gar_miner` from `tools/miner`. These are current code-organization facts, not additional semantic layers.

## Extension guidelines

- Add a policy by implementing `Policy` and preserving symbolic admissibility.
- Add a scorer by implementing `Scorer` without mutating execution history.
- Add a value model only with an explicit feature schema and training manifest.
- Add a match backend through `MatchBackend`, not by adding policy logic to MatrixGraph.
- Add an LLM protocol through `LlmClient`; do not introduce Python or model-weight dependencies into the C++ core.
- Keep CPU as the default agent backend. GPU experiments must select it explicitly.
- Record caps, timeouts, fallbacks, and resource failures in experiment output.
- Follow the surrounding Google C++ style: PascalCase types, snake_case functions and variables, `k`prefixed constants, and self-contained headers.

## Validation expectations

A change is not validated by compilation alone. Depending on the touched layer, run:

- a full Release build;
- the synthetic Chase and agent workflow from the README;
- `gar_analyze verify` against the exact closure;
- CPU/GPU closure comparison for matching changes;
- single-goal and multi-worker comparison for graph-state changes;
- model true/false/unregistered cases for ML-predicate changes;
- mock and real-service runs for guide changes;
- repeated end-to-end measurements with caps and failures included for performance changes.
