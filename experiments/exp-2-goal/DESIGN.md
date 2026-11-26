# exp-2-goal: Safe Relevance Filtering (Proposition 2)

## 0. What this verifies

**Proposition 2 (safe filtering).** Restricting candidate actions to those with
goal relevance `r(g, μ) > 0` removes candidate actions but loses **no derivable
goal**. This experiment quantifies the candidate reduction and confirms zero goal
loss, sweeping the rule count and the goal-irrelevant rule proportion.

Paper text target:
> compare derivable goals under unrestricted chase vs actions with r(g,μ)>0;
> vary #GARs and proportion of goal-irrelevant rules; Fig (a): relevance
> filtering removes X% of candidate actions while losing X derivable goals.

## 1. Definitions grounded in code

- `r(g, μ) > 0` ⟺ `rel_fam = γ^{d(p0)} > 0` ⟺ the consequent predicate `p0` of
  action μ can reach goal predicate `g` in the dependency graph ⟺ μ lies in the
  goal cone. Computed by `analysis::GoalConeRuleIndices(rules, g)`
  (`core/analysis/.../dependency_graph.h`); `d(p0)=kUnreachable` ⇒ `rel_fam=0`.
- **Candidate action** = a pattern embedding that passes the label filter (one
  potential rule application). Chase reports `embeddings_total` (all candidates)
  and `derivation_candidates_total` (candidates yielding a new conclusion).
  We report **both**: `embeddings_total` = candidate actions examined;
  `derivation_candidates_total` = productive candidates.
- **Unrestricted** = chase on the full rule set R (no relevance filter).
- **Filtered (r>0)** = chase on `R ∩ cone(g)` (only goal-relevant rules).
- Safety holds by construction: the cone is closed under premise dependency
  (a rule whose consequent feeds a cone rule is itself in the cone), so an
  out-of-cone rule can never contribute to deriving g (Theorem 5). The experiment
  confirms this empirically and measures the candidate savings.

## 2. Controlled axes

Fix a goal predicate g. Let `C = cone(g)` (relevant rules), `I = R \ C`
(irrelevant pool). Build rule sets

```
R_k = C ∪ S_k ,  S_k ⊆ I , |S_k| = k
```

- **#GARs** = `|R_k| = |C| + k`.
- **irrelevant proportion** = `k / (|C| + k)`.

Sweep `k ∈ {0, ¼|I|, ½|I|, ¾|I|, |I|}` (and optionally beyond by importing
irrelevant rules from other datasets' libraries). For each `R_k` run:
- unrestricted chase on `R_k`;
- filtered chase on `C` (the r>0 subset of `R_k`).

**Constraint (important, honest):** the relevant core C must stay COMPLETE —
subsampling C breaks goal derivability and invalidates the safety check. So the
irrelevant proportion is varied by ADDING irrelevant rules to the full cone, not
by removing relevant ones. The achievable irr% range is bounded by `|I|/|R|`:
- central goal (e.g. FinReflectKG `discloses`: |C|=401, |I|=54) → irr% ≤ 12%;
- peripheral goal (small cone, large I) → irr% up to ~90%+.

To span a wide irr% range we therefore **select goals across cone sizes**
(peripheral → central). Step 3.1 characterizes the cone-size distribution so we
pick goals giving irr% ≈ {10, 30, 50, 70, 90}.

## 3. Procedure

### 3.1 Characterize relevance distribution (cheap, one pass)
For each candidate goal predicate g, compute `|cone(g)| / |R|`. Report the
distribution; select a goal panel spanning irr% from low (central) to high
(peripheral). Dataset: FinReflectKG (455 rules) primary; BioKG (720) / FB15k (640)
as cross-dataset checks.

### 3.2 Safety + candidate sweep (per selected goal)
For each goal g and each `R_k`:
1. `gar_cone_union`-style symlink dir for `C` and for `R_k` (reuse
   `tools/cli/gar_cone_union.cpp` logic; extend to emit `R_k` mixes).
2. `gar_reason --mode chase --threads 1 --rules R_k` → unrestricted:
   `embeddings_total`, `derivation_candidates_total`, `derived_total`,
   `closure_edges.tsv`.
3. `gar_reason --mode chase --threads 1 --rules C` → filtered: same metrics.
4. **Derivable goals lost**: take a goal panel G (derived facts). Check each
   `g' ∈ G` present in unrestricted closure vs filtered closure. Lost =
   derivable_unrestricted \ derivable_filtered. Expect **0** for g's own cone;
   for a multi-goal panel use `C = cone-union(G)`.
5. **Candidate reduction** = `1 − emb_filtered / emb_unrestricted` (and the
   productive-candidate version).

### 3.3 Figure (a)
x = irrelevant proportion (or #GARs); y = candidate reduction %; one curve per
goal (or per dataset); annotate **goals lost = 0** on every point. Secondary
panel: candidate reduction vs goal-cone size (shows central goals filter little).

## 4. Metrics summary (per config)

| metric | source |
|---|---|
| #GARs, irr% | rule-set construction |
| candidate actions (unrestricted / filtered) | `embeddings_total` |
| productive candidates (unrestricted / filtered) | `derivation_candidates_total` |
| candidate reduction % | `1 − filtered/unrestricted` |
| derivable goals lost | closure membership diff (expect 0) |
| derived facts (unrestricted / filtered) | `derived_total` (dedup, one chase) |

All chases single-thread (`--threads 1`), no candidate cap (completeness).
Derived-fact column here is **dedup** (one chase per config), NOT a per-goal sum —
avoids the口径 confusion from the 50-goal table.

## 5. Expected honest outcome

- **Goals lost = 0** everywhere (safe filtering verified).
- Candidate reduction **scales with irr% and the irrelevant rules' match
  productivity**. Central goals (large cone) → small reduction (FinReflectKG
  discloses: full 48.36M vs cone 45.84M embeddings ≈ 5%); peripheral goals (small
  cone) → large reduction. This is the same phenomenon as the cone-fairness check:
  filtering benefit is modest for central predicates, large for peripheral ones.
- The figure should report the reduction honestly per goal, not a single
  over-claimed average.

## 6. Implementation plan

- `experiments/exp-2-goal/run_exp2.py`:
  - load rules + graph; compute per-predicate cone sizes (call a small C++ helper
    or reuse `gar_cone_union` per predicate);
  - select goal panel; build `R_k` symlink dirs;
  - run unrestricted/filtered chases; parse summaries; check goal membership;
  - emit `results.json` + `fig_a.svg` (reduction vs irr%, goals-lost annotation).
- Reuse: `gar_cone_union` (cone symlinks), `gar_reason --mode chase` (metrics),
  `closure_edges.tsv` (goal membership).
- Runtime estimate: ~100 s/chase × 2 × (#goals × #k). Keep panel ≤ 5 goals × 5 k
  ≈ 50 chases ≈ 80 min (detached, setsid). Use a smaller dataset (BioKG ~27 s) for
  a fast first pass.

## 7. Open choices (confirm before running)

1. Dataset: FinReflectKG primary, or BioKG fast first pass?
2. Goal panel: single fixed goal + added irrelevant rules, OR multi-goal panel
   spanning cone sizes (recommended for wide irr% range)?
3. Candidate metric: `embeddings_total` (all candidates) vs
   `derivation_candidates_total` (productive) — report both?
4. irr% sweep range bounded by natural irrelevant pool, or import extra
   irrelevant rules to reach high irr% on central goals?