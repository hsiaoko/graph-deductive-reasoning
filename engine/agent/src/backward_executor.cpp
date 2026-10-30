// BackwardExecutor: goal-directed backward chaining (see executor.h for the
// design rationale). Per target fact: pinned enumeration of the rules whose
// consequent names the target's predicate (the candidate space collapses to
// motifs that can derive the goal), antecedent evaluation with the chase's
// exact semantics, open-edge subgoals when pattern edges are missing (an
// edge absent but derivable becomes a recursive subgoal; at most two opened
// edges per rule, sharing at most one free variable), recursive Derive on
// subgoals. Every antecedent evaluation is charged one unit (paper: an
// application pays for testing its preconditions). Facts only accumulate;
// soundness by construction (each ApplyDerivation is a valid chase step).

#include "gar/engine/agent/executor.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "apply_helpers.h"
#include "gar/engine/agent/value_model.h"
#include "gar/graph/pattern_matcher.h"
#include "gar/graph/predicate_evaluator.h"

namespace gar {
namespace engine {
namespace agent {

namespace {

// A fact to derive; an absent side is open (any derived value satisfies).
struct Target {
  std::uint32_t slot;
  std::optional<std::uint32_t> src, dst;
};

std::uint64_t TargetKey(const Target& t) {
  return (static_cast<std::uint64_t>(t.slot) << 40) ^
         graph::PackPair(t.src.value_or(UINT32_MAX),
                         t.dst.value_or(UINT32_MAX));
}

// Trace control: GAR_DEBUG_BACKWARD=1 prints target entry/exit (debugging
// only; off by default).
bool DebugBackward() {
  static const bool on = std::getenv("GAR_DEBUG_BACKWARD") != nullptr;
  return on;
}

// --backward-order: apply the configured ordering to an index sequence that
// is already score-sorted. score = keep as is (default); random = seeded
// full shuffle; hybrid = seeded shuffle of the top-8 window only. The RNG
// is re-seeded per call site from (seed, salt) so the shuffle is
// deterministic per run regardless of ordering code paths taken.
template <typename Index>
void ApplyBackwardOrder(const ExecConfig& cfg, std::uint64_t salt,
                        std::vector<Index>* ord) {
  if (cfg.backward_order == "score" || ord->size() < 2) return;
  std::mt19937 rng(cfg.seed ^ (salt * 0x9E3779B97F4A7C15ull));
  if (cfg.backward_order == "random") {
    std::shuffle(ord->begin(), ord->end(), rng);
  } else {  // hybrid: shuffle the top-8 window, keep the tail score-sorted
    const std::size_t w = std::min<std::size_t>(8, ord->size());
    std::shuffle(ord->begin(), ord->begin() + w, rng);
  }
}

}  // namespace

AgentResult BackwardExecutor::Run(const Goal& goal, AgentState* state,
                                  WitnessLog* witness,
                                  FailureRegistry* failures,
                                  const AgentContext& ctx,
                                  const ExecConfig& cfg,
                                  const ExecKit& kit) {
  AgentResult res;
  const auto& rules = state->rules();
  const graph::TripleGraph& g = state->graph();

  // Head index: predicate name -> rules deriving it (same skip conditions
  // as the forward generator: no/short pattern, unlabeled edges, ml,
  // unresolved edge labels, non-edge or wildcard consequent).
  std::unordered_map<std::string, std::vector<std::uint32_t>> head_index;
  for (std::uint32_t i = 0; i < rules.size(); ++i) {
    const GARule& r = rules[i];
    const auto n = static_cast<std::uint32_t>(r.pattern.num_vertices());
    if (n < 2 || r.pattern.num_edges() < 1 || !r.pattern.has_edge_labels()) {
      continue;
    }
    const RuleHead head = ResolveRuleHead(r);
    if (!head.valid) continue;
    bool skip = false;
    for (const auto& pred : r.antecedent) {
      if (pred.kind == gar::PredKind::kMl ||
          (pred.kind == gar::PredKind::kEdge &&
           pred.label == gar::kWildcardLabel)) {
        skip = true;
        break;
      }
    }
    if (skip) continue;
    head_index[analysis::PredicateIdOf(*r.consequent, r)].push_back(i);
  }

  const ScoringContext sctx{&ctx.distances, ctx.table, witness, state};
  std::uint64_t spent = 0;
  const auto wall_t0 = std::chrono::steady_clock::now();
  auto wall_exceeded = [&]() {
    return cfg.wall_limit_sec > 0.0 &&
           std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         wall_t0)
                   .count() >= cfg.wall_limit_sec;
  };
  const std::size_t kMaxCollected = cfg.open_collect_cap;
  std::unordered_set<std::uint64_t> active;  // recursion cycle guard
  // Target-level failure memo: a target that yields nothing at the current
  // fact-set version is not retried from other parents (the repeated
  // identical subgoal attempts otherwise drain the budget on recursive
  // families). Re-admitted on Gamma growth, like dynamic motif failures.
  std::unordered_map<std::uint64_t, std::uint64_t> failed_targets;

  // Applies one motif: evaluates X (charged), derives on success. Returns
  // the derived fact on success, nullopt on failure. Shared by the full
  // pinned path and the open-edge re-match path.
  auto apply_motif = [&](std::uint32_t ri, const std::uint32_t* emb,
                         std::uint32_t n) -> std::optional<graph::DerivedEdge> {
    const GARule& r = rules[ri];
    const std::uint64_t mkey = MotifHash(ri, emb, n);
    if (failures->IsRetired(mkey, state->version())) return std::nullopt;
    ++spent;  // one application: evaluating the antecedent under h
    AgentStep step;
    step.motif = Motif{ri, std::vector<std::uint32_t>(emb, emb + n)};
    step.cost = kit.cost_model->RuleLowerBound(r);  // unit: 1
    std::vector<Fact> premises;
    GroundPremises(r, step.motif, g, *ctx.table, &premises);
    const bool ok =
        graph::EvalAntecedent(r.antecedent, emb, state->attrs(), g);
    std::fprintf(stderr, "[backward] apply %s ok=%d spent=%llu\n",
                 r.name.substr(0, 80).c_str(), static_cast<int>(ok),
                 static_cast<unsigned long long>(spent));
    if (ok) {
      const RuleHead head = ResolveRuleHead(r);
      const graph::DerivedEdge d{
          emb[head.pu], emb[head.pv], head.slot, ri,
          static_cast<std::uint32_t>(res.chain.size() + 1)};
      state->ApplyDerivation(d);
      step.success = true;
      step.derived = d;
      for (const auto& f : premises) witness->WitnessFact(f, ctx.encoder);
      witness->WitnessFact(MakeEdgeFact(d.slot, d.src, d.dst), ctx.encoder);
      res.chain.push_back(std::move(step));
      return d;
    }
    const FailureInfo fi = ClassifyFailure(r, emb, state->attrs(), g, ctx);
    failures->RecordFailure(mkey, fi.dynamic, state->version());
    const std::size_t n_pattern = r.pattern.num_edges();
    const std::size_t upto = fi.fail_idx < r.antecedent.size()
                                 ? n_pattern + fi.fail_idx + 1
                                 : premises.size();
    for (std::size_t i = 0; i < upto && i < premises.size(); ++i) {
      witness->WitnessFact(premises[i], ctx.encoder);
    }
    res.chain.push_back(std::move(step));
    return std::nullopt;
  };

  // The matcher and one rule's spec/pins builder.
  const graph::PatternMatcher matcher(g);
  auto build_spec = [](const GARule& r) {
    graph::PatternSpec spec;
    spec.num_vertices = static_cast<std::uint32_t>(r.pattern.num_vertices());
    for (std::uint32_t s = 0; s < spec.num_vertices; ++s) {
      for (gar::EdgeIndex e = r.pattern.offsets[s];
           e < r.pattern.offsets[s + 1]; ++e) {
        spec.edges.push_back(
            {s, r.pattern.indices[e], r.pattern.edge_labels[e]});
      }
    }
    return spec;
  };
  auto label_ok = [&](const GARule& r, const std::uint32_t* emb) {
    if (!g.has_vertex_labels()) return true;
    for (std::uint32_t u = 0; u < r.pattern.num_vertices(); ++u) {
      if (u < r.node_labels.size() &&
          r.node_labels[u] != gar::kWildcardLabel &&
          g.VertexLabel(emb[u]) != r.node_labels[u]) {
        return false;
      }
    }
    return true;
  };

  // CollectCandidateValues: the free-endpoint values that rules deriving
  // `slot` could produce RIGHT NOW with `pinned_val` on one side — the
  // 1-step match horizon (no application, no charge). Used by the
  // semi-join pre-check in the derive-first path: compatibility is tested
  // on candidate values BEFORE paying for derivations.
  auto collect_candidate_values = [&](gar::LabelID slot, bool pin_src,
                                      std::uint32_t pinned_val)
      -> std::vector<std::uint32_t> {
    std::vector<std::uint32_t> vals;
    std::unordered_set<std::uint32_t> seen;
    const std::string& pred_name = g.RelationName(slot);
    const auto it = head_index.find(pred_name);
    if (it == head_index.end()) return vals;
    for (const std::uint32_t ri : it->second) {
      const GARule& r = rules[ri];
      const RuleHead head = ResolveRuleHead(r);
      std::unordered_map<std::uint32_t, std::uint32_t> pins;
      if (pin_src) {
        pins[head.pu] = pinned_val;
      } else {
        pins[head.pv] = pinned_val;
      }
      const auto spec = build_spec(r);
      std::vector<std::uint32_t> embs;
      bool capped = false;
      const std::uint64_t n_emb = matcher.Enumerate(
          spec, cfg.match_cap_per_rule, &embs, &capped, pins);
      const auto n = static_cast<std::uint32_t>(r.pattern.num_vertices());
      const std::uint32_t free_v = pin_src ? head.pv : head.pu;
      for (std::uint64_t k = 0; k < n_emb; ++k) {
        const std::uint32_t* emb = embs.data() + k * n;
        if (!label_ok(r, emb)) continue;
        if (!graph::EvalAntecedent(r.antecedent, emb, state->attrs(), g)) {
          continue;
        }
        if (seen.insert(emb[free_v]).second) vals.push_back(emb[free_v]);
      }
    }
    return vals;
  };

  // PresentValues: free-endpoint values of `slot` edges ALREADY in the
  // current fact set under the pin (base + derived; a graph read, not an
  // application — no charge). Candidate sets for the two-sided coordination
  // are present-first, then derivable: present ends make pairs for free.
  auto present_values = [&](gar::LabelID slot, bool pin_src,
                            std::uint32_t pinned) {
    const std::uint32_t rel =
        pin_src ? slot : slot + static_cast<std::uint32_t>(g.num_relations());
    const auto [ptr, cnt] = g.OutNeighbors(pinned, rel);
    return std::vector<std::uint32_t>(ptr, ptr + cnt);
  };
  auto merge_candidates = [](std::vector<std::uint32_t> present,
                             const std::vector<std::uint32_t>& derivable) {
    std::unordered_set<std::uint32_t> seen(present.begin(), present.end());
    for (const std::uint32_t v : derivable) {
      if (seen.insert(v).second) present.push_back(v);
    }
    return present;
  };

  // OneLevelDerives: would any rule deriving `slot` fire RIGHT NOW with both
  // endpoints pinned (1-step match horizon, no charge)? The two-sided
  // coordination prices a missing semi-join edge with this before paying for
  // its derivation, so wrong candidate pairs cost nothing but the probe.
  auto one_level_derives = [&](gar::LabelID slot, std::uint32_t src,
                               std::uint32_t dst) {
    const std::string& pred_name = g.RelationName(slot);
    const auto it = head_index.find(pred_name);
    if (it == head_index.end()) return false;
    for (const std::uint32_t ri : it->second) {
      const GARule& r = rules[ri];
      const RuleHead head = ResolveRuleHead(r);
      std::unordered_map<std::uint32_t, std::uint32_t> pins;
      pins[head.pu] = src;
      pins[head.pv] = dst;
      const auto spec = build_spec(r);
      std::vector<std::uint32_t> embs;
      bool capped = false;
      const std::uint64_t n_emb = matcher.Enumerate(
          spec, cfg.match_cap_per_rule, &embs, &capped, pins);
      const auto n = static_cast<std::uint32_t>(r.pattern.num_vertices());
      for (std::uint64_t k = 0; k < n_emb; ++k) {
        const std::uint32_t* emb = embs.data() + k * n;
        if (!label_ok(r, emb)) continue;
        if (graph::EvalAntecedent(r.antecedent, emb, state->attrs(), g)) {
          return true;
        }
      }
    }
    return false;
  };

  // TryTarget: make facts matching `t` hold (derive them if derivable).
  // Returns the (capped) set of (src,dst) pairs now present.
  //   - both sides pinned: present -> {it}; else derive.
  //   - one side open: derive up to cfg.open_collect_cap distinct facts.
  // local_cap bounds the applications charged INSIDE this call (the budget
  // halves per recursion level): a wandering recursive family (sibling
  // chains) can't drain the shared budget before the right opening is
  // tried. Resource allocation only — soundness unaffected.
  std::function<std::vector<std::pair<std::uint32_t, std::uint32_t>>(
      const Target&, std::uint32_t, std::uint64_t)>
      try_target = [&](const Target& t, std::uint32_t depth,
                       std::uint64_t local_cap)
      -> std::vector<std::pair<std::uint32_t, std::uint32_t>> {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> got;
    if (t.src && t.dst && state->ContainsEdge(t.slot, *t.src, *t.dst)) {
      return {{*t.src, *t.dst}};  // already present: premise holds for free
    }
    const std::uint64_t local_limit = spent + local_cap;
    auto out_of_budget = [&]() {
      return spent >= cfg.budget || spent >= local_limit || wall_exceeded();
    };
    if (out_of_budget() || depth > cfg.max_subgoal_depth) return got;
    if (!active.insert(TargetKey(t)).second) return got;  // cycle
    {
      const auto ft = failed_targets.find(TargetKey(t));
      if (ft != failed_targets.end() && ft->second == state->version()) {
        return got;  // proven unproductive at this fact set
      }
    }

    const std::string& pred_name = g.RelationName(t.slot);
    if (DebugBackward()) {
      std::fprintf(stderr, "[bwd] try %s(%s,%s) depth=%u spent=%llu\n",
                   pred_name.c_str(),
                   t.src ? g.EntityName(*t.src).c_str() : "_",
                   t.dst ? g.EntityName(*t.dst).c_str() : "_", depth,
                   static_cast<unsigned long long>(spent));
    }
    const auto it = head_index.find(pred_name);
    if (it == head_index.end()) {
      active.erase(TargetKey(t));
      return got;
    }

    // ---- pass 1: full pinned enumeration ----
    std::vector<Motif> mus;
    for (const std::uint32_t ri : it->second) {
      const GARule& r = rules[ri];
      const RuleHead head = ResolveRuleHead(r);
      std::unordered_map<std::uint32_t, std::uint32_t> pins;
      if (t.src) pins[head.pu] = *t.src;
      if (t.dst) pins[head.pv] = *t.dst;
      const auto spec = build_spec(r);
      std::vector<std::uint32_t> embs;
      bool capped = false;
      const std::uint64_t n_emb = matcher.Enumerate(
          spec, cfg.match_cap_per_rule, &embs, &capped, pins);
      const auto n = static_cast<std::uint32_t>(r.pattern.num_vertices());
      for (std::uint64_t k = 0; k < n_emb; ++k) {
        const std::uint32_t* emb = embs.data() + k * n;
        if (label_ok(r, emb)) {
          mus.push_back(Motif{ri, std::vector<std::uint32_t>(emb, emb + n)});
        }
      }
    }
    // Rank by the scorer, deterministic order: total desc, then rule/emb.
    std::vector<ScoredMotif> scored;
    kit.scorer->ScoreBatch(rules, mus, sctx, &scored);
    std::vector<std::size_t> ord(scored.size());
    for (std::size_t i = 0; i < ord.size(); ++i) ord[i] = i;
    std::sort(ord.begin(), ord.end(), [&](std::size_t x, std::size_t y) {
      if (scored[x].total != scored[y].total) {
        return scored[x].total > scored[y].total;
      }
      if (scored[x].motif.rule != scored[y].motif.rule) {
        return scored[x].motif.rule < scored[y].motif.rule;
      }
      return scored[x].motif.embedding < scored[y].motif.embedding;
    });
    ApplyBackwardOrder(cfg, TargetKey(t) ^ (depth + 1), &ord);
    if (DebugBackward()) {
      std::fprintf(stderr, "[bwd]   pass1 mus=%zu\n", mus.size());
    }
    bool first_cand = true;
    for (const std::size_t oi : ord) {
      if (out_of_budget()) break;
      if (got.size() >= kMaxCollected) break;
      // Degenerate single-path arm (cfg.no_backtrack, --direction backward
      // --executor greedy): try only the top-ranked candidate per target,
      // no open-edge fallback below.
      if (cfg.no_backtrack && !first_cand) break;
      first_cand = false;
      const Motif& mu = scored[oi].motif;
      if (cfg.backward_full_enum) {
        // Skip conclusions already in the graph: re-deriving them burns
        // budget and (worse) keeps `got` non-empty forever, which would
        // starve the open-edge fallback across outer-loop rounds.
        const GARule& fr = rules[mu.rule];
        const RuleHead fh = ResolveRuleHead(fr);
        if (fh.valid && state->ContainsEdge(fh.slot, mu.embedding[fh.pu],
                                            mu.embedding[fh.pv])) {
          continue;
        }
      }
      if (auto d = apply_motif(mu.rule, mu.embedding.data(),
                               static_cast<std::uint32_t>(
                                   mu.embedding.size()))) {
        if ((!t.src || d->src == *t.src) && (!t.dst || d->dst == *t.dst)) {
          got.emplace_back(d->src, d->dst);
        }
        if (t.src && t.dst) break;  // ground target: done
      } else {
        // Dynamic edge-predicate failure: recurse on the blocking premise.
        const Motif& m2 = scored[oi].motif;
        const GARule& r = rules[m2.rule];
        const FailureInfo fi = ClassifyFailure(r, m2.embedding.data(),
                                               state->attrs(), g, ctx);
        if (fi.dynamic && fi.fail_idx < r.antecedent.size() &&
            depth < cfg.max_subgoal_depth && !out_of_budget()) {
          const gar::Predicate& pred = r.antecedent[fi.fail_idx];
          Target sub;
          if (pred.label >= g.num_relations()) {
            sub = Target{pred.label - g.num_relations(),
                         m2.embedding[pred.dst], m2.embedding[pred.src]};
          } else {
            sub = Target{pred.label, m2.embedding[pred.src],
                         m2.embedding[pred.dst]};
          }
          if (!try_target(sub, depth + 1, local_cap / 2).empty()) {
            if (auto d = apply_motif(m2.rule, m2.embedding.data(),
                                     static_cast<std::uint32_t>(
                                         m2.embedding.size()))) {
              if ((!t.src || d->src == *t.src) && (!t.dst || d->dst == *t.dst)) {
                got.emplace_back(d->src, d->dst);
              }
              if (t.src && t.dst) break;
            }
          }
        }
      }
    }
    if (!got.empty() || depth >= cfg.max_subgoal_depth || out_of_budget() ||
        cfg.no_backtrack) {
      active.erase(TargetKey(t));
      return got;
    }

    // ---- pass 2: open-edge subgoals (pattern edges missing from the
    // current graph but derivable). At most two opened edges per rule;
    // two opened edges must share exactly one variable (the free join
    // variable gets candidate values from deriving one side first).
    // Collect (rule, open-set) attempts for ALL head rules first, then
    // execute in guidance order: the opened edge closest to the goal first
    // (d of its base predicate toward the goal — the intermediate cheapest
    // to derive), then the rule's rel_fam (via the scorer on a dummy
    // embedding — rel_fam is embedding-independent), then confidence, then
    // index. Without this the fallback burns the budget on hopeless
    // openings of early rules (fb15k: prequel-openings before the
    // person-chain rule the chase actually used).
    struct OpenAttempt {
      std::uint32_t rule;
      gar::EdgeIndex oe1, oe2;
      std::uint32_t open_d;   // min d(base slot of opened edges)
      bool derive_first;      // 2-open with one goal-pinned endpoint each
      std::uint32_t tier;     // derive-first evidence: 1 both sides have
                              // 1-level candidates, 2 one side, 3 neither
      double rel_fam;
      double conf;
      // side candidate-set sizes from the tier probe (0 for non-df) — the
      // value-model feature vector's vals1/vals2
      std::uint32_t vals1_n = 0, vals2_n = 0;
    };
    // Cache for the derive-first ordering probes (valid within this call's
    // collection phase; ordering only, never a soundness input).
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> tier_cache;
    auto tier_vals = [&](gar::LabelID base, bool pin_src,
                         std::uint32_t pinned)
        -> const std::vector<std::uint32_t>& {
      const std::uint64_t key = (static_cast<std::uint64_t>(base) << 33) ^
                                (static_cast<std::uint64_t>(pin_src ? 1 : 0)
                                 << 32) ^
                                pinned;
      const auto itc = tier_cache.find(key);
      if (itc != tier_cache.end()) return itc->second;
      // Present endpoints first, then 1-level-derivable ones: the ordering
      // probes judge an opening by everything that can ground it.
      auto merged = present_values(base, pin_src, pinned);
      {
        const auto& deriv = collect_candidate_values(base, pin_src, pinned);
        std::unordered_set<std::uint32_t> seen(merged.begin(), merged.end());
        for (const std::uint32_t v : deriv) {
          if (seen.insert(v).second) merged.push_back(v);
        }
      }
      return tier_cache.emplace(key, std::move(merged)).first->second;
    };
    std::vector<OpenAttempt> attempts;
    for (const std::uint32_t ri : it->second) {
      const GARule& r = rules[ri];
      const RuleHead head = ResolveRuleHead(r);
      const auto n = static_cast<std::uint32_t>(r.pattern.num_vertices());
      const auto m_edges = r.pattern.num_edges();

      // Flatten pattern edges to (src, dst, slot).
      struct PE {
        std::uint32_t s, d;
        gar::LabelID slot;
      };
      std::vector<PE> pedges;
      for (std::uint32_t s = 0; s < n; ++s) {
        for (gar::EdgeIndex e = r.pattern.offsets[s];
             e < r.pattern.offsets[s + 1]; ++e) {
          pedges.push_back({s, r.pattern.indices[e], r.pattern.edge_labels[e]});
        }
      }
      // Candidate open sets: single edge, or ANY edge pair (the shared
      // free variable, when one exists, gets the coordinated derivation;
      // pairs sharing no variable are handled when the remaining
      // sub-pattern binds everything — the fb15k film goal opens two
      // NON-adjacent edges: film(x0,x1) and person^-1(x2,x3), with x1/x2
      // bound by the remaining person(x1,x2) edge).
      std::vector<std::pair<gar::EdgeIndex, gar::EdgeIndex>> open_sets;
      for (gar::EdgeIndex e = 0; e < m_edges; ++e) {
        open_sets.emplace_back(e, gar::EdgeIndex(UINT32_MAX));
      }
      for (gar::EdgeIndex e1 = 0; e1 < m_edges; ++e1) {
        for (gar::EdgeIndex e2 = e1 + 1; e2 < m_edges; ++e2) {
          open_sets.emplace_back(e1, e2);
        }
      }
      // Rule-level guidance score (rel_fam is embedding-independent).
      ScoredMotif probe;
      kit.scorer->Score(r, Motif{ri, std::vector<std::uint32_t>(n, 0)},
                          sctx, &probe);
      auto open_d = [&](gar::EdgeIndex oe) {
        const gar::LabelID l = pedges[oe].slot;
        const std::uint32_t b =
            l >= g.num_relations() ? l - g.num_relations() : l;
        const std::uint32_t pid =
            ctx.table->Find(g.RelationName(b));
        return (pid != analysis::kInvalidPredicate &&
                pid < ctx.distances.size())
                   ? ctx.distances[pid]
                   : analysis::kUnreachable;
      };
      // Side candidate-set size under the goal pin (tier-probe cache) — a
      // value-model feature; 0 when the edge is not one-pinned.
      auto va_size_hint = [&](const PE& pe) -> std::size_t {
        const bool ps = (t.src && pe.s == head.pu) ||
                        (t.dst && pe.s == head.pv);
        const bool pd = (t.src && pe.d == head.pu) ||
                        (t.dst && pe.d == head.pv);
        if (ps == pd) return 0;
        const bool inv = pe.slot >= g.num_relations();
        const gar::LabelID base =
            inv ? pe.slot - g.num_relations() : pe.slot;
        const std::uint32_t pinned =
            ps ? (pe.s == head.pu ? *t.src : *t.dst)
               : (pe.d == head.pu ? *t.src : *t.dst);
        return tier_vals(base, inv ? !ps : ps, pinned).size();
      };
      for (const auto& [oe1, oe2] : open_sets) {
        std::uint32_t od = open_d(oe1);
        bool df = false;
        std::uint32_t tier = 3;
        if (oe2 != gar::EdgeIndex(UINT32_MAX)) {
          od = std::max(od, open_d(oe2));
          // derive-first eligible: each opened edge has exactly one
          // goal-pinned endpoint, and the free endpoints differ.
          const std::uint32_t pu = head.pu, pv = head.pv;
          auto one_pinned = [&](const PE& pe) {
            const bool ps = t.src && (pe.s == pu || pe.s == pv);
            const bool pd = t.dst && (pe.d == pu || pe.d == pv);
            return ps != pd;
          };
          if (one_pinned(pedges[oe1]) && one_pinned(pedges[oe2])) {
            df = true;
            // Ordering probe (no charge, cached per side): 1-level
            // candidate values for each opened edge's free endpoint in
            // base orientation (inverse edges flip the side).
            auto side_vals_of = [&](const PE& pe)
                -> const std::vector<std::uint32_t>& {
              const bool ps =
                  (t.src && pe.s == pu) || (t.dst && pe.s == pv);
              const bool inv = pe.slot >= g.num_relations();
              const gar::LabelID base =
                  inv ? pe.slot - g.num_relations() : pe.slot;
              const std::uint32_t pinned =
                  ps ? (pe.s == pu ? *t.src : *t.dst)
                     : (pe.d == pu ? *t.src : *t.dst);
              return tier_vals(base, inv ? !ps : ps, pinned);
            };
            const auto& va = side_vals_of(pedges[oe1]);
            const auto& vb = side_vals_of(pedges[oe2]);
            tier = (!va.empty() && !vb.empty()) ? 1
                   : (!va.empty() || !vb.empty()) ? 2
                                                  : 3;
            if (tier == 1) {
              // Promote to tier 0 when a candidate pair composes through
              // the remaining sub-pattern — edges present now OR 1-level
              // derivable (the chased shapes derive the middle edge too).
              // Composition evidence beats blind two-sided collects.
              const PE& A2 = pedges[oe1];
              const PE& B2 = pedges[oe2];
              const bool a_ps =
                  (t.src && A2.s == pu) || (t.dst && A2.s == pv);
              const bool b_ps =
                  (t.src && B2.s == pu) || (t.dst && B2.s == pv);
              const std::uint32_t a_free_v = a_ps ? A2.d : A2.s;
              const std::uint32_t b_free_v = b_ps ? B2.d : B2.s;
              auto pinned_or = [&](std::uint32_t v, std::uint32_t v1,
                                   std::uint32_t v2, bool& is_free) {
                if (v == pu && t.src) {
                  is_free = false;
                  return *t.src;
                }
                if (v == pv && t.dst) {
                  is_free = false;
                  return *t.dst;
                }
                is_free = true;
                return v == a_free_v ? v1 : v2;
              };
              bool sj = true;
              for (std::size_t i = 0; i < pedges.size(); ++i) {
                if (i == oe1 || i == oe2) continue;
                for (const std::uint32_t v : {pedges[i].s, pedges[i].d}) {
                  bool dummy;
                  pinned_or(v, 0, 0, dummy);
                  if (dummy && v != a_free_v && v != b_free_v) sj = false;
                }
              }
              bool composable = false;
              std::size_t n1 = 0;
              for (const std::uint32_t v1 : va) {
                if (composable || sj == false || ++n1 > 64) break;
                std::size_t n2 = 0;
                for (const std::uint32_t v2 : vb) {
                  if (++n2 > 64) break;
                  bool edges_ok = true;
                  for (std::size_t i = 0; i < pedges.size() && edges_ok;
                       ++i) {
                    if (i == oe1 || i == oe2) continue;
                    const PE& pe = pedges[i];
                    bool f1, f2;
                    const std::uint32_t s = pinned_or(pe.s, v1, v2, f1);
                    const std::uint32_t d = pinned_or(pe.d, v1, v2, f2);
                    const bool inv = pe.slot >= g.num_relations();
                    const gar::LabelID bs =
                        inv ? pe.slot - g.num_relations() : pe.slot;
                    const std::uint32_t cs = inv ? d : s;
                    const std::uint32_t cd = inv ? s : d;
                    if (state->ContainsEdge(bs, cs, cd)) continue;
                    if (head_index.find(g.RelationName(bs)) ==
                        head_index.end()) {
                      edges_ok = false;
                      break;
                    }
                    // 1-level derivable? (cached collect: src-pinned free
                    // dst values)
                    const auto& reach = tier_vals(bs, true, cs);
                    if (std::find(reach.begin(), reach.end(), cd) ==
                        reach.end()) {
                      edges_ok = false;
                    }
                  }
                  if (edges_ok) {
                    composable = true;
                    break;
                  }
                }
              }
              if (composable) tier = 0;
            }
          }
        }
        attempts.push_back(OpenAttempt{ri, oe1, oe2, od, df, tier,
                                       probe.rel_fam,
                                       r.quality.confidence,
                                       df ? static_cast<std::uint32_t>(
                                                va_size_hint(pedges[oe1]))
                                          : 0u,
                                       df ? static_cast<std::uint32_t>(
                                                va_size_hint(pedges[oe2]))
                                          : 0u});
      }
    }
    std::sort(attempts.begin(), attempts.end(),
              [](const OpenAttempt& a, const OpenAttempt& b) {
                // derive-first-eligible openings first (coordinated
                // intermediates are the rare winning shape); among them the
                // evidence tier (both sides 1-level-derivable beats blind
                // deep chains), then the opened edges' distance to the goal,
                // then rule quality.
                if (a.derive_first != b.derive_first) return a.derive_first;
                if (a.tier != b.tier) return a.tier < b.tier;
                if (a.open_d != b.open_d) return a.open_d < b.open_d;
                if (a.rel_fam != b.rel_fam) return a.rel_fam > b.rel_fam;
                if (a.conf != b.conf) return a.conf > b.conf;
                if (a.rule != b.rule) return a.rule < b.rule;
                if (a.oe1 != b.oe1) return a.oe1 < b.oe1;
                return a.oe2 < b.oe2;
              });
    {
      std::vector<std::uint32_t> aord(attempts.size());
      for (std::uint32_t ai = 0; ai < aord.size(); ++ai) aord[ai] = ai;
      ApplyBackwardOrder(cfg, TargetKey(t) ^ (depth + 2), &aord);
      if (cfg.backward_order != "score") {
        std::vector<OpenAttempt> tmp;
        tmp.reserve(attempts.size());
        for (const std::uint32_t ai : aord) tmp.push_back(attempts[ai]);
        attempts = std::move(tmp);
      }
    }
    // Optional HDC value-model re-ordering of the OR candidates
    // (--backward-priority hdc-value): predicted failure probability asc,
    // then the heuristic tuple for determinism. ONLY the attempt order
    // changes; pinned matching, semi-join intersection, accounting and
    // derivation validation are untouched. Default (no model) reproduces
    // the heuristic order bit-for-bit.
    if (kit.value_model != nullptr && !attempts.empty()) {
      const double remaining =
          cfg.budget > spent ? static_cast<double>(cfg.budget - spent) : 0.0;
      std::vector<std::pair<double, std::size_t>> keyed(attempts.size());
      for (std::size_t ai = 0; ai < attempts.size(); ++ai) {
        const OpenAttempt& a = attempts[ai];
        const std::array<double, 11> raw = {
            static_cast<double>(a.open_d),
            a.derive_first ? 1.0 : 0.0,
            static_cast<double>(a.tier),
            a.rel_fam,
            a.conf,
            static_cast<double>(it->second.size()),
            static_cast<double>(a.vals1_n),
            static_cast<double>(a.vals2_n),
            static_cast<double>(depth),
            static_cast<double>(spent),
            remaining};
        keyed[ai] = {kit.value_model->PredictRemaining(raw), ai};
      }
      std::sort(keyed.begin(), keyed.end(),
                [](const auto& x, const auto& y) {
                  return x.first != y.first ? x.first < y.first
                                            : x.second < y.second;
                });
      std::vector<OpenAttempt> reordered;
      reordered.reserve(attempts.size());
      for (const auto& [p, ai] : keyed) reordered.push_back(attempts[ai]);
      attempts = std::move(reordered);
    }

    if (DebugBackward()) {
      std::size_t pos = 0;
      for (const auto& att : attempts) {
        std::fprintf(stderr,
                     "[bwd]   attempt#%zu rule=%u oe=(%u,%u) open_d=%u df=%d "
                     "tier=%u rel_fam=%.3f conf=%.3f\n",
                     pos++, att.rule, att.oe1,
                     att.oe2 == gar::EdgeIndex(UINT32_MAX) ? 999u : att.oe2,
                     att.open_d, static_cast<int>(att.derive_first), att.tier,
                     att.rel_fam, att.conf);
        if (pos > 40) break;
      }
    }
    // Outcome-per-attempt logging (GAR_DEBUG_BACKWARD): each executed
    // attempt's result is printed at the next iteration's start (and the
    // last one after the loop) — `continue` paths inside an attempt are
    // still charged to it, which is the intended semantics.
    const OpenAttempt* prev_att = nullptr;
    std::uint64_t prev_spent = 0;
    std::size_t prev_got = 0;
    auto log_att_result = [&]() {
      if (prev_att == nullptr) return;
      std::fprintf(stderr,
                   "[bwd]   att-result rule=%u oe=(%u,%u) df=%d tier=%u "
                   "open_d=%u relfam=%.3f conf=%.3f nrules=%zu vals1=%u "
                   "vals2=%u spent=%llu remain=%llu succ=%d dspent=%llu "
                   "depth=%u\n",
                   prev_att->rule, prev_att->oe1,
                   prev_att->oe2 == gar::EdgeIndex(UINT32_MAX)
                       ? 999u
                       : prev_att->oe2,
                   static_cast<int>(prev_att->derive_first), prev_att->tier,
                   prev_att->open_d, prev_att->rel_fam, prev_att->conf,
                   it->second.size(), prev_att->vals1_n, prev_att->vals2_n,
                   static_cast<unsigned long long>(prev_spent),
                   static_cast<unsigned long long>(
                       cfg.budget > prev_spent ? cfg.budget - prev_spent : 0),
                   static_cast<int>(got.size() > prev_got),
                   static_cast<unsigned long long>(spent - prev_spent),
                   depth);
    };
    for (const auto& att : attempts) {
      if (DebugBackward()) log_att_result();
      prev_att = nullptr;
      if (!got.empty() || out_of_budget()) break;
      prev_att = &att;
      prev_spent = spent;
      prev_got = got.size();
      const std::uint32_t ri = att.rule;
      const gar::EdgeIndex oe1 = att.oe1, oe2 = att.oe2;
      const GARule& r = rules[ri];
      const RuleHead head = ResolveRuleHead(r);
      const auto n = static_cast<std::uint32_t>(r.pattern.num_vertices());

      // Flatten pattern edges to (src, dst, slot).
      struct PE {
        std::uint32_t s, d;
        gar::LabelID slot;
      };
      std::vector<PE> pedges;
      for (std::uint32_t s = 0; s < n; ++s) {
        for (gar::EdgeIndex e = r.pattern.offsets[s];
             e < r.pattern.offsets[s + 1]; ++e) {
          pedges.push_back({s, r.pattern.indices[e], r.pattern.edge_labels[e]});
        }
      }

      {  // scope of the former open_sets loop body
        const bool two = oe2 != gar::EdgeIndex(UINT32_MAX);
        // Opened slots must be derivable.
        auto derivable_slot = [&](gar::LabelID l) {
          const std::uint32_t b = l >= g.num_relations()
                                      ? l - g.num_relations()
                                      : l;
          return head_index.find(g.RelationName(b)) != head_index.end();
        };
        if (!derivable_slot(pedges[oe1].slot)) continue;
        if (two && !derivable_slot(pedges[oe2].slot)) continue;

        // Sub-pattern without the opened edges; drop vertices isolated by
        // the opening when pinned (their value is fixed), skip when an
        // unpinned vertex is isolated (undetermined). Remap to dense ids.
        std::vector<char> has_edge(n, 0);
        std::unordered_map<std::uint32_t, std::uint32_t> full_pins;
        if (t.src) full_pins[head.pu] = *t.src;
        if (t.dst) full_pins[head.pv] = *t.dst;

        // Derive-first 2-open (drives by derivability instead of by base
        // matches): when each opened edge has exactly one goal-pinned
        // endpoint and their free endpoints are DIFFERENT variables,
        // collect derivations of both edges with the free side open
        // (bounded by kMaxCollected each), then re-enumerate the full
        // pattern — the remaining edges validate the combination for free.
        // This is the fb15k film/award task shape: two derived
        // intermediates sharing no variable, joined by a base edge
        // (person(x1,x2)) that must not be enumerated over all base pairs.
        if (two) {
          const PE& A = pedges[oe1];
          const PE& B = pedges[oe2];
          auto one_pinned = [&](const PE& pe, bool& src_pinned) -> bool {
            const bool ps = full_pins.count(pe.s) != 0;
            const bool pd = full_pins.count(pe.d) != 0;
            if (ps == pd) return false;  // both or neither pinned: not this case
            src_pinned = ps;
            return true;
          };
          bool a_src_pinned = false, b_src_pinned = false;
          const bool disjoint_free =
              one_pinned(A, a_src_pinned) && one_pinned(B, b_src_pinned) &&
              (a_src_pinned ? A.d : A.s) != (b_src_pinned ? B.d : B.s);
          if (disjoint_free) {
            auto open_target = [&](const PE& pe, bool src_pinned) {
              Target tt;
              tt.slot = pe.slot >= g.num_relations()
                            ? pe.slot - g.num_relations()
                            : pe.slot;
              const std::uint32_t pinned_val =
                  src_pinned ? full_pins.at(pe.s) : full_pins.at(pe.d);
              // mind the inverse-slot flip when pinning
              if (pe.slot >= g.num_relations()) {
                if (src_pinned) {
                  tt.dst = pinned_val;  // flipped: pattern src is base dst
                } else {
                  tt.src = pinned_val;
                }
              } else {
                if (src_pinned) {
                  tt.src = pinned_val;
                } else {
                  tt.dst = pinned_val;
                }
              }
              return tt;
            };
            // Two-sided derivation coordination: candidate values for the
            // two free endpoints WITHOUT deriving (match horizon, no
            // charge, base-orientation pins — inverse edges flip the side),
            // then intersect over the remaining pattern edges. Only a
            // composing pair pays for derivations; a missing remaining edge
            // whose predicate is itself derivable is derived for the pair
            // (the fb15k film/award shape: the middle person/nominated_for
            // edge is a chase-derived fact, not a base edge).
            const PE& Ae = pedges[oe1];
            const PE& Be = pedges[oe2];
            const std::uint32_t a_free = a_src_pinned ? Ae.d : Ae.s;
            const std::uint32_t b_free = b_src_pinned ? Be.d : Be.s;
            auto base_pin_src = [&](const PE& pe, bool pat_src_pinned) {
              return (pe.slot >= g.num_relations()) ? !pat_src_pinned
                                                    : pat_src_pinned;
            };
            const Target t1o = open_target(A, a_src_pinned);
            const Target t2o = open_target(B, b_src_pinned);
            const std::vector<std::uint32_t> vals1 = merge_candidates(
                present_values(t1o.slot, base_pin_src(Ae, a_src_pinned),
                               full_pins.at(a_src_pinned ? Ae.s : Ae.d)),
                collect_candidate_values(
                    t1o.slot, base_pin_src(Ae, a_src_pinned),
                    full_pins.at(a_src_pinned ? Ae.s : Ae.d)));
            const std::vector<std::uint32_t> vals2 = merge_candidates(
                present_values(t2o.slot, base_pin_src(Be, b_src_pinned),
                               full_pins.at(b_src_pinned ? Be.s : Be.d)),
                collect_candidate_values(
                    t2o.slot, base_pin_src(Be, b_src_pinned),
                    full_pins.at(b_src_pinned ? Be.s : Be.d)));
            // The pre-check is only well-defined when every remaining-edge
            // endpoint is pinned or one of the two free endpoints
            // (otherwise fall back to the derive-collect path below).
            bool semi_joinable = true;
            for (std::size_t i = 0; i < pedges.size(); ++i) {
              if (i == oe1 || i == oe2) continue;
              for (const std::uint32_t v : {pedges[i].s, pedges[i].d}) {
                if (!full_pins.count(v) && v != a_free && v != b_free) {
                  semi_joinable = false;
                }
              }
            }
            auto ground_of = [&](std::uint32_t v, std::uint32_t v1,
                                 std::uint32_t v2) {
              if (full_pins.count(v)) return full_pins.at(v);
              return v == a_free ? v1 : v2;
            };
            // Remaining edges grounded by pins+(v1,v2); fills `missing` with
            // the grounded targets absent from the current fact set.
            auto collect_missing = [&](std::uint32_t v1, std::uint32_t v2,
                                       std::vector<Target>* missing) {
              for (std::size_t i = 0; i < pedges.size(); ++i) {
                if (i == oe1 || i == oe2) continue;
                const PE& pe = pedges[i];
                const std::uint32_t s = ground_of(pe.s, v1, v2);
                const std::uint32_t d = ground_of(pe.d, v1, v2);
                const std::uint32_t bs = pe.slot >= g.num_relations()
                                             ? pe.slot - g.num_relations()
                                             : pe.slot;
                const std::uint32_t cs = pe.slot >= g.num_relations() ? d : s;
                const std::uint32_t cd = pe.slot >= g.num_relations() ? s : d;
                if (!state->ContainsEdge(bs, cs, cd)) {
                  missing->push_back(Target{bs, cs, cd});
                }
              }
            };
            bool compatible = false;
            std::uint32_t v1_hit = 0, v2_hit = 0;
            if (semi_joinable) {
              for (const std::uint32_t v1 : vals1) {
                for (const std::uint32_t v2 : vals2) {
                  std::vector<Target> missing;
                  collect_missing(v1, v2, &missing);
                  if (missing.empty()) {
                    compatible = true;
                    v1_hit = v1;
                    v2_hit = v2;
                    break;
                  }
                }
                if (compatible) break;
              }
            }
            // A remaining edge whose predicate has deriving rules may still
            // appear (be derived); only when none can is an incompatible
            // pre-check a sound rejection.
            bool mid_derivable = false;
            for (std::size_t i = 0; i < pedges.size() && !mid_derivable; ++i) {
              if (i == oe1 || i == oe2) continue;
              const gar::LabelID bs = pedges[i].slot >= g.num_relations()
                                          ? pedges[i].slot -
                                                g.num_relations()
                                          : pedges[i].slot;
              if (head_index.find(g.RelationName(bs)) != head_index.end()) {
                mid_derivable = true;
              }
            }
            if (DebugBackward()) {
              std::fprintf(stderr,
                           "[bwd]   df rule=%u vals1=%zu vals2=%zu compat=%d "
                           "mideriv=%d\n",
                           ri, vals1.size(), vals2.size(),
                           static_cast<int>(compatible),
                           static_cast<int>(mid_derivable));
            }
            // Free reject only when BOTH sides produced candidate values,
            // none compose, and no remaining edge can ever appear; an empty
            // side is ambiguous (the value may need a deeper chain).
            if (semi_joinable && !vals1.empty() && !vals2.empty() &&
                !compatible && !mid_derivable) {
              continue;
            }
            // Pin the free end (base orientation) of an open target.
            auto pin_free = [&](Target* tt, const PE& pe,
                                bool pat_src_pinned, std::uint32_t v) {
              const bool inv = pe.slot >= g.num_relations();
              if (pat_src_pinned == inv) {
                tt->src = v;
              } else {
                tt->dst = v;
              }
            };
            const std::uint64_t side_cap =
                std::min<std::uint64_t>(local_cap / 2, 3);
            // Full-pattern verify after the opened edges hold: pinned
            // re-enumeration + one application.
            auto verify_full = [&]() {
              const auto full = build_spec(r);
              std::unordered_map<std::uint32_t, std::uint32_t> fpins;
              if (t.src) fpins[head.pu] = *t.src;
              if (t.dst) fpins[head.pv] = *t.dst;
              std::vector<std::uint32_t> embs2;
              bool capped2 = false;
              const std::uint64_t n2 = matcher.Enumerate(
                  full, cfg.match_cap_per_rule, &embs2, &capped2, fpins);
              for (std::uint64_t k2 = 0; k2 < n2; ++k2) {
                const std::uint32_t* emb2 = embs2.data() + k2 * n;
                if (!label_ok(r, emb2)) continue;
                if (auto d = apply_motif(ri, emb2, n)) {
                  if ((!t.src || d->src == *t.src) &&
                      (!t.dst || d->dst == *t.dst)) {
                    got.emplace_back(d->src, d->dst);
                  }
                  break;
                }
              }
            };
            if (semi_joinable && compatible) {
              if (DebugBackward()) {
                std::fprintf(stderr, "[bwd]   df mode=compat\n");
              }
              // Compatible pair present in the fact set: derive both opened
              // edges pinned to it (the second only if the first holds).
              Target t1 = t1o, t2 = t2o;
              pin_free(&t1, Ae, a_src_pinned, v1_hit);
              pin_free(&t2, Be, b_src_pinned, v2_hit);
              if (!try_target(t1, depth + 1, side_cap).empty() &&
                  !try_target(t2, depth + 1, side_cap).empty()) {
                verify_full();
              }
            } else if (semi_joinable && !vals1.empty() && !vals2.empty()) {
              // No pair composes yet, but the remaining edges may be
              // derivable: walk candidate pairs and semi-join them through
              // the remaining sub-pattern (a missing edge is derived only
              // when a 1-level match for it exists — wrong pairs cost
              // nothing but the free probes). The side with fewer
              // candidates drives the loop; its values come present-first
              // (already in the fact set — free), then from a paid
              // derive-collect.
              const bool collect_side1 = vals1.size() <= vals2.size();
              const PE& CE = collect_side1 ? Ae : Be;
              const bool c_pat_src =
                  collect_side1 ? a_src_pinned : b_src_pinned;
              const bool c_pin_base_src =
                  (c_pat_src != (CE.slot >= g.num_relations()));
              const auto& pvals = collect_side1 ? vals2 : vals1;
              constexpr std::size_t kMaxPairs = 64;
              bool done = false;
              std::unordered_set<std::uint32_t> cseen;
              auto run_pairs = [&](const std::vector<std::uint32_t>& cvals) {
                std::size_t nc = 0;
                for (const std::uint32_t cv : cvals) {
                  if (done || out_of_budget() || ++nc > kMaxPairs) break;
                  if (!cseen.insert(cv).second) continue;
                  std::size_t tried = 0;
                  for (const std::uint32_t pv : pvals) {
                    if (done || out_of_budget() ||
                        ++tried > kMaxPairs) {
                      break;
                    }
                    const std::uint32_t v1 = collect_side1 ? cv : pv;
                    const std::uint32_t v2 = collect_side1 ? pv : cv;
                    std::vector<Target> missing;
                    collect_missing(v1, v2, &missing);
                    bool pair_ok = true;
                    for (const Target& mt : missing) {
                      if (head_index.find(g.RelationName(mt.slot)) ==
                              head_index.end() ||
                          !one_level_derives(mt.slot, *mt.src, *mt.dst) ||
                          try_target(mt, depth + 1, 2).empty()) {
                        pair_ok = false;
                        break;
                      }
                    }
                    if (DebugBackward() &&
                        (pair_ok || (!missing.empty() &&
                                     head_index.find(g.RelationName(
                                         missing.front().slot)) !=
                                        head_index.end() &&
                                     one_level_derives(
                                         missing.front().slot,
                                         *missing.front().src,
                                         *missing.front().dst)))) {
                      std::fprintf(stderr,
                                   "[bwd]   pair v1=%s v2=%s miss=%zu ok=%d\n",
                                   g.EntityName(v1).c_str(),
                                   g.EntityName(v2).c_str(), missing.size(),
                                   static_cast<int>(pair_ok));
                    }
                    if (!pair_ok) continue;
                    Target tp = collect_side1 ? t2o : t1o;
                    pin_free(&tp, collect_side1 ? Be : Ae,
                             collect_side1 ? b_src_pinned : a_src_pinned, pv);
                    if (try_target(tp, depth + 1, side_cap).empty()) {
                      continue;
                    }
                    verify_full();
                    done = !got.empty();
                  }
                }
              };
              // Pass A: values already present under the collect side's pin
              // (free — no derivation needed for that side).
              run_pairs(present_values(
                  collect_side1 ? t1o.slot : t2o.slot,
                  base_pin_src(CE, c_pat_src),
                  full_pins.at(c_pat_src ? CE.s : CE.d)));
              // Pass B: pay for the derive-collect only if the present
              // values composed nothing.
              if (!done && !out_of_budget()) {
                const auto cc = try_target(collect_side1 ? t1o : t2o,
                                           depth + 1, side_cap);
                if (!cc.empty()) {
                  std::vector<std::uint32_t> cvals;
                  for (const auto& pr : cc) {
                    cvals.push_back(c_pin_base_src ? pr.second : pr.first);
                  }
                  run_pairs(cvals);
                }
              }
            } else {
              // A side has no 1-level evidence (or the pre-check is N/A):
              // open derive-collect both sides; the second is skipped when
              // the first yields nothing.
              if (!try_target(t1o, depth + 1, side_cap).empty() &&
                  !try_target(t2o, depth + 1, side_cap).empty()) {
                verify_full();
              }
            }
            if (!got.empty()) break;
            // derive-first failed: fall through to the sub-pattern path
            // (the opened edges may still be derivable with both endpoints
            // grounded by the remaining pattern).
          }
        }
        for (std::size_t i = 0; i < pedges.size(); ++i) {
          if (i == oe1 || (two && i == oe2)) continue;
          has_edge[pedges[i].s] = 1;
          has_edge[pedges[i].d] = 1;
        }
        std::vector<std::uint32_t> old2new(n, UINT32_MAX), new2old;
        bool ok_open = true;
        for (std::uint32_t u = 0; u < n; ++u) {
          if (has_edge[u]) {
            old2new[u] = static_cast<std::uint32_t>(new2old.size());
            new2old.push_back(u);
          } else if (!full_pins.count(u)) {
            ok_open = false;
            break;
          }
        }
        if (!ok_open) continue;
        if (new2old.empty()) {
          // The opened edge was the rule's ONLY edge: nothing to
          // enumerate; the subgoal is determined by the pins (e.g.
          // r(x,y) -> q(x,y) with r missing: derive r's fact directly,
          // then the rule matches trivially). One pin suffices — the
          // other side stays open and is collected from the derivation.
          if (full_pins.empty()) continue;  // need at least one endpoint
          const PE& pe = pedges[oe1];
          Target t1;
          t1.slot = pe.slot >= g.num_relations() ? pe.slot - g.num_relations()
                                                 : pe.slot;
          const auto ps_it = full_pins.find(pe.s);
          const auto pd_it = full_pins.find(pe.d);
          std::optional<std::uint32_t> a, b;
          if (ps_it != full_pins.end()) a = ps_it->second;
          if (pd_it != full_pins.end()) b = pd_it->second;
          if (pe.slot >= g.num_relations()) std::swap(a, b);
          t1.src = a;
          t1.dst = b;
          ++spent;  // the subgoal attempt charges one application
          if (try_target(t1, depth + 1, std::min<std::uint64_t>(local_cap / 2, 3)).empty()) continue;
          const auto full = build_spec(r);
          std::unordered_map<std::uint32_t, std::uint32_t> fpins;
          if (t.src) fpins[head.pu] = *t.src;
          if (t.dst) fpins[head.pv] = *t.dst;
          std::vector<std::uint32_t> embs2;
          bool capped2 = false;
          const std::uint64_t n2 = matcher.Enumerate(
              full, cfg.match_cap_per_rule, &embs2, &capped2, fpins);
          for (std::uint64_t k2 = 0; k2 < n2; ++k2) {
            const std::uint32_t* emb2 = embs2.data() + k2 * n;
            if (!label_ok(r, emb2)) continue;
            if (auto d = apply_motif(ri, emb2, n)) {
              if ((!t.src || d->src == *t.src) &&
                  (!t.dst || d->dst == *t.dst)) {
                got.emplace_back(d->src, d->dst);
              }
              break;
            }
          }
          continue;
        }
        graph::PatternSpec sub;
        sub.num_vertices = static_cast<std::uint32_t>(new2old.size());
        for (std::size_t i = 0; i < pedges.size(); ++i) {
          if (i == oe1 || (two && i == oe2)) continue;
          sub.edges.push_back({old2new[pedges[i].s],
                               old2new[pedges[i].d], pedges[i].slot});
        }
        std::unordered_map<std::uint32_t, std::uint32_t> pins;
        for (const auto& [v, w] : full_pins) {
          if (old2new[v] != UINT32_MAX) pins[old2new[v]] = w;
        }
        std::vector<std::uint32_t> embs;
        bool capped = false;
        const std::uint64_t n_emb = matcher.Enumerate(
            sub, cfg.match_cap_per_rule, &embs, &capped, pins);
        const auto sub_n = static_cast<std::uint32_t>(new2old.size());

        for (std::uint64_t k = 0; k < n_emb && got.empty() &&
             !out_of_budget(); ++k) {
          const std::uint32_t* emb_sub = embs.data() + k * sub_n;
          std::vector<std::uint32_t> emb(n, 0);
          for (std::uint32_t u = 0; u < sub_n; ++u) {
            emb[new2old[u]] = emb_sub[u];
          }
          for (const auto& [v, w] : full_pins) emb[v] = w;

          // Ground the opened edges; a shared free variable stays open on
          // one side and is collected by deriving the other side first.
          auto ground_target = [&](gar::EdgeIndex oe) -> Target {
            const PE& pe = pedges[oe];
            Target tt;
            tt.slot = pe.slot >= g.num_relations()
                          ? pe.slot - g.num_relations()
                          : pe.slot;
            std::uint32_t a = emb[pe.s], b = emb[pe.d];
            if (pe.slot >= g.num_relations()) std::swap(a, b);
            tt.src = a;
            tt.dst = b;
            return tt;
          };
          Target t1 = ground_target(oe1);
          bool ok = false;
          if (!two) {
            ok = !try_target(t1, depth + 1, std::min<std::uint64_t>(local_cap / 2, 3)).empty();
          } else {
            // Two opened edges share one variable. The shared variable is
            // free iff unpinned and uncovered by the sub-pattern.
            const PE& A = pedges[oe1];
            const PE& B = pedges[oe2];
            std::uint32_t shared = UINT32_MAX;
            for (const std::uint32_t v : {A.s, A.d}) {
              if (v == B.s || v == B.d) shared = v;
            }
            const bool shared_free =
                shared != UINT32_MAX && !full_pins.count(shared) &&
                !has_edge[shared];
            if (!shared_free) {
              // Gate the charged recursive derivations with free 1-step
              // probes: a grounded opened edge with no current presence and
              // no 1-level match would only thrash the recursion budget.
              auto probe_ok = [&](const Target& tt) {
                if (!tt.src || !tt.dst) return true;  // open side: keep
                if (state->ContainsEdge(tt.slot, *tt.src, *tt.dst)) {
                  return true;
                }
                return one_level_derives(tt.slot, *tt.src, *tt.dst);
              };
              const Target t2g = ground_target(oe2);
              ok = probe_ok(t1) && probe_ok(t2g) &&
                   !try_target(t1, depth + 1, std::min<std::uint64_t>(local_cap / 2, 3)).empty() &&
                   !try_target(t2g, depth + 1, std::min<std::uint64_t>(local_cap / 2, 3)).empty();
            } else {
              // Collect candidate values for the shared variable by
              // deriving the first edge with that side open.
              Target open1;
              open1.slot = t1.slot;
              const bool share_is_src_of_1 = A.s == shared;
              if (share_is_src_of_1) {
                open1.dst = t1.dst;  // free src
              } else {
                open1.src = t1.src;  // free dst
              }
              const auto cands = try_target(open1, depth + 1, std::min<std::uint64_t>(local_cap / 2, 3));
              for (const auto& [fs, fd] : cands) {
                const std::uint32_t shared_val =
                    share_is_src_of_1 ? fs : fd;
                Target t2 = ground_target(oe2);
                if (B.s == shared) t2.src = shared_val;
                if (B.d == shared) t2.dst = shared_val;
                if (!try_target(t2, depth + 1, std::min<std::uint64_t>(local_cap / 2, 3)).empty()) {
                  ok = true;
                  break;
                }
              }
            }
          }
          if (!ok) continue;

          // The opened edges now hold: full pinned match + apply.
          const auto full = build_spec(r);
          std::unordered_map<std::uint32_t, std::uint32_t> fpins;
          if (t.src) fpins[head.pu] = *t.src;
          if (t.dst) fpins[head.pv] = *t.dst;
          std::vector<std::uint32_t> embs2;
          bool capped2 = false;
          const std::uint64_t n2 = matcher.Enumerate(
              full, cfg.match_cap_per_rule, &embs2, &capped2, fpins);
          for (std::uint64_t k2 = 0; k2 < n2; ++k2) {
            const std::uint32_t* emb2 = embs2.data() + k2 * n;
            if (!label_ok(r, emb2)) continue;
            if (auto d = apply_motif(ri, emb2, n)) {
              if ((!t.src || d->src == *t.src) && (!t.dst || d->dst == *t.dst)) {
                got.emplace_back(d->src, d->dst);
              }
              break;
            }
          }
        }
      }
    }
    if (DebugBackward()) log_att_result();  // the last executed attempt
    if (DebugBackward()) {
      std::fprintf(stderr, "[bwd] exit %s(%s,%s) -> %zu\n", pred_name.c_str(),
                   t.src ? g.EntityName(*t.src).c_str() : "_",
                   t.dst ? g.EntityName(*t.dst).c_str() : "_", got.size());
    }
    if (got.empty()) failed_targets[TargetKey(t)] = state->version();
    active.erase(TargetKey(t));
    return got;
  };

  // Same upfront check as the other executors.
  if (goal.args[0] && goal.args[1] &&
      state->ContainsEdge(goal.slot, *goal.args[0], *goal.args[1])) {
    res.goal_derived = true;
    res.termination = "goal_already_in_graph";
    return res;
  }
  const Target top{goal.slot, goal.args[0], goal.args[1]};
  std::vector<std::pair<std::uint32_t, std::uint32_t>> got;
  if (cfg.backward_full_enum && (!goal.args[0] || !goal.args[1])) {
    // Full-extension loop: each round is one try_target scan (pass 1, with
    // the open-edge fallback when pass 1 finds nothing new). Between rounds
    // the graph has grown, re-admitting memoized failures and enabling
    // fresh matches; stop at the journal fixpoint or on budget.
    std::unordered_set<std::uint64_t> seen;
    for (;;) {
      const std::size_t j0 = state->journal().size();
      for (auto& p : try_target(top, 0, cfg.budget)) {
        if (seen.insert(graph::PackPair(p.first, p.second)).second) {
          got.push_back(p);
        }
      }
      if (state->journal().size() == j0 || spent >= cfg.budget ||
          wall_exceeded()) {
        break;
      }
    }
    res.goal_derived = !got.empty();
    res.termination = wall_exceeded()   ? "timeout"
                      : spent >= cfg.budget ? "budget"
                                            : "full_enum_fixpoint";
  } else {
    got = try_target(top, 0, cfg.budget);
    if (!got.empty()) {
      res.goal_derived = true;
      res.termination = "goal";
    } else {
      res.termination =
          spent >= cfg.budget ? "budget" : "no_candidates";
    }
  }
  res.total_cost = static_cast<double>(spent);
  res.budget_exhausted = spent >= cfg.budget && !res.goal_derived;
  res.derived = state->journal();
  std::fprintf(stderr, "[backward] done: goal=%s spent=%llu derived=%zu\n",
               res.goal_derived ? "true" : "false",
               static_cast<unsigned long long>(spent), res.derived.size());
  return res;
}

}  // namespace agent
}  // namespace engine
}  // namespace gar
