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
#include <cstdlib>
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

// Trace control: GAR_DEBUG_BACKWARD=1 prints target entry/exit (debugging
// only; off by default).
bool DebugBackward() {
  static const bool on = std::getenv("GAR_DEBUG_BACKWARD") != nullptr;
  return on;
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
      return spent >= cfg.budget || spent >= local_limit;
    };
    if (out_of_budget() || depth > cfg.max_subgoal_depth) return got;
    if (!active.insert(TargetKey(t)).second) return got;  // cycle

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
    for (const std::size_t oi : ord) {
      if (out_of_budget()) break;
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
    if (!got.empty() || depth >= cfg.max_subgoal_depth || out_of_budget()) {
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
      double rel_fam;
      double conf;
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
      for (const auto& [oe1, oe2] : open_sets) {
        std::uint32_t od = open_d(oe1);
        bool df = false;
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
          }
        }
        attempts.push_back(OpenAttempt{ri, oe1, oe2, od, df, probe.rel_fam,
                                       r.quality.confidence});
      }
    }
    std::sort(attempts.begin(), attempts.end(),
              [](const OpenAttempt& a, const OpenAttempt& b) {
                // derive-first-eligible openings first (coordinated
                // intermediates are the rare winning shape), then by the
                // opened edges' distance to the goal, then rule quality.
                if (a.derive_first != b.derive_first) return a.derive_first;
                if (a.open_d != b.open_d) return a.open_d < b.open_d;
                if (a.rel_fam != b.rel_fam) return a.rel_fam > b.rel_fam;
                if (a.conf != b.conf) return a.conf > b.conf;
                if (a.rule != b.rule) return a.rule < b.rule;
                if (a.oe1 != b.oe1) return a.oe1 < b.oe1;
                return a.oe2 < b.oe2;
              });

    for (const auto& att : attempts) {
      if (!got.empty() || out_of_budget()) break;
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
            const auto c1 =
                try_target(open_target(A, a_src_pinned), depth + 1,
                           local_cap / 2);
            const auto c2 =
                try_target(open_target(B, b_src_pinned), depth + 1,
                           local_cap / 2);
            if (!c1.empty() && !c2.empty()) {
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
          if (try_target(t1, depth + 1, local_cap / 2).empty()) continue;
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
            ok = !try_target(t1, depth + 1, local_cap / 2).empty();
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
              ok = !try_target(t1, depth + 1, local_cap / 2).empty() &&
                   !try_target(ground_target(oe2), depth + 1, local_cap / 2).empty();
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
              const auto cands = try_target(open1, depth + 1, local_cap / 2);
              for (const auto& [fs, fd] : cands) {
                const std::uint32_t shared_val =
                    share_is_src_of_1 ? fs : fd;
                Target t2 = ground_target(oe2);
                if (B.s == shared) t2.src = shared_val;
                if (B.d == shared) t2.dst = shared_val;
                if (!try_target(t2, depth + 1, local_cap / 2).empty()) {
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
    if (DebugBackward()) {
      std::fprintf(stderr, "[bwd] exit %s(%s,%s) -> %zu\n", pred_name.c_str(),
                   t.src ? g.EntityName(*t.src).c_str() : "_",
                   t.dst ? g.EntityName(*t.dst).c_str() : "_", got.size());
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
  const auto got = try_target(top, 0, cfg.budget);
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
