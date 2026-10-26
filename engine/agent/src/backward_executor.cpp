#include "apply_helpers.h"
#include "gar/engine/agent/executor.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <unordered_map>
#include <unordered_set>

#include "gar/graph/pattern_matcher.h"
#include "gar/graph/predicate_evaluator.h"

// BackwardExecutor: goal-directed backward chaining (see executor.h for the
// design rationale). Per target fact: pinned enumeration of the rules whose
// consequent names the target's predicate (the candidate space collapses to
// motifs that can derive the goal), antecedent evaluation with the chase's
// exact semantics, one-level open-edge subgoals when a pattern edge of the
// target rule is missing (the edge is derivable but absent, e.g. a
// round-2 intermediate), recursive Derive on the subgoal. Every antecedent
// evaluation is charged one unit (paper: an application pays for testing
// its preconditions). Facts only accumulate; soundness by construction.

namespace gar {
namespace engine {
namespace agent {

namespace {

// A ground (or partially ground) edge fact to derive.
struct Target {
  std::uint32_t slot;
  std::uint32_t src, dst;
  bool pin_src, pin_dst;  // false = open side (any derived value satisfies)
};

std::uint64_t TargetKey(std::uint32_t slot, std::uint32_t src,
                        std::uint32_t dst) {
  return (static_cast<std::uint64_t>(slot) << 40) ^ graph::PackPair(src, dst);
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

  // Derive `t` (returns true when the fact holds afterwards, whether freshly
  // derived or already present). See the file-top comment for the algorithm.
  std::unordered_set<std::uint64_t> active;  // recursion cycle guard
  std::function<bool(const Target&, std::uint32_t)> derive =
      [&](const Target& t, std::uint32_t depth) -> bool {
    if (t.pin_src && t.pin_dst &&
        state->ContainsEdge(t.slot, t.src, t.dst)) {
      return true;  // already present: premise satisfied for free
    }
    if (spent >= cfg.budget || depth > cfg.max_subgoal_depth) return false;
    const std::uint64_t tkey = TargetKey(t.slot, t.src, t.dst);
    if (!active.insert(tkey).second) return false;  // cycle on this path

    const std::string& pred_name = g.RelationName(t.slot);
    const auto it = head_index.find(pred_name);
    bool done = false;
    if (it != head_index.end()) {
      // Pinned candidates from every rule deriving this predicate.
      std::vector<Motif> mus;
      for (const std::uint32_t ri : it->second) {
        const GARule& r = rules[ri];
        const RuleHead head = ResolveRuleHead(r);
        std::unordered_map<std::uint32_t, std::uint32_t> pins;
        if (t.pin_src) pins[head.pu] = t.src;
        if (t.pin_dst) pins[head.pv] = t.dst;
        graph::PatternSpec spec;
        spec.num_vertices =
            static_cast<std::uint32_t>(r.pattern.num_vertices());
        for (std::uint32_t s = 0; s < spec.num_vertices; ++s) {
          for (gar::EdgeIndex e = r.pattern.offsets[s];
               e < r.pattern.offsets[s + 1]; ++e) {
            spec.edges.push_back(
                {s, r.pattern.indices[e], r.pattern.edge_labels[e]});
          }
        }
        const graph::PatternMatcher matcher(g);
        std::vector<std::uint32_t> embs;
        bool capped = false;
        const std::uint64_t n_emb = matcher.Enumerate(
            spec, cfg.match_cap_per_rule, &embs, &capped, pins);
        const std::uint32_t n = spec.num_vertices;
        const bool typed = g.has_vertex_labels();
        for (std::uint64_t k = 0; k < n_emb; ++k) {
          const std::uint32_t* emb = embs.data() + k * n;
          bool label_ok = true;
          for (std::uint32_t u = 0; u < n; ++u) {
            if (typed && u < r.node_labels.size() &&
                r.node_labels[u] != gar::kWildcardLabel &&
                g.VertexLabel(emb[u]) != r.node_labels[u]) {
              label_ok = false;
              break;
            }
          }
          if (label_ok) {
            mus.push_back(Motif{ri, std::vector<std::uint32_t>(emb, emb + n)});
          }
        }
      }
      // Rank by the scorer (rel_fam / rel_evd / confidence), deterministic
      // order: total desc, ties by rule index then embedding.
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

      for (const std::size_t oi : ord) {
        if (spent >= cfg.budget) break;
        const Motif& mu = scored[oi].motif;
        const GARule& r = rules[mu.rule];
        const std::uint32_t* emb = mu.embedding.data();
        const std::uint32_t n =
            static_cast<std::uint32_t>(r.pattern.num_vertices());
        const std::uint64_t mkey = MotifHash(mu.rule, emb, n);
        if (failures->IsRetired(mkey, state->version())) continue;
        const RuleHead head = ResolveRuleHead(r);

        ++spent;  // one application: evaluating the antecedent under h
        AgentStep step;
        step.motif = mu;
        step.cost = kit.cost_model->RuleLowerBound(r);  // unit: 1

        std::vector<Fact> premises;
        GroundPremises(r, mu, g, *ctx.table, &premises);
        if (graph::EvalAntecedent(r.antecedent, emb, state->attrs(), g)) {
          const graph::DerivedEdge d{
              emb[head.pu], emb[head.pv], head.slot, mu.rule,
              static_cast<std::uint32_t>(res.chain.size() + 1)};
          state->ApplyDerivation(d);
          step.success = true;
          step.derived = d;
          for (const auto& f : premises) {
            witness->WitnessFact(f, ctx.encoder);
          }
          witness->WitnessFact(MakeEdgeFact(d.slot, d.src, d.dst),
                               ctx.encoder);
          res.chain.push_back(std::move(step));
          done = true;
          break;
        }
        // Failed: classify. A dynamic blocking edge predicate (derivable
        // but absent) becomes a subgoal; recurse once, then retry.
        const FailureInfo fi =
            ClassifyFailure(r, emb, state->attrs(), g, ctx);
        bool repaired = false;
        if (fi.dynamic && fi.fail_idx < r.antecedent.size()) {
          const gar::Predicate& pred = r.antecedent[fi.fail_idx];
          Target sub{pred.label >= g.num_relations()
                         ? pred.label - g.num_relations()
                         : pred.label,
                     0, 0, true, true};
          if (pred.label >= g.num_relations()) {  // inverse: swap endpoints
            sub.src = emb[pred.dst];
            sub.dst = emb[pred.src];
          } else {
            sub.src = emb[pred.src];
            sub.dst = emb[pred.dst];
          }
          if (derive(sub, depth + 1)) {
            if (graph::EvalAntecedent(r.antecedent, emb, state->attrs(), g)) {
              const graph::DerivedEdge d{
                  emb[head.pu], emb[head.pv], head.slot, mu.rule,
                  static_cast<std::uint32_t>(res.chain.size() + 1)};
              state->ApplyDerivation(d);
              step.success = true;
              step.derived = d;
              for (const auto& f : premises) {
                witness->WitnessFact(f, ctx.encoder);
              }
              witness->WitnessFact(MakeEdgeFact(d.slot, d.src, d.dst),
                                   ctx.encoder);
              res.chain.push_back(std::move(step));
              done = true;
              repaired = true;
              break;
            }
          }
        }
        if (!repaired) {
          failures->RecordFailure(mkey, fi.dynamic, state->version());
          const std::size_t n_pattern = r.pattern.num_edges();
          const std::size_t upto = fi.fail_idx < r.antecedent.size()
                                       ? n_pattern + fi.fail_idx + 1
                                       : premises.size();
          for (std::size_t i = 0; i < upto && i < premises.size(); ++i) {
            witness->WitnessFact(premises[i], ctx.encoder);
          }
          res.chain.push_back(std::move(step));
        }
      }
    }

    // Open-edge fallback (the depth>1 case): the full pinned match found
    // nothing because a pattern edge is missing. When the target is still
    // underived and the budget allows, open ONE pattern edge whose slot is
    // derivable, enumerate the rest, and recurse on the opened ground edge.
    // (v1: at most one open edge per rule; the remaining sub-pattern must
    // stay connected and cover every vertex.)
    if (!done && depth < cfg.max_subgoal_depth && spent < cfg.budget &&
        it != head_index.end()) {
      for (const std::uint32_t ri : it->second) {
        if (done || spent >= cfg.budget) break;
        const GARule& r = rules[ri];
        const RuleHead head = ResolveRuleHead(r);
        const auto n = static_cast<std::uint32_t>(r.pattern.num_vertices());
        for (gar::EdgeIndex open_e = 0;
             open_e < r.pattern.num_edges() && !done; ++open_e) {
          std::uint32_t es = 0, ed = 0;
          gar::LabelID eslot = 0;
          // locate the opened edge's (src, dst, slot)
          {
            std::uint32_t idx = 0;
            for (std::uint32_t s = 0; s < n; ++s) {
              for (gar::EdgeIndex e = r.pattern.offsets[s];
                   e < r.pattern.offsets[s + 1]; ++e, ++idx) {
                if (idx == open_e) {
                  es = s;
                  ed = r.pattern.indices[e];
                  eslot = r.pattern.edge_labels[e];
                }
              }
            }
          }
          // The opened slot must be derivable (some rule derives its base
          // relation) — otherwise the subgoal can never be produced.
          const std::uint32_t base_slot =
              eslot >= g.num_relations() ? eslot - g.num_relations() : eslot;
          const std::string& sub_name = g.RelationName(base_slot);
          if (head_index.find(sub_name) == head_index.end()) continue;

          // Sub-pattern without the opened edge. The matcher returns 0 for
          // disconnected patterns, and a vertex isolated by the opening has
          // no back edge at all — so drop isolated vertices that carry a
          // pin (their value is fixed by the pin) and skip the opening when
          // an unpinned vertex is isolated (undetermined in v1). Remaining
          // vertices are remapped to dense ids; the kept edges must form
          // one connected component (v1: otherwise skip).
          std::vector<char> has_edge(n, 0);
          {
            std::uint32_t idx = 0;
            for (std::uint32_t s = 0; s < n; ++s) {
              for (gar::EdgeIndex e = r.pattern.offsets[s];
                   e < r.pattern.offsets[s + 1]; ++e, ++idx) {
                if (idx == open_e) continue;
                has_edge[s] = 1;
                has_edge[r.pattern.indices[e]] = 1;
              }
            }
          }
          std::unordered_map<std::uint32_t, std::uint32_t> full_pins;
          if (t.pin_src) full_pins[head.pu] = t.src;
          if (t.pin_dst) full_pins[head.pv] = t.dst;

          std::vector<std::uint32_t> old2new(n, UINT32_MAX), new2old;
          bool ok_open = true;
          for (std::uint32_t u = 0; u < n; ++u) {
            if (has_edge[u]) {
              old2new[u] = static_cast<std::uint32_t>(new2old.size());
              new2old.push_back(u);
            } else if (full_pins.count(u)) {
              continue;  // pinned + isolated: value fixed by the pin
            } else {
              ok_open = false;  // unpinned and isolated: undetermined
              break;
            }
          }
          if (!ok_open || new2old.empty()) continue;

          graph::PatternSpec sub;
          sub.num_vertices = static_cast<std::uint32_t>(new2old.size());
          {
            std::uint32_t idx = 0;
            for (std::uint32_t s = 0; s < n; ++s) {
              for (gar::EdgeIndex e = r.pattern.offsets[s];
                   e < r.pattern.offsets[s + 1]; ++e, ++idx) {
                if (idx == open_e) continue;
                sub.edges.push_back({old2new[s], old2new[r.pattern.indices[e]],
                                     r.pattern.edge_labels[e]});
              }
            }
          }
          std::unordered_map<std::uint32_t, std::uint32_t> pins;
          for (const auto& [v, w] : full_pins) {
            if (old2new[v] != UINT32_MAX) pins[old2new[v]] = w;
          }

          const graph::PatternMatcher matcher(g);
          std::vector<std::uint32_t> embs;
          bool capped = false;
          const std::uint64_t n_emb = matcher.Enumerate(
              sub, cfg.match_cap_per_rule, &embs, &capped, pins);
          const auto sub_n = static_cast<std::uint32_t>(new2old.size());
          for (std::uint64_t k = 0; k < n_emb && !done; ++k) {
            const std::uint32_t* emb_sub = embs.data() + k * sub_n;
            // Expand back to a full embedding (pins fill dropped vertices).
            std::vector<std::uint32_t> emb(n, 0);
            for (std::uint32_t u = 0; u < sub_n; ++u) {
              emb[new2old[u]] = emb_sub[u];
            }
            for (const auto& [v, w] : full_pins) emb[v] = w;
            Target sub_t{base_slot, emb[es], emb[ed], true, true};
            if (eslot >= g.num_relations()) {
              sub_t.src = emb[ed];  // inverse slot: the base edge is flipped
              sub_t.dst = emb[es];
            }
            if (state->ContainsEdge(base_slot, sub_t.src, sub_t.dst)) {
              continue;  // premise already present (enumeration artifact)
            }
            ++spent;  // the subgoal attempt charges one application
            if (derive(sub_t, depth + 1)) {
              // The opened edge now exists: full pinned match + antecedent.
              std::unordered_map<std::uint32_t, std::uint32_t> fpins;
              if (t.pin_src) fpins[head.pu] = t.src;
              if (t.pin_dst) fpins[head.pv] = t.dst;
              graph::PatternSpec full;
              full.num_vertices = n;
              for (std::uint32_t s = 0; s < n; ++s) {
                for (gar::EdgeIndex e = r.pattern.offsets[s];
                     e < r.pattern.offsets[s + 1]; ++e) {
                  full.edges.push_back({s, r.pattern.indices[e],
                                        r.pattern.edge_labels[e]});
                }
              }
              std::vector<std::uint32_t> embs2;
              bool capped2 = false;
              const std::uint64_t n2 = matcher.Enumerate(
                  full, cfg.match_cap_per_rule, &embs2, &capped2, fpins);
              for (std::uint64_t k2 = 0; k2 < n2 && !done; ++k2) {
                const std::uint32_t* emb2 = embs2.data() + k2 * n;
                ++spent;
                AgentStep step;
                step.motif = Motif{ri, std::vector<std::uint32_t>(emb2,
                                                                  emb2 + n)};
                step.cost = kit.cost_model->RuleLowerBound(r);
                if (graph::EvalAntecedent(r.antecedent, emb2,
                                          state->attrs(), g)) {
                  const graph::DerivedEdge d{
                      emb2[head.pu], emb2[head.pv], head.slot, ri,
                      static_cast<std::uint32_t>(res.chain.size() + 1)};
                  state->ApplyDerivation(d);
                  step.success = true;
                  step.derived = d;
                  std::vector<Fact> premises;
                  GroundPremises(r, step.motif, g, *ctx.table, &premises);
                  for (const auto& f : premises) {
                    witness->WitnessFact(f, ctx.encoder);
                  }
                  witness->WitnessFact(MakeEdgeFact(d.slot, d.src, d.dst),
                                       ctx.encoder);
                  res.chain.push_back(std::move(step));
                  done = true;
                  break;
                }
                res.chain.push_back(std::move(step));
              }
            }
          }
        }
      }
    }
    active.erase(tkey);
    return done || (t.pin_src && t.pin_dst &&
                    state->ContainsEdge(t.slot, t.src, t.dst));
  };

  Target top{goal.slot, goal.args[0] ? *goal.args[0] : 0,
             goal.args[1] ? *goal.args[1] : 0,
             goal.args[0].has_value(), goal.args[1].has_value()};
  // Same upfront check as the other executors.
  if (top.pin_src && top.pin_dst &&
      state->ContainsEdge(top.slot, top.src, top.dst)) {
    res.goal_derived = true;
    res.termination = "goal_already_in_graph";
    return res;
  }
  res.termination = "no_candidates";
  if (derive(top, 0)) {
    res.goal_derived = true;
    res.termination = "goal";
  }
  res.total_cost = static_cast<double>(spent);
  res.budget_exhausted = spent >= cfg.budget && !res.goal_derived;
  if (!res.goal_derived && spent >= cfg.budget) res.termination = "budget";
  res.derived = state->journal();
  std::fprintf(stderr, "[backward] done: goal=%s spent=%llu derived=%zu\n",
               res.goal_derived ? "true" : "false",
               static_cast<unsigned long long>(spent), res.derived.size());
  return res;
}

}  // namespace agent
}  // namespace engine
}  // namespace gar
