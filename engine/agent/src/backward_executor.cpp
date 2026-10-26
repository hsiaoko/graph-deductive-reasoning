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
#include <cstdio>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "apply_helpers.h"
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
  constexpr std::size_t kMaxCollected = 8;  // distinct facts per open target
  std::unordered_set<std::uint64_t> active;  // recursion cycle guard

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

  // TryTarget: make facts matching `t` hold (derive them if derivable).
  // Returns the (capped) set of (src,dst) pairs now present.
  //   - both sides pinned: present -> {it}; else derive.
  //   - one side open: derive up to kMaxCollected distinct facts.
  std::function<std::vector<std::pair<std::uint32_t, std::uint32_t>>(
      const Target&, std::uint32_t)>
      try_target = [&](const Target& t, std::uint32_t depth)
      -> std::vector<std::pair<std::uint32_t, std::uint32_t>> {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> got;
    if (t.src && t.dst && state->ContainsEdge(t.slot, *t.src, *t.dst)) {
      return {{*t.src, *t.dst}};  // already present: premise holds for free
    }
    if (spent >= cfg.budget || depth > cfg.max_subgoal_depth) return got;
    if (!active.insert(TargetKey(t)).second) return got;  // cycle

    const std::string& pred_name = g.RelationName(t.slot);
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
    for (const std::size_t oi : ord) {
      if (spent >= cfg.budget) break;
      if (got.size() >= kMaxCollected) break;
      const Motif& mu = scored[oi].motif;
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
            depth < cfg.max_subgoal_depth && spent < cfg.budget) {
          const gar::Predicate& pred = r.antecedent[fi.fail_idx];
          Target sub;
          if (pred.label >= g.num_relations()) {
            sub = Target{pred.label - g.num_relations(),
                         m2.embedding[pred.dst], m2.embedding[pred.src]};
          } else {
            sub = Target{pred.label, m2.embedding[pred.src],
                         m2.embedding[pred.dst]};
          }
          if (!try_target(sub, depth + 1).empty()) {
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
    if (!got.empty() || depth >= cfg.max_subgoal_depth || spent >= cfg.budget) {
      active.erase(TargetKey(t));
      return got;
    }

    // ---- pass 2: open-edge subgoals (pattern edges missing from the
    // current graph but derivable). At most two opened edges per rule;
    // two opened edges must share exactly one variable (the free join
    // variable gets candidate values from deriving one side first).
    for (const std::uint32_t ri : it->second) {
      if (!got.empty() || spent >= cfg.budget) break;
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
      // Candidate open sets: single edge, or edge pairs sharing a variable.
      std::vector<std::pair<gar::EdgeIndex, gar::EdgeIndex>> open_sets;
      for (gar::EdgeIndex e = 0; e < m_edges; ++e) {
        open_sets.emplace_back(e, gar::EdgeIndex(UINT32_MAX));
      }
      for (gar::EdgeIndex e1 = 0; e1 < m_edges; ++e1) {
        for (gar::EdgeIndex e2 = e1 + 1; e2 < m_edges; ++e2) {
          const PE& a = pedges[e1];
          const PE& b = pedges[e2];
          if (a.s == b.s || a.s == b.d || a.d == b.s || a.d == b.d) {
            open_sets.emplace_back(e1, e2);
          }
        }
      }

      for (const auto& [oe1, oe2] : open_sets) {
        if (!got.empty() || spent >= cfg.budget) break;
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
        if (!ok_open || new2old.empty()) continue;
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

        for (std::uint64_t k = 0; k < n_emb && got.empty(); ++k) {
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
            ok = !try_target(t1, depth + 1).empty();
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
              ok = !try_target(t1, depth + 1).empty() &&
                   !try_target(ground_target(oe2), depth + 1).empty();
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
              const auto cands = try_target(open1, depth + 1);
              for (const auto& [fs, fd] : cands) {
                const std::uint32_t shared_val =
                    share_is_src_of_1 ? fs : fd;
                Target t2 = ground_target(oe2);
                if (B.s == shared) t2.src = shared_val;
                if (B.d == shared) t2.dst = shared_val;
                if (!try_target(t2, depth + 1).empty()) {
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
  const auto got = try_target(top, 0);
  if (!got.empty()) {
    res.goal_derived = true;
    res.termination = "goal";
  } else {
    res.termination =
        spent >= cfg.budget ? "budget" : "no_candidates";
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
