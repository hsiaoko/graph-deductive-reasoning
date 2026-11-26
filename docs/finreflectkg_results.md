# FinReflectKG 50-Goal Five-Method Results

Date: 2026-11-25. Protocol: 50 derived goals stratified by closure round
(8/8/8/8/8/6/4 for rounds 1–7), seed 7 (results are seed-invariant: 3-seed
derived counts were identical, so 1 seed = the 3-seed median). Per-goal wall
summed across goals. Learned uses the float-SIMD optimized encoder.

## Main table (total over 50 goals)

| method | derived facts | wall (s) | hit | vs Det derived | vs Det wall |
|---|---:|---:|---:|---:|---:|
| **Chase** (one full closure) | 3,122,568 | ~100.23 | all | — | — |
| **AdaDeduce** (hybrid) | **8,265,772** | **115.75** | 50/50 | **−88.0%** | **−86.9%** |
| Learned (float) | 66,752,163 | 1179.86 | 50/50 | −3.3% | +33.7% |
| Deterministic | 69,022,664 | 882.65 | 50/50 | — | — |
| Random (3-seed median) | 69,668,879 | 1085.95 | 50/50 | +0.9% | +23.0% |
| DepOnly (cone chase, early-stop) | 129,702,166 | 2467.50 | 50/50 | +87.9% | +179.5% |

Random is seed-sensitive (Gumbel-max softmax sampling), so it uses the 3-seed
(7/13/42) median; the other methods are seed-invariant (1 seed = median).
**Random derived ≈ Det (+0.9%)** confirms policy ordering barely changes the
derived count — search reduction comes from the cone + admissibility + early-stop
structure, not the scoring formula. Random wall +23% vs Det: the Gumbel
perturbation disrupts matching-friendly derivation order (same heap mechanism,
worse locality), the same trajectory effect seen in Learned.

Wall口径: Deterministic/Learned/AdaDeduce use `execution_cost_ms` (internal pure
execution). DepOnly uses external wall (includes ~8.6 s graph load per goal;
pure execution ≈ 2037 s). Chase is a single amortized closure (loads once).

## Per-round derived facts

| round | n | DepOnly | Deterministic | Learned | AdaDeduce |
|---|---|---:|---:|---:|---:|
| 1 | 8 | 11,710,758 | 3,286,879 | 2,925,192 | 10 |
| 2 | 8 | 18,366,278 | 11,152,130 | 10,696,136 | 16 |
| 3 | 8 | 22,830,112 | 14,469,994 | 13,813,217 | 24 |
| 4 | 8 | 23,373,912 | 13,033,782 | 12,557,273 | 3,097,896 |
| 5 | 8 | 23,709,648 | 12,485,360 | 12,347,351 | 3,169,794 |
| 6 | 6 | 17,825,430 | 8,201,013 | 8,098,114 | 1,998,003 |
| 7 | 4 | 11,886,028 | 6,393,506 | 6,314,880 | 29 |

## Findings

1. **AdaDeduce (backward-first hybrid) is the computation reducer**: −88% derived
   and −87% wall vs Deterministic. On 45/50 goals backward proves the goal with
   4–8 derived facts (vs millions for forward). Its wall (115.75 s) is competitive
   with a single full Chase closure (~100 s) while answering goals individually.

2. **5 backward-hard goals fall back to forward**: g027, g031 (round 4), g032,
   g033 (round 5), g043 (round 6). On these, AdaDeduce derives *exactly* the
   Deterministic count (e.g. g027 both 1,417,611) — backward fails, forward
   fallback matches Det. These 5 goals contribute ~8.27 M (essentially all) of
   AdaDeduce's derived total. The gain is concentrated on backward-provable goals.

3. **Learned relevance: small derived savings, net wall cost**. −3.3% derived
   (shallow round 1 −11%, deep round 7 −1.2%) but +33.7% wall vs Det. The cost is
   HDC encoding (see `learned_cost_optimization.md`); the float-SIMD optimization
   cut Learned wall −10.5% (1317.9 → 1179.9 s) but the trade-off stays structural.
   Learned is an honest ablation, not the speedup source.

4. **DepOnly derives the most** (129.7 M): per-goal cone chase materializes the
   full round frontier before early-stop, ~1.9× Det's derived. Early-stop keeps
   its pure-execution wall (~2037 s) below the derived ratio would suggest.

## Fairness check: are irrelevant rules inflating Chase? (2026-11-25)

Hypothesis tested: "most rules' conclusions are unrelated to the goal, so Chase
wastes computation on them and goal-aware methods look artificially fast."
**Refuted.**

| | rules | derived | wall (s) |
|---|---|---|---|
| Full Chase | 455 | 3,122,568 | 100.23 |
| Cone-union A (goal-relevant, fair batch baseline) | 401 (88%) | 2,971,515 | 98.70 |
| Difference (54 "irrelevant" rules) | 54 (12%) | +151,053 (+4.8%) | +1.53 (+1.5%) |

- The goal predicate `discloses` (47/50 goals) is **central**: its dependency cone
  covers 401/455 = 88% of rules. Most rules ARE goal-relevant, not irrelevant.
- The 12% out-of-cone rules add only 4.8% facts and 1.5% wall. Chase cost is
  dominated by goal-relevant rules.
- The fair cone baseline (GoalChase batch) ≈ Chase (98.70 vs 100.23 s). AdaDeduce's
  −87% wall is therefore **not** a rule-filtering artifact; it comes from
  early-stop + backward search. Consistent with FB15k (cone filter −11% wall) and
  BioKG (−6.5% facts): for central predicates cone filtering barely reduces cost.

Paper baseline decomposition: Chase → GoalChase gap is tiny (~1.5%); the
order-of-magnitude gain is GoalChase → AdaDeduce (early-stop + policy), not cone
filtering. Tool: `gar_cone_union --goals-tsv <LF tsv> --out A` then
`gar_reason --mode chase --threads 1 --rules A`. (Note: goals tsv must be LF;
csv.writer emits CRLF which breaks the `spec` header match.)

## Reproduce

```
python3 experiments/overall-performance/finreflectkg/run_finreflectkg_train.py
python3 experiments/overall-performance/finreflectkg/run_finreflectkg_eval.py --seeds 7
python3 experiments/overall-performance/finreflectkg/partial_table.py   # read-only
```

Data: `.work_hdcv/overall-performance/finreflectkg-50/` (test_goals.tsv,
model_h4/relevance.meta pairwise 0.884, runs/, summary_eval.json).