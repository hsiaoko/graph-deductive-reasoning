# Learned Policy Cost Optimization — Diagnosis & Results

Date: 2026-11-25. Goal: reduce the learned relevance policy's wall overhead so
that cost < benefit (it derives fewer facts but was slower than Deterministic).

## 1. Corrected diagnosis: the cost is HDC encoding, hidden in generation_wall

Earlier framing ("scoring is only ~1% of wall; the overhead is matching
locality") was a **measurement artifact**.

- `LearnedPolicy::SelectionPriorityForCandidate` runs at **ready-insertion time**
  (`deduce_session.cpp` ~line 280), and insertion happens **inside `GenerateStar`**.
  Its HDC encode/project/cosine cost is therefore booked to `generation_wall_ms`,
  **not** `score_wall_ms`. `score_wall_ms` only measures the `SelectReady` scoring
  path and misses the insertion-time encoding.

Proof (FinReflectKG g011, identical derived set 1,343,345):

| config | generation_wall | execution |
|---|---|---|
| Deterministic | 4066 ms | 15907 ms |
| Learned hw=1 rel=0 (still calls Predict) | 8234 ms | 20685 ms |
| Learned hw=1 rel=0 (**short-circuit skips Predict**) | **4066 ms** | 14917 ms |

Skipping Predict drops generation to exactly the Deterministic level. The ~4168 ms
gap was entirely HDC encoding.

## 2. Encoding breakdown (g000, 359,289 unique facts, GAR_DERIVE_PROFILE)

| component | double scalar | double SIMD | float SIMD |
|---|---|---|---|
| encode (2 Add + Normalized + cache reads) | 491.5 ms (62%) | 422.0 ms | **333.0 ms** |
| cosine (action·goal Dot) | 246.5 ms (31%) | 216.3 ms | **202.9 ms** |
| project (MLP hidden, already SIMD) | 56.2 ms | 58.5 ms | 48.4 ms |
| predict (per-call MLP) | 85.5 ms | 83.5 ms | 80.9 ms |

Encode dominates and is **memory-bandwidth-bound** (unordered_map cache lookups +
random 1 KB double-vector reads), not compute-bound.

## 3. Optimizations applied

1. **DenseVector double → float** (`core/hdc`): halves per-vector traffic
   (1 KB → 512 B) and doubles SIMD lane count. `Add/Dot/Scale` use AVX2/AVX512
   float kernels with runtime dispatch (no global `-march`). Dot accumulates in
   float lanes, widens to double at the end.
2. **Mixed-precision projection** (`relevance_model.cpp` `DotMixed`): double model
   weights × float fact vectors, widening float→double in-register. Weight
   bandwidth (small, cached) stays double; fact bandwidth (the bottleneck) halves.
3. **rel=0 short-circuit** (`learned_policy.cpp` `Score`): skip Predict when
   `relevance_weight_ == 0` (it cannot affect priority but still encoded).

Correctness: derived sets **identical** to double on g000 (348,241) and g011
(1,192,080); HdcScorer evidence path runs without crash/NaN.

## 4. Wall result (FinReflectKG g011, clean, no contention)

| | execution | generation | vs Det wall |
|---|---|---|---|
| Deterministic | 15907 ms | 4066 ms | — |
| Learned (double, original) | 20950 ms | 8729 ms | +31.7% |
| Learned (float SIMD) | **18722 ms** | 7637 ms | **+17.7%** |

Float reduces Learned execution **−10.6%** (encode −32%, cosine −18%).

## 5. Structural conclusion

The trade-off remains **structural**: the policy encodes ~one vector per unique
candidate fact (≈ derived count), while the benefit is only 3–11% fewer
derivations. Even with float, Learned wall is +17.7% vs Det on g011 for −11%
derived — cost still exceeds benefit. Making encoding free would barely flip deep
goals (consistent with prior BioKG findings).

The genuine computation reducer is **backward search (AdaDeduce)**: −88% derived,
−87% wall on FinReflectKG (see `finreflectkg_results.md`). Learned relevance is an
honest ablation, not the source of the speedup.

### Failed direction (removed)
Predicate-bucketed priority (`rel_fam` primary + ε·relevance) did **not** reduce
generation_wall — encoding happens regardless of selection order. It was based on
the wrong "matching locality" diagnosis and has been reverted.