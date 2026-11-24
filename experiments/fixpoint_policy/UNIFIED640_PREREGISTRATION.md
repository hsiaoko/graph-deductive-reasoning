# Unified GoalChase / Deterministic / Random / Learned (rules640)

Date: 2026-11-08.

This preregistration froze one protocol so that older reports would not mix single runs, three repetitions, fair intervals, and execution batches. Older experiments remain separate and are not merged into the new tables.

This is a historical protocol. The retired `--executor fixpoint` option corresponds to the current `--mode agent`, and `--fixpoint-star-order goal` corresponds to `--star-order goal`.

## Inputs and shared execution parameters

- Graph: `/ssd_data/zhuxk/graphs/KG/fb15k-237/immutable_csr`.
- Rules: `.work_hdcv/fixpoint_rules640/rules640`, containing 640 rules.
- Test goals: the same 36 goals in `.work_hdcv/fixpoint_rules640/goals.tsv`.
- Ground truth: the complete Chase closure in `.work_hdcv/fixpoint_rules640/runs/chase/rep1`.
- All methods use goal star order.
- Shared limits: budget `1,000,000,000`, candidate cap `100,000`, fair interval `0`, and `print=0`.
- All methods use policy seeds `{7,13,42}`. Deterministic, GoalChase, and Learned do not consume policy randomness, so these are repeated timing runs of the same logical search. Random produces different sampled trajectories.
- Each run has an external limit of 1000 seconds and an internal limit of 900 seconds. Process-tree and system-used memory limits are both 256 GiB.
- Runs are serial. A new run starts only when system-used memory is below 210 GiB. No other user's process is terminated.

## Four method arms

Only action selection and the scorer/model required by that selection differ.

1. GoalChase: `--policy deterministic --scorer null`.
2. Deterministic: `--policy deterministic --scorer exact`.
3. Random: `--policy random --scorer exact`. Online Gumbel-max is distributionally equivalent to `softmax(total/tau)`.
4. Learned HDC relevance: `--policy learned --scorer exact --relevance-model .work_hdcv/relevance640/models/selected.meta --hdc-dim 128`.

All arms share dependency-goal filtering, candidate generation, exact precondition evaluation, fact derivation, suspension/revival, and early goal termination. Rules, goals, training data, and models are not replaced after seeing test results.

## Run count and aggregation

- Four arms × 36 goals × 3 seeds = 432 goal-directed runs.
- Chase computes the full closure and is not part of the five-arm per-goal matrix. Its three existing cold runs provide a non-equivalent work reference only.
- For each goal and method, take the median over seeds `{7,13,42}`, then take an equally weighted macro average over the 36 goals.
- Primary metrics are external wall time, internal wall time, selection branches, ready insertions, match embeddings, evaluated actions, derived facts, goal hits, soundness, caps, timeouts, and RSS.
- Selection branches include zero-cost skips of known conclusions. Real evaluations are reported separately and must not be conflated with them.
- The representative 36-goal time is the sum of per-goal medians. Actual total wall time over all 108 runs per arm is reported separately.
- Every derived fact must belong to the complete Chase closure. Caps, timeouts, misses, and OOM remain in the report; limits are not increased to replace inconvenient results.

## Outputs

Write unified outputs to `.work_hdcv/fixpoint_unified640/` without overwriting older experiments. The driver and aggregation script is `experiments/fixpoint_policy/run_unified640.py`.
