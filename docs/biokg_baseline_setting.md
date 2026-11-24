# ogbl-biokg Baseline Experiment Setting (for review)

Six-method comparison on ogbl-biokg, 50 held-out goals, 720 GARs. Draft setting
for approval before the formal run. Protocol follows UNIFIED640.

## 1. Dataset and rules

| item | value |
|---|---|
| graph | ogbl-biokg immutable CSR (`/ssd_data/zhuxk/graphs/KG/ogbl-biokg/immutable_csr`) |
| vertices | 93,773 |
| directed edges | 4,762,677 (CSR materializes both directions: 9,525,354) |
| relations | 51 |
| rule library | **720 GARs** (frozen, `.work_hdcv/overall-performance/ogbl-biokg/rules`) |
| ground truth | chase closure (`.work_hdcv/overall-performance/ogbl-biokg/ground_truth`) for soundness |

## 2. Goal set (50, held-out)

`goals.tsv` in `.work_hdcv/overall-performance/ogbl-biokg-50/`:

| stratum | count | meaning |
|---|---|---|
| round1 | 18 | goal derivable in chase round 1 (shallow) |
| round2 | 18 | round 2 (medium) |
| deep | 14 | round 3+ (deep) |

Train goals (24, `ogbl-biokg/train_goals.tsv`) are disjoint from these 50; the
Learned relevance model and LearnedStar selector are trained on train goals only.

## 3. Methods (six) and three-level mapping

Layers: **engine** (`--search-direction`) / **schedule** (`--star-order`) /
**policy** (`--policy`) / scorer (`--scorer`).

| method | engine | schedule | policy | scorer | extra flags |
|---|---|---|---|---|---|
| noScoring | forward | goal | deterministic | **null** | — |
| Deterministic | forward | goal | deterministic | exact | — |
| Random | forward | goal | **random** (softmax, heap) | exact | — |
| Learned | forward | goal | **learned** (relevance) | exact | `--relevance-model model_cos_rank_h4/relevance.meta --learned-handcrafted-weight 0 --learned-relevance-weight 1` |
| LearnedStar | forward | **learned** | deterministic | exact | `--star-order-model star_order.model` |
| AdaDeduce | **hybrid** (backward+fallback) | goal | deterministic | exact | `--backward-max-calls 100000 --backward-max-depth 256` |

Notes:
- `Random` uses `--policy random` = RandomizedPolicy (softmax, Gumbel-max, heap
  selection). NOT `--policy uniform-random` (full O(|R|) rescan; its wall is a
  selection-machinery artifact, not a strategy effect).
- `LearnedStar` is a schedule-layer selector (goal-level logistic on cone_size +
  log1p degree choosing goal/input order), pure forward, no backward.
- `AdaDeduce` = hybrid executor (backward-first, forward fallback). On BioKG
  backward solves all 50 goals in single-digit facts.

## 4. Common run flags

```
--mode agent --engine cpu --csr <CSR> --rules <rules> --goal <spec>
--star-order goal            # except LearnedStar (learned) / hybrid fallback
--scorer exact               # except noScoring (null)
--budget 1000000000 --wall-limit 900
--max-candidates-per-rule 10000000   # no-cap (cap 1e5 truncates -> hit 0.92 artifact)
--hdc-dim 128 --fair-interval 0 --policy-seed <seed>
```

## 5. Protocol (UNIFIED640)

- **3 seeds** (7, 13, 42) per goal per method = 50 × 6 × 3 = **900 runs**.
- Per goal: take the **median** of the 3 seeds (derived and wall).
- **Total derived / total wall = sum of per-goal medians** across the 50 goals
  (the "answer all 50 queries" cost), reported overall and per stratum.
- Deterministic/noScoring/LearnedStar are seed-invariant (fair_interval 0);
  Random and the hybrid backward tie-breaks vary by seed.

## 6. Metrics

- total derived (sum of per-goal medians), overall + per stratum.
- total wall seconds (same aggregation).
- hit rate (goal_derived true), soundness (derived ⊆ ground-truth closure, must be 0 failures).
- wins vs Deterministic (per-goal derived fewer).

## 7. Reference baselines (context, not re-run here)

| baseline | meaning | prior BioKG total |
|---|---|---|
| Chase | all 720 rules to fixpoint | (full closure) |
| GoalChase | goal-cone rules to fixpoint (`--mode chase --goal`) | cone closure |
| DepOnly | cone chase + early-stop (`--mode dep-only`) | 580.89s / 22.4M |

## 8. Prior measured results (informal, to be re-confirmed by this run)

| method | total derived | vs Det | note |
|---|---|---|---|
| Deterministic | 16,941,089 | — | goal order |
| noScoring | ≈16.94M | ±0.03% | weight no-op |
| Learned (relevance) | 16,188,598 | −4.4% | round1 −50%, wall +36% |
| LearnedStar | 15,898,508 | −6.2% | ≈ input order; oracle −16.9% |
| AdaDeduce (backward) | 194 | ~−99.99% | 50/50 hit, mean 4 facts |
| Random | pending | — | this run |

## 9. Open items before run

- Confirm Random arm uses softmax (`--policy random`), heap-based.
- Confirm whether to also report Chase/GoalChase/DepOnly reference rows.
- Output dir: `.work_hdcv/overall-performance/ogbl-biokg-baselines/`.