# FinReflectKG Expanded Experiment Setting (for review)

Five-method comparison on FinReflectKG, 50 derived goals, recording derived facts
and wall time. Draft for approval before the formal run.

## 1. Dataset

| item | value |
|---|---|
| graph | `/ssd_data/zhuxk/graphs/KG/finreflectkg/immutable_csr` |
| vertices | 2,477,375 |
| edges | 16,981,048 |
| rules | 455 (2-node 160 = 35%, 3-node chain 295 = 65%, chain-miner-v1) |
| full closure | 9 rounds, 3,122,568 derived, 48,360,195 matches, Chase 100.23s (single-thread) |

Closure round distribution (derived facts per round):

| round | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| facts | 2,048,287 | 706,604 | 246,368 | 70,909 | 42,511 | 7,260 | 618 | 11 |

## 2. Goal set (50 derived goals, stratified by derivation round)

Goals are sampled from the Chase `closure_edges.tsv` (derived facts), NOT from
`candidate_test/test.txt` (those positives are base-graph edges, trivially true,
0 derivations — not a deduction test).

Stratified sampling (deeper rounds stress the cascade):

| round | 1 | 2 | 3 | 4 | 5 | 6 | 7 | total |
|---|---|---|---|---|---|---|---|---|
| goals | 8 | 8 | 8 | 8 | 8 | 6 | 4 | 50 |

`closure_edges.tsv` is name-format `src\trelation\tdst\tround\trule`; goal spec is
`relation(src,dst)`. Sampling seed fixed (e.g. 20261124) for reproducibility.
Deep goals (round 5-7, 18 goals) are the cascade stress test; round 8 has only 11
facts so is excluded.

## 3. Methods (five)

| method | command flags | scope |
|---|---|---|
| **Chase** | `--mode chase --threads 1` (no goal) | one full closure answering all goals |
| **DepOnly** | `--mode dep-only --goal <spec>` | per-goal cone chase + early-stop |
| **Deterministic** | `--mode agent --search-direction forward --policy deterministic --star-order goal --scorer exact` | per-goal forward agent |
| **Learned** | `--mode agent --search-direction forward --policy learned --relevance-model <fin_model> --learned-handcrafted-weight 0 --learned-relevance-weight 1` | per-goal forward + learned relevance |
| **AdaDeduce** | `--mode agent --search-direction hybrid` | per-goal backward-first + forward fallback |

Common flags: `--engine cpu --budget 1000000000 --wall-limit 900
--max-candidates-per-rule 10000000 --hdc-dim 128 --policy-seed <seed>
--fair-interval 0`.

## 4. Prerequisite: train a FinReflectKG Learned model

No FinReflectKG relevance model exists (the BioKG model has a different graph
fingerprint and will not load). Steps:
1. Generate Deterministic policy traces on ~100-200 FinReflectKG train goals
   (disjoint from the 50 test goals), `--policy-trace --hdc-dim 128`.
2. `gar_train_policy --label proof --ranking --hdc-dim 128 --hidden 4` (cosine
   feature + ranking loss, the configuration that worked on BioKG).
3. Use the resulting `relevance.meta` for the Learned arm.

## 5. Protocol (UNIFIED640)

- 3 seeds (7, 13, 42) per goal per method.
- Per goal: median of the 3 seeds (derived and wall).
- Total derived / total wall = sum of per-goal medians across the 50 goals,
  reported overall and per round-stratum.
- Chase is a single full-closure run (no seed variance); its wall is the
  one-closure cost answering all goals. DepOnly/Deterministic/Learned/AdaDeduce
  are per-goal sums.
- Deterministic/Learned/AdaDeduce are seed-sensitive only via tie-breaks;
  DepOnly is deterministic (seed-invariant).

## 6. Metrics

- derived facts per goal (and total).
- wall time per goal (and total).
- hit rate (goal_derived true).
- soundness: derived ⊆ Chase closure (must be 0 failures).
- wins vs Deterministic (per-goal derived fewer).

## 7. Expected picture (from pilot)

- Chase 100.23s / 3.12M derived (one closure).
- AdaDeduce (backward) ~19× faster than Chase on round-1 goals (pilot 20 goals
  5.25s); deeper goals stress backward (may fall back to forward).
- Deterministic forward derives the cone frontier per goal (slower than backward
  on shallow, may beat Chase amortization on deep).
- Learned: depends on the trained model quality (BioKG gave −4.4% macro /
  −50% shallow; FinReflectKG TBD).

## 8. Open items before run

- Confirm goal stratification (8/8/8/8/8/6/4) and sampling seed.
- Confirm AdaDeduce = hybrid (vs bidirectional).
- Confirm Learned model training scope (train goal count, hidden size).
- Output dir: `.work_hdcv/overall-performance/finreflectkg-50/`.