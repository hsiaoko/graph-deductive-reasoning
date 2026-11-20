# OGBL-BioKG overall-performance experiment design

Status: **DESIGN ONLY. This document does not authorize launching the experiment or inserting results.**

Date: 2026-11-17.

## 1. Purpose and scope

Compare five methods on one BioKG graph, one frozen rule set, and the same 36 grounded test goals:

1. **Deterministic**: deterministic action selection using the exact handcrafted score.
2. **DepOnly**: dependency-cone reasoning with every other optimization disabled.
3. **Random**: softmax action selection using the exact score.
4. **Learned**: action-level numeric-Q that predicts remaining evaluation cost.
5. **AdaDeduceStar**: the proof-star extension with learned proof-efficiency star ordering, exact deterministic action selection, and executor optimizations. Learning may reorder within a goal-distance bucket but cannot promote a high-yield distant star across buckets.

This experiment does not use an LLM or JEV. Chase produces ground truth and a non-equivalent work reference; it is not ranked as a sixth goal-directed method.

The AdaDeduce executor is shared by several agent arms. **AdaDeduceStar** names this proof-star variant, while **Learned** names action-level numeric-Q. They are not interchangeable.

## 2. Frozen inputs

- Graph: `/ssd_data/zhuxk/graphs/KG/ogbl-biokg/immutable_csr`.
- Size: 93,773 entities, 51 base relations, and 4,762,677 deduplicated base edges.
- Vertex types: drug, protein, disease, sideeffect, and function.
- Original rules: `/ssd_data/zhuxk/graphs/KG/ogbl-biokg/rules`.
- Original rule count: 56,601 typed rules, mined with minimum support 200 and minimum confidence 0.3.

All methods share exact rule semantics, vertex-type checks, predicate evaluation, and goal-hit semantics. DepOnly deliberately disables optimizations other than dependency filtering.

Do not mix FB15K-237 rules, goals, models, or ground truth into this experiment.

## 3. Rule budget and deterministic selection

The main rule budget is at most **6,528 rules**:

```text
51 base relations × at most 128 rules per consequent relation = 6,528
```

This is an upper bound, not a quota to fill. Unused capacity is not transferred to high-frequency relations.

An earlier 32-rules-per-relation pilot froze 720 rules. Chase reached a fixpoint, but the deep closure contained only two consequent relations and could not support deep training plus relation-disjoint testing. The quota was therefore increased to 128 per relation, with rules and ground truth rebuilt in an independent work directory.

The full 56,601 rules are not used directly because this is a policy comparison, not a rule-mining coverage evaluation. The BioKG rule budget also need not match FB15K-237 because relation count and typed-rule structure differ.

Selection follows this deterministic procedure:

1. Remove unsupported rules and record every removal reason without weakening semantics.
2. Group rules by normalized base consequent relation.
3. Stratify by body length, vertex-type signature, dependency recursion, and dependency depth. Interleave strata deterministically.
4. Order within a stratum by stable hash, not confidence or support.
5. Keep at most 128 rules per consequent group without using goals or method results.
6. Freeze `rule_ids.txt`, the rule directory, and SHA-256 before generating ground truth or training data.

Rule-selection salt: `20261117-biokg-overall-rules-v2`.

Before execution, record the actual rule count, per-consequent counts, body-length distribution, and type-signature count. Unexpected structure requires a design revision rather than a silent adjustment.

## 4. Ground truth and difficulty strata

Run full Chase on the frozen rule set until fixpoint and preserve:

- the complete derived closure;
- each fact's first Chase derivation round;
- graph, rule, and closure SHA-256.

If full Chase cannot reach fixpoint within the resource envelope, mark ground truth **unavailable**. Do not use a partial closure for soundness or completeness claims.

Difficulty strata are:

- `round1`: first derived in round one;
- `round2`: first derived in round two;
- `deep`: first derived in round three or later.

Derivation round is a proxy, not a strict shortest-proof distance. Goals must be grounded positive facts absent from the initial graph but present in the complete closure.

## 5. Test-goal construction

Freeze 36 test goals before evaluating any method, with 12 goals per stratum:

| Test cohort | round1 | round2 | deep | Total |
|---|---:|---:|---:|---:|
| Instance-disjoint | 8 | 8 | 8 | 24 |
| Relation-disjoint | 4 | 4 | 4 | 12 |
| Total | 12 | 12 | 12 | 36 |

Instance-disjoint goals do not appear as training goal specs and may share consequent relations with training goals. Prefer predicate diversity, with at most two test goals per consequent relation.

Relation-disjoint goals come from a small stable-hash sample of held-out consequent relations. Exclude every training goal and training trace for those relations.

Candidates are ordered by stable hash, never by hit rate, wall time, derived count, or method outcome. Do not remove difficult or unfavorable goals.

- Test salt: `20261117-biokg-overall-test-v1`.
- Relation split salt: `20261117-biokg-overall-relation-split-v1`.

If the cohort constraints are unsatisfiable, report that before execution and revise the design explicitly.

## 6. Training goals and model isolation

Select another 24 training goals, eight per stratum. They must:

- differ from every test goal spec;
- exclude all relation-disjoint test relations;
- avoid sharing chains, proofs, Q traces, or derivation journals with test goals;
- use the independent salt `20261117-biokg-overall-train-v1`.

### Learned numeric-Q

Run Deterministic and Random(seed=7) continuations for each training goal. Labels are the remaining actual evaluations from the selected action through goal arrival.

Sample at most 256 evenly spaced rows per successful trajectory. Do not assign artificial finite labels to failed trajectories or unobserved alternative actions. Fit features and normalization using training goals only.

### AdaDeduceStar proof-star model

Extract proof DAGs from successful training chains and aggregate proof hits and total derived facts by rule. The efficiency label is:

```text
(proof_hits + 1e-6) / (1 + total_facts)
```

Absence from one extracted proof does not prove that a rule is logically useless. The runtime score combines proof membership, predicted log-work, and support/body/distance interactions. Fit every feature using training trajectories only.

`work_weight=0.1` is selected on held-out training goals and frozen without BioKG test-goal tuning. `proof_star_weight=0.5` enables the ordering.

Stars are bucketed by consequent-to-goal dependency distance. Direct goal-head stars use proof-efficiency order. For upstream buckets, retain the first baseline star as an anchor and blend efficiency into the remainder. Cross-bucket promotion is forbidden. Initially `proof_star_min_cost=0`.

Persist `trained_on`, feature version, training rows, seed, and model hash for both models.

## 7. Method configurations

| Method | Configuration |
|---|---|
| DepOnly | `--mode dep-only`; fixed input order, single-threaded goal-cone Chase, no scoring, learning, ready heap, delta optimization, precondition cache, or R/W scheduler |
| Deterministic | `--policy deterministic --scorer exact --star-order goal` with the optimized executor and no learning |
| Random | `--policy random --scorer exact --star-order goal` with score-based softmax |
| Learned | `--policy learned --scorer exact --star-order goal --value-model <biokg-q-numeric>` |
| AdaDeduceStar | `--policy deterministic --scorer exact --star-order goal --proof-star-model <biokg-proof-efficiency-model> --proof-star-weight 0.5 --proof-star-min-cost 0` |

`--scorer null` still uses the optimized AdaDeduce executor and cannot represent DepOnly. All arms use CPU and no LLM.

Shared parameters:

- budget `1,000,000,000`;
- candidate cap `100,000`;
- fair interval `0`;
- `print=0`;
- internal wall limit 900 seconds and external limit 1000 seconds;
- process-tree and system-used memory limits of 256 GiB;
- serial execution, starting only when system-used memory is below 210 GiB;
- never terminate another user's process.

Resource guards are not reasoning optimizations. Goal-directed early termination is the shared task semantics.

## 8. Run count and aggregation

Five methods × 36 goals × seeds `{7,13,42}` gives **540 goal-directed runs**.

Deterministic, DepOnly, Learned, and AdaDeduceStar do not consume action-policy randomness; their three runs are timing repetitions. Random produces different trajectories, and every seed must be retained.

For each goal and method, take the seed median and then an equally weighted macro average over the 36 goals. Also aggregate separately by:

- round1, round2, and deep;
- instance-disjoint and relation-disjoint cohorts.

Report:

- external and internal wall time;
- evaluated actions and derived facts;
- selection branches, ready insertions, and match embeddings;
- goal hits, soundness, timeouts, caps, OOM, and RSS;
- model inference time and training size for learned arms.

Selection branches include zero-cost known-head skips and are not real evaluations. DepOnly does not have ready-heap or suspension metrics; report those fields as N/A rather than zero. Preserve every cap, timeout, miss, and OOM.

## 9. Correctness and reporting

- Check every derived fact against the complete Chase closure from the same frozen rules.
- Do not obtain speed by dropping logical facts, bypassing type constraints, or fabricating an early goal.
- Compare paired per-goal wall time and work, not only macro averages.
- Ask whether AdaDeduceStar reduces deep-goal work enough to produce an end-to-end benefit over Deterministic and Learned.
- Report relation-disjoint and instance-disjoint results separately.
- Publish the full distribution, per-goal table, and actual total runtime.

## 10. Expected outputs after execution

```text
.work_hdcv/overall-performance/ogbl-biokg/
  rules/                 # Frozen rules and hashes
  ground_truth/          # Complete Chase closure
  goals.tsv              # 36 test goals with cohort and stratum
  train_goals.tsv        # 24 frozen training goals
  models/                # Numeric-Q and proof-star models with manifests
  runs/                  # 540 raw runs
  rows.tsv
  paired.tsv
  summary.json
  summary.md
```

This directory currently stores only the experiment design. It contains no BioKG performance results.
