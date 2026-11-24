# BioKG Policy/Scoring Ablation Findings (2026-11-23/24)

Process record of the scoring/policy ablation campaign on ogbl-biokg (50 goals,
UNIFIED640 protocol: 3 seeds 7/13/42, per-goal median summed). Data under
`.work_hdcv/overall-performance/ogbl-biokg-{cosformal,score-abl50,score-time12,
star-order,learnedstar,backward50,overfit,generalize,value}/`.

## Three-level taxonomy (confirmed against code, no refactor needed)

`config.h` already separates three independent enums:

- **Engine** (`SearchDirection`): forward / backward / hybrid / learned-strategy.
- **Schedule** (`StarOrder`): input / goal / learned (LearnedStar) / proof-star / LLM order.
- **Policy** (`PolicyKind`): deterministic / random / learned-relevance / ...

LearnedStar is a *schedule*-layer selector (which star to expand), not a
within-ready-set policy. The LLM guide is cross-cutting (rule order + predicate
prior + match priority).

## Verified findings

1. **Handcrafted score weights are ~no-op for derived.** Eight weight arms
   (full / dg-only / evidence-only / cost-only / no-cost / no-evd / no-dg / null)
   give total derived within ±0.03%; null (f=0, enumeration order) ≈ full. Only
   DG `rel_fam` has a marginal effect; evidence and cost are no-ops. `rel_fam` is
   constant within a star (shared consequent predicate), redundant with the
   goal star order — hence no-op at the within-star level.

2. **Det-vs-Random wall gap is selection machinery, not strategy.** p01 Det and
   uniform-random have identical ready_peak (125267) and ~equal derived (~68K);
   selection wall is 74ms (Det, heap O(log n)) vs 391s (uniform-random, full
   O(|R|) rescan). The paper's `--policy random` is `RandomizedPolicy` (softmax,
   heap-based) and is fair; `--policy uniform-random` is the full-scan variant.

3. **Goal star order helps shallow, hurts deep.** goal-vs-input derived ratio:
   round1 3.10x (goal better), round2 0.85x, deep 0.81x (input better), total
   0.94x. The main gain over Chase/GoalChase is **early-stop**, not the star
   order per se. Best-of-two oracle (pick better order per goal) = −16.9% derived.

4. **LearnedStar (cheap goal-level logistic on cone_size/degree) ≈ input order.**
   Trained on 24 goals (labels perfectly stratified round1→goal, round2/deep→input)
   but CV accuracy only 0.59: the order choice tracks instance-level derivation
   depth, which predicate-level features cannot predict (same predicate spans all
   strata). Measured −6.2% vs goal (= the free input-order gain), +0.0% vs input;
   it mis-selects round1 (3.085x worse than goal). The −16.9% oracle headroom needs
   instance-level depth (backward probe) and converges to hybrid.

5. **Learned relevance (cosine) is a real but shallow gain.** Explicit
   cosine(v_mu,v_g) feature + ranking loss fixes the additive-architecture
   bottleneck (pairwise 0.499→0.809). Formal: −4.4% macro derived, −50.2% round1
   (several goals collapse to near-proof), −1.4% deep; wall +36.5% (per-fact HDC
   encoding cost). Overfit probe: representation has full expressiveness (shallow
   goals memorize to derived=1); bottleneck is cross-goal generalization + deep
   premise closure.

6. **Predicate prior rho_q is a derived no-op** even with oracle values
   (boost/suppress/baseline proof predicates give identical derived). It reorders
   within a distance level, which does not change the premise closure size.

7. **Backward dominates BioKG.** Pure backward solves 50/50 goals, mean 4 facts,
   total derived 194 / wall 1.92s (vs Det 16.9M / 278.83s). Deep goals solved in
   single-digit facts. The FB15k "backward fails on central/deep predicates"
   boundary does not transfer to BioKG.

## Implication for the paper

- Main gain claim: early-stop + backward/hybrid (structure), not score weights.
- Learned contribution: shallow-goal instance relevance (stratified −50%) and the
  goal-level strategy selector; report stratified, not only the −4.4% macro.
- Honest negatives: handcrafted weight / rho_q within-level no-op; deep-goal
  instance ranking has no signal; learned wall trade-off; LearnedStar cheap model
  captures only the free input-order gain.